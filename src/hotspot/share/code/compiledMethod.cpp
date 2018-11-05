/*
 * Copyright (c) 2015, 2018, Oracle and/or its affiliates. All rights reserved.
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
#include "code/compiledIC.hpp"
#include "code/compiledMethod.inline.hpp"
#include "code/scopeDesc.hpp"
#include "code/codeCache.hpp"
#include "gc/shared/barrierSet.hpp"
#include "gc/shared/gcBehaviours.hpp"
#include "interpreter/bytecode.inline.hpp"
#include "logging/log.hpp"
#include "logging/logTag.hpp"
#include "memory/resourceArea.hpp"
#include "oops/methodData.hpp"
#include "oops/method.inline.hpp"
#include "prims/methodHandles.hpp"
#include "runtime/handles.inline.hpp"
#include "runtime/mutexLocker.hpp"

CompiledMethod::CompiledMethod(Method* method, const char* name, CompilerType type, const CodeBlobLayout& layout,
                               int frame_complete_offset, int frame_size, ImmutableOopMapSet* oop_maps,
                               bool caller_must_gc_arguments)
  : CodeBlob(name, type, layout, frame_complete_offset, frame_size, oop_maps, caller_must_gc_arguments),
    _mark_for_deoptimization_status(not_marked),
    _is_unloading_state(0),
    _method(method)
{
  init_defaults();
  clear_unloading_state();
}

CompiledMethod::CompiledMethod(Method* method, const char* name, CompilerType type, int size,
                               int header_size, CodeBuffer* cb, int frame_complete_offset, int frame_size,
                               OopMapSet* oop_maps, bool caller_must_gc_arguments)
  : CodeBlob(name, type, CodeBlobLayout((address) this, size, header_size, cb), cb,
             frame_complete_offset, frame_size, oop_maps, caller_must_gc_arguments),
    _mark_for_deoptimization_status(not_marked),
    _is_unloading_state(0),
    _method(method)
{
  init_defaults();
  clear_unloading_state();
}

void CompiledMethod::init_defaults() {
  _has_unsafe_access          = 0;
  _has_method_handle_invokes  = 0;
  _lazy_critical_native       = 0;
  _has_wide_vectors           = 0;
}

bool CompiledMethod::is_method_handle_return(address return_pc) {
  if (!has_method_handle_invokes())  return false;
  PcDesc* pd = pc_desc_at(return_pc);
  if (pd == NULL)
    return false;
  return pd->is_method_handle_invoke();
}

// Returns a string version of the method state.
const char* CompiledMethod::state() const {
  int state = get_state();
  switch (state) {
  case not_installed:
    return "not installed";
  case in_use:
    return "in use";
  case not_used:
    return "not_used";
  case not_entrant:
    return "not_entrant";
  case zombie:
    return "zombie";
  case unloaded:
    return "unloaded";
  default:
    fatal("unexpected method state: %d", state);
    return NULL;
  }
}

//-----------------------------------------------------------------------------

ExceptionCache* CompiledMethod::exception_cache_acquire() const {
  return OrderAccess::load_acquire(&_exception_cache);
}

void CompiledMethod::add_exception_cache_entry(ExceptionCache* new_entry) {
  assert(ExceptionCache_lock->owned_by_self(),"Must hold the ExceptionCache_lock");
  assert(new_entry != NULL,"Must be non null");
  assert(new_entry->next() == NULL, "Must be null");

  for (;;) {
    ExceptionCache *ec = exception_cache();
    if (ec != NULL) {
      Klass* ex_klass = ec->exception_type();
      if (!ex_klass->is_loader_alive()) {
        // We must guarantee that entries are not inserted with new next pointer
        // edges to ExceptionCache entries with dead klasses, due to bad interactions
        // with concurrent ExceptionCache cleanup. Therefore, the inserts roll
        // the head pointer forward to the first live ExceptionCache, so that the new
        // next pointers always point at live ExceptionCaches, that are not removed due
        // to concurrent ExceptionCache cleanup.
        ExceptionCache* next = ec->next();
        if (Atomic::cmpxchg(next, &_exception_cache, ec) == ec) {
          CodeCache::release_exception_cache(ec);
        }
        continue;
      }
      ec = exception_cache();
      if (ec != NULL) {
        new_entry->set_next(ec);
      }
    }
    if (Atomic::cmpxchg(new_entry, &_exception_cache, ec) == ec) {
      return;
    }
  }
}

void CompiledMethod::clean_exception_cache() {
  // For each nmethod, only a single thread may call this cleanup function
  // at the same time, whether called in STW cleanup or concurrent cleanup.
  // Note that if the GC is processing exception cache cleaning in a concurrent phase,
  // then a single writer may contend with cleaning up the head pointer to the
  // first ExceptionCache node that has a Klass* that is alive. That is fine,
  // as long as there is no concurrent cleanup of next pointers from concurrent writers.
  // And the concurrent writers do not clean up next pointers, only the head.
  // Also note that concurent readers will walk through Klass* pointers that are not
  // alive. That does not cause ABA problems, because Klass* is deleted after
  // a handshake with all threads, after all stale ExceptionCaches have been
  // unlinked. That is also when the CodeCache::exception_cache_purge_list()
  // is deleted, with all ExceptionCache entries that were cleaned concurrently.
  // That similarly implies that CAS operations on ExceptionCache entries do not
  // suffer from ABA problems as unlinking and deletion is separated by a global
  // handshake operation.
  ExceptionCache* prev = NULL;
  ExceptionCache* curr = exception_cache_acquire();

  while (curr != NULL) {
    ExceptionCache* next = curr->next();

    if (!curr->exception_type()->is_loader_alive()) {
      if (prev == NULL) {
        // Try to clean head; this is contended by concurrent inserts, that
        // both lazily clean the head, and insert entries at the head. If
        // the CAS fails, the operation is restarted.
        if (Atomic::cmpxchg(next, &_exception_cache, curr) != curr) {
          prev = NULL;
          curr = exception_cache_acquire();
          continue;
        }
      } else {
        // It is impossible to during cleanup connect the next pointer to
        // an ExceptionCache that has not been published before a safepoint
        // prior to the cleanup. Therefore, release is not required.
        prev->set_next(next);
      }
      // prev stays the same.

      CodeCache::release_exception_cache(curr);
    } else {
      prev = curr;
    }

    curr = next;
  }
}

// public method for accessing the exception cache
// These are the public access methods.
address CompiledMethod::handler_for_exception_and_pc(Handle exception, address pc) {
  // We never grab a lock to read the exception cache, so we may
  // have false negatives. This is okay, as it can only happen during
  // the first few exception lookups for a given nmethod.
  ExceptionCache* ec = exception_cache_acquire();
  while (ec != NULL) {
    address ret_val;
    if ((ret_val = ec->match(exception,pc)) != NULL) {
      return ret_val;
    }
    ec = ec->next();
  }
  return NULL;
}

void CompiledMethod::add_handler_for_exception_and_pc(Handle exception, address pc, address handler) {
  // There are potential race conditions during exception cache updates, so we
  // must own the ExceptionCache_lock before doing ANY modifications. Because
  // we don't lock during reads, it is possible to have several threads attempt
  // to update the cache with the same data. We need to check for already inserted
  // copies of the current data before adding it.

  MutexLocker ml(ExceptionCache_lock);
  ExceptionCache* target_entry = exception_cache_entry_for_exception(exception);

  if (target_entry == NULL || !target_entry->add_address_and_handler(pc,handler)) {
    target_entry = new ExceptionCache(exception,pc,handler);
    add_exception_cache_entry(target_entry);
  }
}

// private method for handling exception cache
// These methods are private, and used to manipulate the exception cache
// directly.
ExceptionCache* CompiledMethod::exception_cache_entry_for_exception(Handle exception) {
  ExceptionCache* ec = exception_cache_acquire();
  while (ec != NULL) {
    if (ec->match_exception_with_space(exception)) {
      return ec;
    }
    ec = ec->next();
  }
  return NULL;
}

//-------------end of code for ExceptionCache--------------

bool CompiledMethod::is_at_poll_return(address pc) {
  RelocIterator iter(this, pc, pc+1);
  while (iter.next()) {
    if (iter.type() == relocInfo::poll_return_type)
      return true;
  }
  return false;
}


bool CompiledMethod::is_at_poll_or_poll_return(address pc) {
  RelocIterator iter(this, pc, pc+1);
  while (iter.next()) {
    relocInfo::relocType t = iter.type();
    if (t == relocInfo::poll_return_type || t == relocInfo::poll_type)
      return true;
  }
  return false;
}

void CompiledMethod::verify_oop_relocations() {
  // Ensure sure that the code matches the current oop values
  RelocIterator iter(this, NULL, NULL);
  while (iter.next()) {
    if (iter.type() == relocInfo::oop_type) {
      oop_Relocation* reloc = iter.oop_reloc();
      if (!reloc->oop_is_immediate()) {
        reloc->verify_oop_relocation();
      }
    }
  }
}


ScopeDesc* CompiledMethod::scope_desc_at(address pc) {
  PcDesc* pd = pc_desc_at(pc);
  guarantee(pd != NULL, "scope must be present");
  return new ScopeDesc(this, pd->scope_decode_offset(),
                       pd->obj_decode_offset(), pd->should_reexecute(), pd->rethrow_exception(),
                       pd->return_oop());
}

ScopeDesc* CompiledMethod::scope_desc_near(address pc) {
  PcDesc* pd = pc_desc_near(pc);
  guarantee(pd != NULL, "scope must be present");
  return new ScopeDesc(this, pd->scope_decode_offset(),
                       pd->obj_decode_offset(), pd->should_reexecute(), pd->rethrow_exception(),
                       pd->return_oop());
}

address CompiledMethod::oops_reloc_begin() const {
  // If the method is not entrant or zombie then a JMP is plastered over the
  // first few bytes.  If an oop in the old code was there, that oop
  // should not get GC'd.  Skip the first few bytes of oops on
  // not-entrant methods.
  if (frame_complete_offset() != CodeOffsets::frame_never_safe &&
      code_begin() + frame_complete_offset() >
      verified_entry_point() + NativeJump::instruction_size)
  {
    // If we have a frame_complete_offset after the native jump, then there
    // is no point trying to look for oops before that. This is a requirement
    // for being allowed to scan oops concurrently.
    return code_begin() + frame_complete_offset();
  }

  // It is not safe to read oops concurrently using entry barriers, if their
  // location depend on whether the nmethod is entrant or not.
  assert(BarrierSet::barrier_set()->barrier_set_nmethod() == NULL, "Not safe oop scan");

  address low_boundary = verified_entry_point();
  if (!is_in_use() && is_nmethod()) {
    low_boundary += NativeJump::instruction_size;
    // %%% Note:  On SPARC we patch only a 4-byte trap, not a full NativeJump.
    // This means that the low_boundary is going to be a little too high.
    // This shouldn't matter, since oops of non-entrant methods are never used.
    // In fact, why are we bothering to look at oops in a non-entrant method??
  }
  return low_boundary;
}

int CompiledMethod::verify_icholder_relocations() {
  ResourceMark rm;
  int count = 0;

  RelocIterator iter(this);
  while(iter.next()) {
    if (iter.type() == relocInfo::virtual_call_type) {
      if (CompiledIC::is_icholder_call_site(iter.virtual_call_reloc(), this)) {
        CompiledIC *ic = CompiledIC_at(&iter);
        if (TraceCompiledIC) {
          tty->print("noticed icholder " INTPTR_FORMAT " ", p2i(ic->cached_icholder()));
          ic->print();
        }
        assert(ic->cached_icholder() != NULL, "must be non-NULL");
        count++;
      }
    }
  }

  return count;
}

// Method that knows how to preserve outgoing arguments at call. This method must be
// called with a frame corresponding to a Java invoke
void CompiledMethod::preserve_callee_argument_oops(frame fr, const RegisterMap *reg_map, OopClosure* f) {
  if (method() != NULL && !method()->is_native()) {
    address pc = fr.pc();
    SimpleScopeDesc ssd(this, pc);
    Bytecode_invoke call(ssd.method(), ssd.bci());
    bool has_receiver = call.has_receiver();
    bool has_appendix = call.has_appendix();
    Symbol* signature = call.signature();

    // The method attached by JIT-compilers should be used, if present.
    // Bytecode can be inaccurate in such case.
    Method* callee = attached_method_before_pc(pc);
    if (callee != NULL) {
      has_receiver = !(callee->access_flags().is_static());
      has_appendix = false;
      signature = callee->signature();
    }

    fr.oops_compiled_arguments_do(signature, has_receiver, has_appendix, reg_map, f);
  }
}

Method* CompiledMethod::attached_method(address call_instr) {
  assert(code_contains(call_instr), "not part of the nmethod");
  RelocIterator iter(this, call_instr, call_instr + 1);
  while (iter.next()) {
    if (iter.addr() == call_instr) {
      switch(iter.type()) {
        case relocInfo::static_call_type:      return iter.static_call_reloc()->method_value();
        case relocInfo::opt_virtual_call_type: return iter.opt_virtual_call_reloc()->method_value();
        case relocInfo::virtual_call_type:     return iter.virtual_call_reloc()->method_value();
        default:                               break;
      }
    }
  }
  return NULL; // not found
}

Method* CompiledMethod::attached_method_before_pc(address pc) {
  if (NativeCall::is_call_before(pc)) {
    NativeCall* ncall = nativeCall_before(pc);
    return attached_method(ncall->instruction_address());
  }
  return NULL; // not a call
}

void CompiledMethod::clear_inline_caches() {
  assert(SafepointSynchronize::is_at_safepoint(), "cleaning of IC's only allowed at safepoint");
  if (is_zombie()) {
    return;
  }

  RelocIterator iter(this);
  while (iter.next()) {
    iter.reloc()->clear_inline_cache();
  }
}

// Clear ICStubs of all compiled ICs
void CompiledMethod::clear_ic_stubs() {
  assert(CompiledICLocker::is_safe(this), "mt unsafe call");
  ResourceMark rm;
  RelocIterator iter(this);
  while(iter.next()) {
    if (iter.type() == relocInfo::virtual_call_type) {
      CompiledIC* ic = CompiledIC_at(&iter);
      ic->clear_ic_stub();
    }
  }
}

#ifdef ASSERT
// Check class_loader is alive for this bit of metadata.
static void check_class(Metadata* md) {
   Klass* klass = NULL;
   if (md->is_klass()) {
     klass = ((Klass*)md);
   } else if (md->is_method()) {
     klass = ((Method*)md)->method_holder();
   } else if (md->is_methodData()) {
     klass = ((MethodData*)md)->method()->method_holder();
   } else {
     md->print();
     ShouldNotReachHere();
   }
   assert(klass->is_loader_alive(), "must be alive");
}
#endif // ASSERT


void CompiledMethod::clean_ic_if_metadata_is_dead(CompiledIC *ic) {
  if (ic->is_icholder_call()) {
    // The only exception is compiledICHolder metdata which may
    // yet be marked below. (We check this further below).
    CompiledICHolder* cichk_metdata = ic->cached_icholder();

    if (cichk_metdata->is_loader_alive()) {
      return;
    }
  } else {
    Metadata* ic_metdata = ic->cached_metadata();
    if (ic_metdata != NULL) {
      if (ic_metdata->is_klass()) {
        if (((Klass*)ic_metdata)->is_loader_alive()) {
          return;
        }
      } else if (ic_metdata->is_method()) {
        Method* method = (Method*)ic_metdata;
        assert(!method->is_old(), "old method should have been cleaned");
        if (method->method_holder()->is_loader_alive()) {
          return;
        }
      } else {
        ShouldNotReachHere();
      }
    }
  }

  ic->set_to_clean();
}

// static_stub_Relocations may have dangling references to
// nmethods so trim them out here.  Otherwise it looks like
// compiled code is maintaining a link to dead metadata.
void CompiledMethod::clean_ic_stubs() {
#ifdef ASSERT
  address low_boundary = oops_reloc_begin();
  RelocIterator iter(this, low_boundary);
  while (iter.next()) {
    address static_call_addr = NULL;
    if (iter.type() == relocInfo::opt_virtual_call_type) {
      CompiledIC* cic = CompiledIC_at(&iter);
      if (!cic->is_call_to_interpreted()) {
        static_call_addr = iter.addr();
      }
    } else if (iter.type() == relocInfo::static_call_type) {
      CompiledStaticCall* csc = compiledStaticCall_at(iter.reloc());
      if (!csc->is_call_to_interpreted()) {
        static_call_addr = iter.addr();
      }
    }
    if (static_call_addr != NULL) {
      RelocIterator sciter(this, low_boundary);
      while (sciter.next()) {
        if (sciter.type() == relocInfo::static_stub_type &&
            sciter.static_stub_reloc()->static_call() == static_call_addr) {
          sciter.static_stub_reloc()->clear_inline_cache();
        }
      }
    }
  }
#endif
}

// Clean references to unloaded nmethods at addr from this one, which is not unloaded.
template <class CompiledICorStaticCall>
static void clean_if_nmethod_is_unloaded(CompiledICorStaticCall *ic, address addr, CompiledMethod* from,
                                         bool clean_all) {
  // Ok, to lookup references to zombies here
  CodeBlob *cb = CodeCache::find_blob_unsafe(addr);
  CompiledMethod* nm = (cb != NULL) ? cb->as_compiled_method_or_null() : NULL;
  if (nm != NULL) {
    // Clean inline caches pointing to both zombie and not_entrant methods
    if (clean_all || !nm->is_in_use() || nm->is_unloading() || (nm->method()->code() != nm)) {
      ic->set_to_clean(from->is_alive());
      assert(ic->is_clean(), "nmethod " PTR_FORMAT "not clean %s", p2i(from), from->method()->name_and_sig_as_C_string());
    }
  }
}

static void clean_if_nmethod_is_unloaded(CompiledIC *ic, CompiledMethod* from,
                                         bool clean_all) {
  clean_if_nmethod_is_unloaded(ic, ic->ic_destination(), from, clean_all);
}

static void clean_if_nmethod_is_unloaded(CompiledStaticCall *csc, CompiledMethod* from,
                                         bool clean_all) {
  clean_if_nmethod_is_unloaded(csc, csc->destination(), from, clean_all);
}

// Cleans caches in nmethods that point to either classes that are unloaded
// or nmethods that are unloaded.
//
// Can be called either in parallel by G1 currently or after all
// nmethods are unloaded.  Return postponed=true in the parallel case for
// inline caches found that point to nmethods that are not yet visited during
// the do_unloading walk.
void CompiledMethod::unload_nmethod_caches(bool unloading_occurred) {
  ResourceMark rm;

  // Exception cache only needs to be called if unloading occurred
  if (unloading_occurred) {
    clean_exception_cache();
  }

  cleanup_inline_caches_impl(unloading_occurred, false);

  // All static stubs need to be cleaned.
  clean_ic_stubs();

  // Check that the metadata embedded in the nmethod is alive
  DEBUG_ONLY(metadata_do(check_class));
}

// The IsUnloadingStruct represents a tuple comprising a result of
// IsUnloadingBehaviour::is_unloading() for a given unloading cycle.
struct IsUnloadingStruct {
  unsigned int _is_unloading:1;
  unsigned int _unloading_cycle:2;
};

// The IsUnloadingUnion allows treating the tuple of the IsUnloadingStruct
// like a uint8_t, making it possible to read and write the tuple atomically.
union IsUnloadingUnion {
  IsUnloadingStruct _inflated;
  uint8_t _value;
};

bool CompiledMethod::is_unloading() {
  IsUnloadingUnion state;
  state._value = RawAccess<MO_RELAXED>::load(&_is_unloading_state);
  if (state._inflated._is_unloading == 1) {
    return true;
  }
  if (state._inflated._unloading_cycle == CodeCache::unloading_cycle()) {
    return state._inflated._is_unloading == 1;
  }

  // The IsUnloadingBehaviour is responsible for checking if there are any dead
  // oops in the CompiledMethod, by calling oops_do on it.
  bool result = IsUnloadingBehaviour::current()->is_unloading(this);

  state._inflated._unloading_cycle = CodeCache::unloading_cycle();
  state._inflated._is_unloading = result ? 1 : 0;

  RawAccess<MO_RELAXED>::store(&_is_unloading_state, state._value);

  return result;
}

void CompiledMethod::clear_unloading_state() {
  IsUnloadingUnion state;
  state._inflated._unloading_cycle = CodeCache::unloading_cycle();
  state._inflated._is_unloading = 0;
  RawAccess<MO_RELAXED>::store(&_is_unloading_state, state._value);
}

// Called to clean up after class unloading for live nmethods and from the sweeper
// for all methods.
void CompiledMethod::cleanup_inline_caches_impl(bool unloading_occurred, bool clean_all) {
  assert(CompiledICLocker::is_safe(this), "mt unsafe call");
  ResourceMark rm;

  // Find all calls in an nmethod and clear the ones that point to non-entrant,
  // zombie and unloaded nmethods.
  RelocIterator iter(this, oops_reloc_begin());
  while(iter.next()) {

    switch (iter.type()) {

    case relocInfo::virtual_call_type:
      if (unloading_occurred) {
        // If class unloading occurred we first clear ICs where the cached metadata
        // is referring to an unloaded klass or method.
        clean_ic_if_metadata_is_dead(CompiledIC_at(&iter));
      }

      clean_if_nmethod_is_unloaded(CompiledIC_at(&iter), this, clean_all);
      break;

    case relocInfo::opt_virtual_call_type:
      clean_if_nmethod_is_unloaded(CompiledIC_at(&iter), this, clean_all);
      break;

    case relocInfo::static_call_type:
      clean_if_nmethod_is_unloaded(compiledStaticCall_at(iter.reloc()), this, clean_all);
      break;

    case relocInfo::oop_type:
      break;

    case relocInfo::metadata_type:
      break; // nothing to do.

    default:
      break;
    }
  }
}

// Iterating over all nmethods, e.g. with the help of CodeCache::nmethods_do(fun) was found
// to not be inherently safe. There is a chance that fields are seen which are not properly
// initialized. This happens despite the fact that nmethods_do() asserts the CodeCache_lock
// to be held.
// To bundle knowledge about necessary checks in one place, this function was introduced.
// It is not claimed that these checks are sufficient, but they were found to be necessary.
bool CompiledMethod::nmethod_access_is_safe(nmethod* nm) {
  Method* method = (nm == NULL) ? NULL : nm->method();  // nm->method() may be uninitialized, i.e. != NULL, but invalid
  return (nm != NULL) && (method != NULL) && (method->signature() != NULL) &&
         !nm->is_zombie() && !nm->is_not_installed() &&
         os::is_readable_pointer(method) &&
         os::is_readable_pointer(method->constants()) &&
         os::is_readable_pointer(method->signature());
}
