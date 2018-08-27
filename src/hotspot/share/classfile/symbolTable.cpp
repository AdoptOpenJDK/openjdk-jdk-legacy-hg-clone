/*
 * Copyright (c) 1997, 2018, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#include "precompiled.hpp"
#include "classfile/altHashing.hpp"
#include "classfile/compactHashtable.inline.hpp"
#include "classfile/javaClasses.hpp"
#include "classfile/symbolTable.hpp"
#include "memory/allocation.inline.hpp"
#include "memory/metaspaceClosure.hpp"
#include "memory/resourceArea.hpp"
#include "oops/oop.inline.hpp"
#include "runtime/atomic.hpp"
#include "runtime/interfaceSupport.inline.hpp"
#include "runtime/timerTrace.hpp"
#include "services/diagnosticCommand.hpp"
#include "utilities/concurrentHashTable.inline.hpp"
#include "utilities/concurrentHashTableTasks.inline.hpp"

// We used to not resize at all, so let's be conservative
// and not set it too short before we decide to resize,
// to match previous startup behavior
#define PREF_AVG_LIST_LEN           8
// 2^17 (131,072) is max size, which is about 6.5 times as large
// as the previous table size (used to be 20,011),
// which never resized
#define END_SIZE                    17
// If a chain gets to 100 something might be wrong
#define REHASH_LEN                  100
// We only get a chance to check whether we need
// to clean infrequently (on class unloading),
// so if we have even one dead entry then mark table for cleaning
#define CLEAN_DEAD_HIGH_WATER_MARK  0.0

#define ON_STACK_BUFFER_LENGTH 128

// --------------------------------------------------------------------------
SymbolTable* SymbolTable::_the_table = NULL;
CompactHashtable<Symbol*, char> SymbolTable::_shared_table;
volatile bool SymbolTable::_alt_hash = false;
volatile bool SymbolTable::_lookup_shared_first = false;
// Static arena for symbols that are not deallocated
Arena* SymbolTable::_arena = NULL;

static juint murmur_seed = 0;

static inline void log_trace_symboltable_helper(Symbol* sym, const char* msg) {
#ifndef PRODUCT
  ResourceMark rm;
  log_trace(symboltable)("%s [%s]", msg, sym->as_quoted_ascii());
#endif // PRODUCT
}

// Pick hashing algorithm.
static uintx hash_symbol(const char* s, int len, bool useAlt) {
  return useAlt ?
  AltHashing::murmur3_32(murmur_seed, (const jbyte*)s, len) :
  java_lang_String::hash_code((const jbyte*)s, len);
}

static uintx hash_shared_symbol(const char* s, int len) {
  return java_lang_String::hash_code((const jbyte*)s, len);
}

class SymbolTableConfig : public SymbolTableHash::BaseConfig {
private:
public:
  static uintx get_hash(Symbol* const& value, bool* is_dead) {
    *is_dead = (value->refcount() == 0);
    if (*is_dead) {
      return 0;
    } else {
      return hash_symbol((const char*)value->bytes(), value->utf8_length(), SymbolTable::_alt_hash);
    }
  }
  // We use default allocation/deallocation but counted
  static void* allocate_node(size_t size, Symbol* const& value) {
    SymbolTable::item_added();
    return SymbolTableHash::BaseConfig::allocate_node(size, value);
  }
  static void free_node(void* memory, Symbol* const& value) {
    // We get here either because #1 some threads lost a race
    // to insert a newly created Symbol, or #2 we are freeing
    // a symbol during normal cleanup deletion.
    // If #1, then the symbol can be a permanent (refcount==PERM_REFCOUNT),
    // or regular newly created one but with refcount==0 (see SymbolTableCreateEntry)
    // If #2, then the symbol must have refcount==0
    assert((value->refcount() == PERM_REFCOUNT) || (value->refcount() == 0),
           "refcount %d", value->refcount());
    SymbolTable::delete_symbol(value);
    SymbolTableHash::BaseConfig::free_node(memory, value);
    SymbolTable::item_removed();
  }
};

static size_t ceil_log2(size_t value) {
  size_t ret;
  for (ret = 1; ((size_t)1 << ret) < value; ++ret);
  return ret;
}

SymbolTable::SymbolTable() :
  _symbols_removed(0), _symbols_counted(0), _local_table(NULL),
  _current_size(0), _has_work(0), _needs_rehashing(false),
  _items_count(0), _uncleaned_items_count(0) {

  size_t start_size_log_2 = ceil_log2(SymbolTableSize);
  _current_size = ((size_t)1) << start_size_log_2;
  log_trace(symboltable)("Start size: " SIZE_FORMAT " (" SIZE_FORMAT ")",
                         _current_size, start_size_log_2);
  _local_table = new SymbolTableHash(start_size_log_2, END_SIZE, REHASH_LEN);
}

void SymbolTable::delete_symbol(Symbol* sym) {
  if (sym->refcount() == PERM_REFCOUNT) {
    MutexLockerEx ml(SymbolArena_lock, Mutex::_no_safepoint_check_flag); // Protect arena
    // Deleting permanent symbol should not occur very often (insert race condition),
    // so log it.
    log_trace_symboltable_helper(sym, "Freeing permanent symbol");
    if (!arena()->Afree(sym, sym->size())) {
      log_trace_symboltable_helper(sym, "Leaked permanent symbol");
    }
  } else {
    delete sym;
  }
}

void SymbolTable::item_added() {
  Atomic::inc(&(SymbolTable::the_table()->_items_count));
}

void SymbolTable::set_item_clean_count(size_t ncl) {
  Atomic::store(ncl, &(SymbolTable::the_table()->_uncleaned_items_count));
  log_trace(symboltable)("Set uncleaned items:" SIZE_FORMAT, SymbolTable::the_table()->_uncleaned_items_count);
}

void SymbolTable::mark_item_clean_count() {
  if (Atomic::cmpxchg((size_t)1, &(SymbolTable::the_table()->_uncleaned_items_count), (size_t)0) == 0) { // only mark if unset
    log_trace(symboltable)("Marked uncleaned items:" SIZE_FORMAT, SymbolTable::the_table()->_uncleaned_items_count);
  }
}

void SymbolTable::item_removed() {
  Atomic::inc(&(SymbolTable::the_table()->_symbols_removed));
  Atomic::dec(&(SymbolTable::the_table()->_items_count));
}

double SymbolTable::get_load_factor() {
  return (double)_items_count/_current_size;
}

double SymbolTable::get_dead_factor() {
  return (double)_uncleaned_items_count/_current_size;
}

size_t SymbolTable::table_size() {
  return ((size_t)1) << _local_table->get_size_log2(Thread::current());
}

void SymbolTable::trigger_concurrent_work() {
  MutexLockerEx ml(Service_lock, Mutex::_no_safepoint_check_flag);
  SymbolTable::the_table()->_has_work = true;
  Service_lock->notify_all();
}

Symbol* SymbolTable::allocate_symbol(const char* name, int len, bool c_heap, TRAPS) {
  assert (len <= Symbol::max_length(), "should be checked by caller");

  Symbol* sym;
  if (DumpSharedSpaces) {
    c_heap = false;
  }
  if (c_heap) {
    // refcount starts as 1
    sym = new (len, THREAD) Symbol((const u1*)name, len, 1);
    assert(sym != NULL, "new should call vm_exit_out_of_memory if C_HEAP is exhausted");
  } else {
    // Allocate to global arena
    MutexLockerEx ml(SymbolArena_lock, Mutex::_no_safepoint_check_flag); // Protect arena
    sym = new (len, arena(), THREAD) Symbol((const u1*)name, len, PERM_REFCOUNT);
  }
  return sym;
}

void SymbolTable::initialize_symbols(int arena_alloc_size) {
  // Initialize the arena for global symbols, size passed in depends on CDS.
  if (arena_alloc_size == 0) {
    _arena = new (mtSymbol) Arena(mtSymbol);
  } else {
    _arena = new (mtSymbol) Arena(mtSymbol, arena_alloc_size);
  }
}

class SymbolsDo : StackObj {
  SymbolClosure *_cl;
public:
  SymbolsDo(SymbolClosure *cl) : _cl(cl) {}
  bool operator()(Symbol** value) {
    assert(value != NULL, "expected valid value");
    assert(*value != NULL, "value should point to a symbol");
    _cl->do_symbol(value);
    return true;
  };
};

// Call function for all symbols in the symbol table.
void SymbolTable::symbols_do(SymbolClosure *cl) {
  // all symbols from shared table
  _shared_table.symbols_do(cl);

  // all symbols from the dynamic table
  SymbolsDo sd(cl);
  if (!SymbolTable::the_table()->_local_table->try_scan(Thread::current(), sd)) {
    log_info(stringtable)("symbols_do unavailable at this moment");
  }
}

class MetaspacePointersDo : StackObj {
  MetaspaceClosure *_it;
public:
  MetaspacePointersDo(MetaspaceClosure *it) : _it(it) {}
  bool operator()(Symbol** value) {
    assert(value != NULL, "expected valid value");
    assert(*value != NULL, "value should point to a symbol");
    _it->push(value);
    return true;
  };
};

void SymbolTable::metaspace_pointers_do(MetaspaceClosure* it) {
  assert(DumpSharedSpaces, "called only during dump time");
  MetaspacePointersDo mpd(it);
  SymbolTable::the_table()->_local_table->do_scan(Thread::current(), mpd);
}

Symbol* SymbolTable::lookup_dynamic(const char* name,
                                    int len, unsigned int hash) {
  Symbol* sym = SymbolTable::the_table()->do_lookup(name, len, hash);
  assert((sym == NULL) || sym->refcount() != 0, "refcount must not be zero");
  return sym;
}

Symbol* SymbolTable::lookup_shared(const char* name,
                                   int len, unsigned int hash) {
  if (!_shared_table.empty()) {
    if (SymbolTable::_alt_hash) {
      // hash_code parameter may use alternate hashing algorithm but the shared table
      // always uses the same original hash code.
      hash = hash_shared_symbol(name, len);
    }
    return _shared_table.lookup(name, hash, len);
  } else {
    return NULL;
  }
}

Symbol* SymbolTable::lookup_common(const char* name,
                            int len, unsigned int hash) {
  Symbol* sym;
  if (_lookup_shared_first) {
    sym = lookup_shared(name, len, hash);
    if (sym == NULL) {
      _lookup_shared_first = false;
      sym = lookup_dynamic(name, len, hash);
    }
  } else {
    sym = lookup_dynamic(name, len, hash);
    if (sym == NULL) {
      sym = lookup_shared(name, len, hash);
      if (sym != NULL) {
        _lookup_shared_first = true;
      }
    }
  }
  return sym;
}

Symbol* SymbolTable::lookup(const char* name, int len, TRAPS) {
  unsigned int hash = hash_symbol(name, len, SymbolTable::_alt_hash);
  Symbol* sym = SymbolTable::the_table()->lookup_common(name, len, hash);
  if (sym == NULL) {
    sym = SymbolTable::the_table()->do_add_if_needed(name, len, hash, true, CHECK_NULL);
  }
  assert(sym->refcount() != 0, "lookup should have incremented the count");
  assert(sym->equals(name, len), "symbol must be properly initialized");
  return sym;
}

Symbol* SymbolTable::lookup(const Symbol* sym, int begin, int end, TRAPS) {
  assert(sym->refcount() != 0, "require a valid symbol");
  const char* name = (const char*)sym->base() + begin;
  int len = end - begin;
  unsigned int hash = hash_symbol(name, len, SymbolTable::_alt_hash);
  Symbol* found = SymbolTable::the_table()->lookup_common(name, len, hash);
  if (found == NULL) {
    found = SymbolTable::the_table()->do_add_if_needed(name, len, hash, true, THREAD);
  }
  return found;
}

class SymbolTableLookup : StackObj {
private:
  Thread* _thread;
  uintx _hash;
  int _len;
  const char* _str;
public:
  SymbolTableLookup(Thread* thread, const char* key, int len, uintx hash)
  : _thread(thread), _hash(hash), _len(len), _str(key) {}
  uintx get_hash() const {
    return _hash;
  }
  bool equals(Symbol** value, bool* is_dead) {
    assert(value != NULL, "expected valid value");
    assert(*value != NULL, "value should point to a symbol");
    Symbol *sym = *value;
    if (sym->equals(_str, _len)) {
      if (sym->try_increment_refcount()) {
        // something is referencing this symbol now.
        return true;
      } else {
        assert(sym->refcount() == 0, "expected dead symbol");
        *is_dead = true;
        return false;
      }
    } else {
      *is_dead = (sym->refcount() == 0);
      return false;
    }
  }
};

class SymbolTableGet : public StackObj {
  Symbol* _return;
public:
  SymbolTableGet() : _return(NULL) {}
  void operator()(Symbol** value) {
    assert(value != NULL, "expected valid value");
    assert(*value != NULL, "value should point to a symbol");
    _return = *value;
  }
  Symbol* get_res_sym() {
    return _return;
  }
};

Symbol* SymbolTable::do_lookup(const char* name, int len, uintx hash) {
  Thread* thread = Thread::current();
  SymbolTableLookup lookup(thread, name, len, hash);
  SymbolTableGet stg;
  bool rehash_warning = false;
  _local_table->get(thread, lookup, stg, &rehash_warning);
  if (rehash_warning) {
    _needs_rehashing = true;
  }
  Symbol* sym = stg.get_res_sym();
  assert((sym == NULL) || sym->refcount() != 0, "found dead symbol");
  return sym;
}

Symbol* SymbolTable::lookup_only(const char* name, int len, unsigned int& hash) {
  hash = hash_symbol(name, len, SymbolTable::_alt_hash);
  return SymbolTable::the_table()->lookup_common(name, len, hash);
}

// Suggestion: Push unicode-based lookup all the way into the hashing
// and probing logic, so there is no need for convert_to_utf8 until
// an actual new Symbol* is created.
Symbol* SymbolTable::lookup_unicode(const jchar* name, int utf16_length, TRAPS) {
  int utf8_length = UNICODE::utf8_length((jchar*) name, utf16_length);
  char stack_buf[ON_STACK_BUFFER_LENGTH];
  if (utf8_length < (int) sizeof(stack_buf)) {
    char* chars = stack_buf;
    UNICODE::convert_to_utf8(name, utf16_length, chars);
    return lookup(chars, utf8_length, THREAD);
  } else {
    ResourceMark rm(THREAD);
    char* chars = NEW_RESOURCE_ARRAY(char, utf8_length + 1);
    UNICODE::convert_to_utf8(name, utf16_length, chars);
    return lookup(chars, utf8_length, THREAD);
  }
}

Symbol* SymbolTable::lookup_only_unicode(const jchar* name, int utf16_length,
                                           unsigned int& hash) {
  int utf8_length = UNICODE::utf8_length((jchar*) name, utf16_length);
  char stack_buf[ON_STACK_BUFFER_LENGTH];
  if (utf8_length < (int) sizeof(stack_buf)) {
    char* chars = stack_buf;
    UNICODE::convert_to_utf8(name, utf16_length, chars);
    return lookup_only(chars, utf8_length, hash);
  } else {
    ResourceMark rm;
    char* chars = NEW_RESOURCE_ARRAY(char, utf8_length + 1);
    UNICODE::convert_to_utf8(name, utf16_length, chars);
    return lookup_only(chars, utf8_length, hash);
  }
}

void SymbolTable::add(ClassLoaderData* loader_data, const constantPoolHandle& cp,
                      int names_count, const char** names, int* lengths,
                      int* cp_indices, unsigned int* hashValues, TRAPS) {
  bool c_heap = !loader_data->is_the_null_class_loader_data();
  for (int i = 0; i < names_count; i++) {
    const char *name = names[i];
    int len = lengths[i];
    unsigned int hash = hashValues[i];
    Symbol* sym = SymbolTable::the_table()->lookup_common(name, len, hash);
    if (sym == NULL) {
      sym = SymbolTable::the_table()->do_add_if_needed(name, len, hash, c_heap, CHECK);
    }
    assert(sym->refcount() != 0, "lookup should have incremented the count");
    cp->symbol_at_put(cp_indices[i], sym);
  }
}

class SymbolTableCreateEntry : public StackObj {
private:
  Thread*     _thread;
  const char* _name;
  int         _len;
  bool        _heap;
  Symbol*     _return;
  Symbol*     _created;

  void assert_for_name(Symbol* sym, const char* where) const {
#ifdef ASSERT
    assert(sym->utf8_length() == _len, "%s [%d,%d]", where, sym->utf8_length(), _len);
    for (int i = 0; i < _len; i++) {
      assert(sym->byte_at(i) == (jbyte) _name[i],
             "%s [%d,%d,%d]", where, i, sym->byte_at(i), _name[i]);
    }
#endif
  }

public:
  SymbolTableCreateEntry(Thread* thread, const char* name, int len, bool heap)
  : _thread(thread), _name(name) , _len(len), _heap(heap), _return(NULL) , _created(NULL) {
    assert(_name != NULL, "expected valid name");
  }
  Symbol* operator()() {
    _created = SymbolTable::the_table()->allocate_symbol(_name, _len, _heap, _thread);
    assert(_created != NULL, "expected created symbol");
    assert_for_name(_created, "operator()()");
    assert(_created->equals(_name, _len),
           "symbol must be properly initialized [%p,%d,%d]", _name, _len, (int)_heap);
    return _created;
  }
  void operator()(bool inserted, Symbol** value) {
    assert(value != NULL, "expected valid value");
    assert(*value != NULL, "value should point to a symbol");
    if (!inserted && (_created != NULL)) {
      // We created our symbol, but someone else inserted
      // theirs first, so ours will be destroyed.
      // Since symbols are created with refcount of 1,
      // we must decrement it here to 0 to delete,
      // unless it's a permanent one.
      if (_created->refcount() != PERM_REFCOUNT) {
        assert(_created->refcount() == 1, "expected newly created symbol");
        _created->decrement_refcount();
        assert(_created->refcount() == 0, "expected dead symbol");
      }
    }
    _return = *value;
    assert_for_name(_return, "operator()");
  }
  Symbol* get_new_sym() const {
    assert_for_name(_return, "get_new_sym");
    return _return;
  }
};

Symbol* SymbolTable::do_add_if_needed(const char* name, int len, uintx hash, bool heap, TRAPS) {
  SymbolTableLookup lookup(THREAD, name, len, hash);
  SymbolTableCreateEntry stce(THREAD, name, len, heap);
  bool rehash_warning = false;
  bool clean_hint = false;
  _local_table->get_insert_lazy(THREAD, lookup, stce, stce, &rehash_warning, &clean_hint);
  if (rehash_warning) {
    _needs_rehashing = true;
  }
  if (clean_hint) {
    // we just found out that there is a dead item,
    // which we were unable to clean right now,
    // but we have no way of telling whether it's
    // been previously counted or not, so mark
    // it only if no other items were found yet
    mark_item_clean_count();
    check_concurrent_work();
  }
  Symbol* sym = stce.get_new_sym();
  assert(sym->refcount() != 0, "zero is invalid");
  return sym;
}

Symbol* SymbolTable::new_permanent_symbol(const char* name, TRAPS) {
  unsigned int hash = 0;
  int len = (int)strlen(name);
  Symbol* sym = SymbolTable::lookup_only(name, len, hash);
  if (sym == NULL) {
    sym = SymbolTable::the_table()->do_add_if_needed(name, len, hash, false, CHECK_NULL);
  }
  if (sym->refcount() != PERM_REFCOUNT) {
    sym->increment_refcount();
    log_trace_symboltable_helper(sym, "Asked for a permanent symbol, but got a regular one");
  }
  return sym;
}

struct SizeFunc : StackObj {
  size_t operator()(Symbol** value) {
    assert(value != NULL, "expected valid value");
    assert(*value != NULL, "value should point to a symbol");
    return (*value)->size() * HeapWordSize;
  };
};

void SymbolTable::print_table_statistics(outputStream* st,
                                         const char* table_name) {
  SizeFunc sz;
  _local_table->statistics_to(Thread::current(), sz, st, table_name);
}

// Verification
class VerifySymbols : StackObj {
public:
  bool operator()(Symbol** value) {
    guarantee(value != NULL, "expected valid value");
    guarantee(*value != NULL, "value should point to a symbol");
    Symbol* sym = *value;
    guarantee(sym->equals((const char*)sym->bytes(), sym->utf8_length()),
              "symbol must be internally consistent");
    return true;
  };
};

void SymbolTable::verify() {
  Thread* thr = Thread::current();
  VerifySymbols vs;
  if (!SymbolTable::the_table()->_local_table->try_scan(thr, vs)) {
    log_info(stringtable)("verify unavailable at this moment");
  }
}

// Dumping
class DumpSymbol : StackObj {
  Thread* _thr;
  outputStream* _st;
public:
  DumpSymbol(Thread* thr, outputStream* st) : _thr(thr), _st(st) {}
  bool operator()(Symbol** value) {
    assert(value != NULL, "expected valid value");
    assert(*value != NULL, "value should point to a symbol");
    Symbol* sym = *value;
    const char* utf8_string = (const char*)sym->bytes();
    int utf8_length = sym->utf8_length();
    _st->print("%d %d: ", utf8_length, sym->refcount());
    HashtableTextDump::put_utf8(_st, utf8_string, utf8_length);
    _st->cr();
    return true;
  };
};

void SymbolTable::dump(outputStream* st, bool verbose) {
  if (!verbose) {
    SymbolTable::the_table()->print_table_statistics(st, "SymbolTable");
  } else {
    Thread* thr = Thread::current();
    ResourceMark rm(thr);
    st->print_cr("VERSION: 1.1");
    DumpSymbol ds(thr, st);
    if (!SymbolTable::the_table()->_local_table->try_scan(thr, ds)) {
      log_info(symboltable)("dump unavailable at this moment");
    }
  }
}

#if INCLUDE_CDS
struct CopyToArchive : StackObj {
  CompactSymbolTableWriter* _writer;
  CopyToArchive(CompactSymbolTableWriter* writer) : _writer(writer) {}
  bool operator()(Symbol** value) {
    assert(value != NULL, "expected valid value");
    assert(*value != NULL, "value should point to a symbol");
    Symbol* sym = *value;
    unsigned int fixed_hash = hash_shared_symbol((const char*)sym->bytes(), sym->utf8_length());
    if (fixed_hash == 0) {
      return true;
    }
    assert(fixed_hash == hash_symbol((const char*)sym->bytes(), sym->utf8_length(), false),
           "must not rehash during dumping");

    // add to the compact table
    _writer->add(fixed_hash, sym);

    return true;
  }
};

void SymbolTable::copy_shared_symbol_table(CompactSymbolTableWriter* writer) {
  CopyToArchive copy(writer);
  SymbolTable::the_table()->_local_table->do_scan(Thread::current(), copy);
}

void SymbolTable::write_to_archive() {
  _shared_table.reset();

  int num_buckets = (int)(SymbolTable::the_table()->_items_count / SharedSymbolTableBucketSize);
  // calculation of num_buckets can result in zero buckets, we need at least one
  CompactSymbolTableWriter writer(num_buckets > 1 ? num_buckets : 1,
                                  &MetaspaceShared::stats()->symbol);
  copy_shared_symbol_table(&writer);
  writer.dump(&_shared_table);

  // Verify table is correct
  Symbol* sym = vmSymbols::java_lang_Object();
  const char* name = (const char*)sym->bytes();
  int len = sym->utf8_length();
  unsigned int hash = hash_symbol(name, len, SymbolTable::_alt_hash);
  assert(sym == _shared_table.lookup(name, hash, len), "sanity");
}

void SymbolTable::serialize(SerializeClosure* soc) {
  _shared_table.set_type(CompactHashtable<Symbol*, char>::_symbol_table);
  _shared_table.serialize(soc);

  if (soc->writing()) {
    // Sanity. Make sure we don't use the shared table at dump time
    _shared_table.reset();
  }
}
#endif //INCLUDE_CDS

// Concurrent work
void SymbolTable::grow(JavaThread* jt) {
  SymbolTableHash::GrowTask gt(_local_table);
  if (!gt.prepare(jt)) {
    return;
  }
  log_trace(symboltable)("Started to grow");
  {
    TraceTime timer("Grow", TRACETIME_LOG(Debug, symboltable, perf));
    while (gt.do_task(jt)) {
      gt.pause(jt);
      {
        ThreadBlockInVM tbivm(jt);
      }
      gt.cont(jt);
    }
  }
  gt.done(jt);
  _current_size = table_size();
  log_debug(symboltable)("Grown to size:" SIZE_FORMAT, _current_size);
}

struct SymbolTableDoDelete : StackObj {
  int _deleted;
  SymbolTableDoDelete() : _deleted(0) {}
  void operator()(Symbol** value) {
    assert(value != NULL, "expected valid value");
    assert(*value != NULL, "value should point to a symbol");
    Symbol *sym = *value;
    assert(sym->refcount() == 0, "refcount");
    _deleted++;
  }
};

struct SymbolTableDeleteCheck : StackObj {
  int _processed;
  SymbolTableDeleteCheck() : _processed(0) {}
  bool operator()(Symbol** value) {
    assert(value != NULL, "expected valid value");
    assert(*value != NULL, "value should point to a symbol");
    _processed++;
    Symbol *sym = *value;
    return (sym->refcount() == 0);
  }
};

void SymbolTable::clean_dead_entries(JavaThread* jt) {
  SymbolTableHash::BulkDeleteTask bdt(_local_table);
  if (!bdt.prepare(jt)) {
    return;
  }

  SymbolTableDeleteCheck stdc;
  SymbolTableDoDelete stdd;
  {
    TraceTime timer("Clean", TRACETIME_LOG(Debug, symboltable, perf));
    while (bdt.do_task(jt, stdc, stdd)) {
      bdt.pause(jt);
      {
        ThreadBlockInVM tbivm(jt);
      }
      bdt.cont(jt);
    }
    SymbolTable::the_table()->set_item_clean_count(0);
    bdt.done(jt);
  }

  Atomic::add((size_t)stdc._processed, &_symbols_counted);

  log_debug(symboltable)("Cleaned " INT32_FORMAT " of " INT32_FORMAT,
                         stdd._deleted, stdc._processed);
}

void SymbolTable::check_concurrent_work() {
  if (_has_work) {
    return;
  }
  double load_factor = SymbolTable::get_load_factor();
  double dead_factor = SymbolTable::get_dead_factor();
  // We should clean/resize if we have more dead than alive,
  // more items than preferred load factor or
  // more dead items than water mark.
  if ((dead_factor > load_factor) ||
      (load_factor > PREF_AVG_LIST_LEN) ||
      (dead_factor > CLEAN_DEAD_HIGH_WATER_MARK)) {
    log_debug(symboltable)("Concurrent work triggered, live factor:%f dead factor:%f",
                           load_factor, dead_factor);
    trigger_concurrent_work();
  }
}

void SymbolTable::concurrent_work(JavaThread* jt) {
  double load_factor = get_load_factor();
  log_debug(symboltable, perf)("Concurrent work, live factor: %g", load_factor);
  // We prefer growing, since that also removes dead items
  if (load_factor > PREF_AVG_LIST_LEN && !_local_table->is_max_size_reached()) {
    grow(jt);
  } else {
    clean_dead_entries(jt);
  }
  _has_work = false;
}

class CountDead : StackObj {
  int _count;
public:
  CountDead() : _count(0) {}
  bool operator()(Symbol** value) {
    assert(value != NULL, "expected valid value");
    assert(*value != NULL, "value should point to a symbol");
    Symbol* sym = *value;
    if (sym->refcount() == 0) {
      _count++;
    }
    return true;
  };
  int get_dead_count() {
    return _count;
  }
};

void SymbolTable::do_check_concurrent_work() {
  CountDead counter;
  if (!SymbolTable::the_table()->_local_table->try_scan(Thread::current(), counter)) {
    log_info(symboltable)("count dead unavailable at this moment");
  } else {
    SymbolTable::the_table()->set_item_clean_count(counter.get_dead_count());
    SymbolTable::the_table()->check_concurrent_work();
  }
}

void SymbolTable::do_concurrent_work(JavaThread* jt) {
  SymbolTable::the_table()->concurrent_work(jt);
}

// Rehash
bool SymbolTable::do_rehash() {
  if (!_local_table->is_safepoint_safe()) {
    return false;
  }

  // We use max size
  SymbolTableHash* new_table = new SymbolTableHash(END_SIZE, END_SIZE, REHASH_LEN);
  // Use alt hash from now on
  _alt_hash = true;
  if (!_local_table->try_move_nodes_to(Thread::current(), new_table)) {
    _alt_hash = false;
    delete new_table;
    return false;
  }

  // free old table
  delete _local_table;
  _local_table = new_table;

  return true;
}

void SymbolTable::try_rehash_table() {
  static bool rehashed = false;
  log_debug(symboltable)("Table imbalanced, rehashing called.");

  // Grow instead of rehash.
  if (get_load_factor() > PREF_AVG_LIST_LEN &&
      !_local_table->is_max_size_reached()) {
    log_debug(symboltable)("Choosing growing over rehashing.");
    trigger_concurrent_work();
    _needs_rehashing = false;
    return;
  }

  // Already rehashed.
  if (rehashed) {
    log_warning(symboltable)("Rehashing already done, still long lists.");
    trigger_concurrent_work();
    _needs_rehashing = false;
    return;
  }

  murmur_seed = AltHashing::compute_seed();

  if (do_rehash()) {
    rehashed = true;
  } else {
    log_info(symboltable)("Resizes in progress rehashing skipped.");
  }

  _needs_rehashing = false;
}

void SymbolTable::rehash_table() {
  SymbolTable::the_table()->try_rehash_table();
}

//---------------------------------------------------------------------------
// Non-product code

#ifndef PRODUCT

class HistogramIterator : StackObj {
public:
  static const size_t results_length = 100;
  size_t counts[results_length];
  size_t sizes[results_length];
  size_t total_size;
  size_t total_count;
  size_t total_length;
  size_t max_length;
  size_t out_of_range_count;
  size_t out_of_range_size;
  HistogramIterator() : total_size(0), total_count(0), total_length(0),
                        max_length(0), out_of_range_count(0), out_of_range_size(0) {
    // initialize results to zero
    for (size_t i = 0; i < results_length; i++) {
      counts[i] = 0;
      sizes[i] = 0;
    }
  }
  bool operator()(Symbol** value) {
    assert(value != NULL, "expected valid value");
    assert(*value != NULL, "value should point to a symbol");
    Symbol* sym = *value;
    size_t size = sym->size();
    size_t len = sym->utf8_length();
    if (len < results_length) {
      counts[len]++;
      sizes[len] += size;
    } else {
      out_of_range_count++;
      out_of_range_size += size;
    }
    total_count++;
    total_size += size;
    total_length += len;
    max_length = MAX2(max_length, len);

    return true;
  };
};

void SymbolTable::print_histogram() {
  SymbolTable* st = SymbolTable::the_table();
  HistogramIterator hi;
  st->_local_table->do_scan(Thread::current(), hi);
  tty->print_cr("Symbol Table Histogram:");
  tty->print_cr("  Total number of symbols  " SIZE_FORMAT_W(7), hi.total_count);
  tty->print_cr("  Total size in memory     " SIZE_FORMAT_W(7) "K",
          (hi.total_size * wordSize) / 1024);
  tty->print_cr("  Total counted            " SIZE_FORMAT_W(7), st->_symbols_counted);
  tty->print_cr("  Total removed            " SIZE_FORMAT_W(7), st->_symbols_removed);
  if (SymbolTable::the_table()->_symbols_counted > 0) {
    tty->print_cr("  Percent removed          %3.2f",
          ((float)st->_symbols_removed / st->_symbols_counted) * 100);
  }
  tty->print_cr("  Reference counts         " SIZE_FORMAT_W(7), Symbol::_total_count);
  tty->print_cr("  Symbol arena used        " SIZE_FORMAT_W(7) "K", arena()->used() / 1024);
  tty->print_cr("  Symbol arena size        " SIZE_FORMAT_W(7) "K", arena()->size_in_bytes() / 1024);
  tty->print_cr("  Total symbol length      " SIZE_FORMAT_W(7), hi.total_length);
  tty->print_cr("  Maximum symbol length    " SIZE_FORMAT_W(7), hi.max_length);
  tty->print_cr("  Average symbol length    %7.2f", ((float)hi.total_length / hi.total_count));
  tty->print_cr("  Symbol length histogram:");
  tty->print_cr("    %6s %10s %10s", "Length", "#Symbols", "Size");
  for (size_t i = 0; i < hi.results_length; i++) {
    if (hi.counts[i] > 0) {
      tty->print_cr("    " SIZE_FORMAT_W(6) " " SIZE_FORMAT_W(10) " " SIZE_FORMAT_W(10) "K",
                    i, hi.counts[i], (hi.sizes[i] * wordSize) / 1024);
    }
  }
  tty->print_cr("  >=" SIZE_FORMAT_W(6) " " SIZE_FORMAT_W(10) " " SIZE_FORMAT_W(10) "K\n",
                hi.results_length, hi.out_of_range_count, (hi.out_of_range_size*wordSize) / 1024);
}
#endif // PRODUCT

// Utility for dumping symbols
SymboltableDCmd::SymboltableDCmd(outputStream* output, bool heap) :
                                 DCmdWithParser(output, heap),
  _verbose("-verbose", "Dump the content of each symbol in the table",
           "BOOLEAN", false, "false") {
  _dcmdparser.add_dcmd_option(&_verbose);
}

void SymboltableDCmd::execute(DCmdSource source, TRAPS) {
  VM_DumpHashtable dumper(output(), VM_DumpHashtable::DumpSymbols,
                         _verbose.value());
  VMThread::execute(&dumper);
}

int SymboltableDCmd::num_arguments() {
  ResourceMark rm;
  SymboltableDCmd* dcmd = new SymboltableDCmd(NULL, false);
  if (dcmd != NULL) {
    DCmdMark mark(dcmd);
    return dcmd->_dcmdparser.num_arguments();
  } else {
    return 0;
  }
}
