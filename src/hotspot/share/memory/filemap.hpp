/*
 * Copyright (c) 2003, 2019, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_MEMORY_FILEMAP_HPP
#define SHARE_MEMORY_FILEMAP_HPP

#include "classfile/classLoader.hpp"
#include "include/cds.h"
#include "memory/metaspaceShared.hpp"
#include "memory/metaspace.hpp"
#include "oops/compressedOops.hpp"
#include "utilities/align.hpp"

// Layout of the file:
//  header: dump of archive instance plus versioning info, datestamp, etc.
//   [magic # = 0xF00BABA2]
//  ... padding to align on page-boundary
//  read-write space
//  read-only space
//  misc data (block offset table, string table, symbols, dictionary, etc.)
//  tag(666)

static const int JVM_IDENT_MAX = 256;

class SharedClassPathEntry {
  enum {
    modules_image_entry,
    jar_entry,
    signed_jar_entry,
    dir_entry,
    non_existent_entry,
    unknown_entry
  };

  void set_name(const char* name, TRAPS);

  u1     _type;
  bool   _from_class_path_attr;
  time_t _timestamp;          // jar timestamp,  0 if is directory, modules image or other
  long   _filesize;           // jar/jimage file size, -1 if is directory, -2 if other
  Array<char>* _name;
  Array<u1>*   _manifest;

public:
  void init(bool is_modules_image, ClassPathEntry* cpe, TRAPS);
  void init_as_non_existent(const char* path, TRAPS);
  void metaspace_pointers_do(MetaspaceClosure* it);
  bool validate(bool is_class_path = true) const;

  // The _timestamp only gets set for jar files.
  bool has_timestamp() const {
    return _timestamp != 0;
  }
  bool is_dir()           const { return _type == dir_entry; }
  bool is_modules_image() const { return _type == modules_image_entry; }
  bool is_jar()           const { return _type == jar_entry; }
  bool is_signed()        const { return _type == signed_jar_entry; }
  void set_is_signed() {
    _type = signed_jar_entry;
  }
  bool from_class_path_attr() { return _from_class_path_attr; }
  time_t timestamp() const { return _timestamp; }
  long   filesize()  const { return _filesize; }
  const char* name() const;
  const char* manifest() const {
    return (_manifest == NULL) ? NULL : (const char*)_manifest->data();
  }
  int manifest_size() const {
    return (_manifest == NULL) ? 0 : _manifest->length();
  }
  void set_manifest(Array<u1>* manifest) {
    _manifest = manifest;
  }
  bool check_non_existent() const;
};

struct ArchiveHeapOopmapInfo {
  address _oopmap;               // bitmap for relocating embedded oops
  size_t  _oopmap_size_in_bits;
};

class SharedPathTable {
  Array<u8>* _table;
  int _size;
public:
  void dumptime_init(ClassLoaderData* loader_data, Thread* THREAD);
  void metaspace_pointers_do(MetaspaceClosure* it);

  int size() {
    return _size;
  }
  SharedClassPathEntry* path_at(int index) {
    if (index < 0) {
      return NULL;
    }
    assert(index < _size, "sanity");
    char* p = (char*)_table->data();
    p += sizeof(SharedClassPathEntry) * index;
    return (SharedClassPathEntry*)p;
  }
  Array<u8>* table() {return _table;}
  void set_table(Array<u8>* table) {_table = table;}
};


class FileMapRegion: private CDSFileMapRegion {
  void assert_is_heap_region() const {
    assert(_is_heap_region, "must be heap region");
  }
  void assert_is_not_heap_region() const {
    assert(!_is_heap_region, "must not be heap region");
  }

public:
  static FileMapRegion* cast(CDSFileMapRegion* p) {
    return (FileMapRegion*)p;
  }

  // Accessors
  int crc()                      const { return _crc; }
  size_t file_offset()           const { return _file_offset; }
  char*  base()                  const { assert_is_not_heap_region(); return _addr._base;  }
  narrowOop offset()             const { assert_is_heap_region();     return (narrowOop)(_addr._offset); }
  size_t used()                  const { return _used; }
  bool read_only()               const { return _read_only != 0; }
  bool allow_exec()              const { return _allow_exec != 0; }
  void* oopmap()                 const { return _oopmap; }
  size_t oopmap_size_in_bits()   const { return _oopmap_size_in_bits; }

  void set_file_offset(size_t s) { _file_offset = s; }
  void set_read_only(bool v)     { _read_only = v; }
  void mark_invalid()            { _addr._base = NULL; }

  void init(bool is_heap_region, char* base, size_t size, bool read_only,
            bool allow_exec, int crc);

  void init_oopmap(void* map, size_t size_in_bits) {
    _oopmap = map;
    _oopmap_size_in_bits = size_in_bits;
  }
};

class FileMapHeader: private CDSFileMapHeaderBase {
  friend class CDSOffsets;
  friend class VMStructs;

  size_t _header_size;

  // The following fields record the states of the VM during dump time.
  // They are compared with the runtime states to see if the archive
  // can be used.
  size_t _alignment;                // how shared archive should be aligned
  int    _obj_alignment;            // value of ObjectAlignmentInBytes
  address _narrow_oop_base;         // compressed oop encoding base
  int    _narrow_oop_shift;         // compressed oop encoding shift
  bool   _compact_strings;          // value of CompactStrings
  uintx  _max_heap_size;            // java max heap size during dumping
  CompressedOops::Mode _narrow_oop_mode; // compressed oop encoding mode
  int     _narrow_klass_shift;      // save narrow klass base and shift
  address _narrow_klass_base;


  char*   _misc_data_patching_start;
  char*   _read_only_tables_start;
  address _i2i_entry_code_buffers;
  size_t  _i2i_entry_code_buffers_size;
  size_t  _core_spaces_size;        // number of bytes allocated by the core spaces
                                    // (mc, md, ro, rw and od).
  address _heap_end;                // heap end at dump time.
  bool _base_archive_is_default;    // indicates if the base archive is the system default one

  // The following fields are all sanity checks for whether this archive
  // will function correctly with this JVM and the bootclasspath it's
  // invoked with.
  char  _jvm_ident[JVM_IDENT_MAX];  // identifier string of the jvm that created this dump

  // size of the base archive name including NULL terminator
  size_t _base_archive_name_size;

  // The following is a table of all the boot/app/module path entries that were used
  // during dumping. At run time, we validate these entries according to their
  // SharedClassPathEntry::_type. See:
  //      check_nonempty_dir_in_shared_path_table()
  //      validate_shared_path_table()
  //      validate_non_existent_class_paths()
  SharedPathTable _shared_path_table;

  jshort _app_class_paths_start_index;  // Index of first app classpath entry
  jshort _app_module_paths_start_index; // Index of first module path entry
  jshort _num_module_paths;             // number of module path entries
  jshort _max_used_path_index;          // max path index referenced during CDS dump
  bool   _verify_local;                 // BytecodeVerificationLocal setting
  bool   _verify_remote;                // BytecodeVerificationRemote setting
  bool   _has_platform_or_app_classes;  // Archive contains app classes
  size_t _shared_base_address;          // SharedBaseAddress used at dump time
  bool   _allow_archiving_with_java_agent; // setting of the AllowArchivingWithJavaAgent option

public:
  // Accessors -- fields declared in CDSFileMapHeaderBase
  unsigned int magic() const {return _magic;}
  int crc()                               const { return _crc; }
  int version()                           const { return _version; }

  void set_crc(int crc_value)                   { _crc = crc_value; }
  void set_version(int v)                       { _version = v; }

  // Accessors -- fields declared in FileMapHeader

  size_t header_size()                     const { return _header_size; }
  size_t alignment()                       const { return _alignment; }
  int obj_alignment()                      const { return _obj_alignment; }
  address narrow_oop_base()                const { return _narrow_oop_base; }
  int narrow_oop_shift()                   const { return _narrow_oop_shift; }
  bool compact_strings()                   const { return _compact_strings; }
  uintx max_heap_size()                    const { return _max_heap_size; }
  CompressedOops::Mode narrow_oop_mode()   const { return _narrow_oop_mode; }
  int narrow_klass_shift()                 const { return _narrow_klass_shift; }
  address narrow_klass_base()              const { return _narrow_klass_base; }
  char* misc_data_patching_start()         const { return _misc_data_patching_start; }
  char* read_only_tables_start()           const { return _read_only_tables_start; }
  address i2i_entry_code_buffers()         const { return _i2i_entry_code_buffers; }
  size_t i2i_entry_code_buffers_size()     const { return _i2i_entry_code_buffers_size; }
  size_t core_spaces_size()                const { return _core_spaces_size; }
  address heap_end()                       const { return _heap_end; }
  bool base_archive_is_default()           const { return _base_archive_is_default; }
  const char* jvm_ident()                  const { return _jvm_ident; }
  size_t base_archive_name_size()          const { return _base_archive_name_size; }
  size_t shared_base_address()             const { return _shared_base_address; }
  bool has_platform_or_app_classes()       const { return _has_platform_or_app_classes; }
  SharedPathTable shared_path_table()      const { return _shared_path_table; }

  // FIXME: These should really return int
  jshort max_used_path_index()             const { return _max_used_path_index; }
  jshort app_module_paths_start_index()    const { return _app_module_paths_start_index; }
  jshort app_class_paths_start_index()     const { return _app_class_paths_start_index; }
  jshort num_module_paths()                const { return _num_module_paths; }

  void set_core_spaces_size(size_t s)            { _core_spaces_size = s; }
  void set_has_platform_or_app_classes(bool v)   { _has_platform_or_app_classes = v; }
  void set_misc_data_patching_start(char* p)     { _misc_data_patching_start = p; }
  void set_read_only_tables_start(char* p)       { _read_only_tables_start   = p; }
  void set_base_archive_name_size(size_t s)      { _base_archive_name_size = s; }
  void set_base_archive_is_default(bool b)       { _base_archive_is_default = b; }
  void set_header_size(size_t s)                 { _header_size = s; }

  void set_i2i_entry_code_buffers(address p, size_t s) {
    _i2i_entry_code_buffers = p;
    _i2i_entry_code_buffers_size = s;
  }

  void relocate_shared_path_table(Array<u8>* t) {
    assert(DynamicDumpSharedSpaces, "only");
    _shared_path_table.set_table(t);
  }

  void shared_path_table_metaspace_pointers_do(MetaspaceClosure* it) {
    assert(DynamicDumpSharedSpaces, "only");
    _shared_path_table.metaspace_pointers_do(it);
  }

  bool validate();
  int compute_crc();

  FileMapRegion* space_at(int i) {
    assert(is_valid_region(i), "invalid region");
    return FileMapRegion::cast(&_space[i]);
  }

  void populate(FileMapInfo* info, size_t alignment);

  static bool is_valid_region(int region) {
    return (0 <= region && region < NUM_CDS_REGIONS);
  }
};

class FileMapInfo : public CHeapObj<mtInternal> {
private:
  friend class ManifestStream;
  friend class VMStructs;
  friend class CDSOffsets;
  friend class FileMapHeader;

  bool           _is_static;
  bool           _file_open;
  int            _fd;
  size_t         _file_offset;
  const char*    _full_path;
  const char*    _base_archive_name;
  FileMapHeader* _header;

  // TODO: Probably change the following to be non-static
  static SharedPathTable       _shared_path_table;
  static bool                  _validating_shared_path_table;

  // FileMapHeader describes the shared space data in the file to be
  // mapped.  This structure gets written to a file.  It is not a class, so
  // that the compilers don't add any compiler-private data to it.

  static FileMapInfo* _current_info;
  static FileMapInfo* _dynamic_archive_info;
  static bool _heap_pointers_need_patching;
  static bool _memory_mapping_failed;
  static GrowableArray<const char*>* _non_existent_class_paths;

  FileMapHeader *header() const       { return _header; }

public:
  static bool get_base_archive_name_from_header(const char* archive_name,
                                                int* size, char** base_archive_name);
  static bool check_archive(const char* archive_name, bool is_static);
  void restore_shared_path_table();
  bool init_from_file(int fd, bool is_static);
  static void metaspace_pointers_do(MetaspaceClosure* it);

  void log_paths(const char* msg, int start_idx, int end_idx);

  FileMapInfo(bool is_static);
  ~FileMapInfo();

  // Accessors
  int    compute_header_crc()  const { return header()->compute_crc(); }
  void   set_header_crc(int crc)     { header()->set_crc(crc); }
  int    space_crc(int i)      const { return space_at(i)->crc(); }
  void   populate_header(size_t alignment);
  bool   validate_header(bool is_static);
  void   invalidate();
  int    crc()                 const { return header()->crc(); }
  int    version()             const { return header()->version(); }
  size_t alignment()           const { return header()->alignment(); }
  address narrow_oop_base()    const { return header()->narrow_oop_base(); }
  int     narrow_oop_shift()   const { return header()->narrow_oop_shift(); }
  uintx   max_heap_size()      const { return header()->max_heap_size(); }
  address narrow_klass_base()  const { return header()->narrow_klass_base(); }
  int     narrow_klass_shift() const { return header()->narrow_klass_shift(); }

  CompressedOops::Mode narrow_oop_mode()      const { return header()->narrow_oop_mode(); }
  jshort app_module_paths_start_index()       const { return header()->app_module_paths_start_index(); }
  jshort app_class_paths_start_index()        const { return header()->app_class_paths_start_index(); }

  char* misc_data_patching_start()            const { return header()->misc_data_patching_start(); }
  void  set_misc_data_patching_start(char* p) const { header()->set_misc_data_patching_start(p); }
  char* read_only_tables_start()              const { return header()->read_only_tables_start(); }
  void  set_read_only_tables_start(char* p)   const { header()->set_read_only_tables_start(p); }

  bool  is_file_position_aligned() const;
  void  align_file_position();

  address i2i_entry_code_buffers()            const { return header()->i2i_entry_code_buffers();  }
  size_t i2i_entry_code_buffers_size()        const { return header()->i2i_entry_code_buffers_size(); }
  void set_i2i_entry_code_buffers(address addr, size_t s) const {
    header()->set_i2i_entry_code_buffers(addr, s);
  }

  void set_core_spaces_size(size_t s)         const { header()->set_core_spaces_size(s); }
  size_t core_spaces_size()                   const { return header()->core_spaces_size(); }

  class DynamicArchiveHeader* dynamic_header() const {
    assert(!_is_static, "must be");
    return (DynamicArchiveHeader*)header();
  }

  void set_has_platform_or_app_classes(bool v) {
    header()->set_has_platform_or_app_classes(v);
  }
  bool has_platform_or_app_classes() const {
    return header()->has_platform_or_app_classes();
  }

  static FileMapInfo* current_info() {
    CDS_ONLY(return _current_info;)
    NOT_CDS(return NULL;)
  }

  static void set_current_info(FileMapInfo* info) {
    CDS_ONLY(_current_info = info;)
  }

  static FileMapInfo* dynamic_info() {
    CDS_ONLY(return _dynamic_archive_info;)
    NOT_CDS(return NULL;)
  }

  static void assert_mark(bool check);

  // File manipulation.
  bool  initialize(bool is_static) NOT_CDS_RETURN_(false);
  bool  open_for_read(const char* path = NULL);
  void  open_for_write(const char* path = NULL);
  void  write_header();
  void  write_region(int region, char* base, size_t size,
                     bool read_only, bool allow_exec);
  size_t write_archive_heap_regions(GrowableArray<MemRegion> *heap_mem,
                                    GrowableArray<ArchiveHeapOopmapInfo> *oopmaps,
                                    int first_region_id, int max_num_regions,
                                    bool print_log);
  void  write_bytes(const void* buffer, size_t count);
  void  write_bytes_aligned(const void* buffer, size_t count);
  size_t  read_bytes(void* buffer, size_t count);
  char* map_regions(int regions[], char* saved_base[], size_t len);
  char* map_region(int i, char** top_ret);
  void  map_heap_regions_impl() NOT_CDS_JAVA_HEAP_RETURN;
  void  map_heap_regions() NOT_CDS_JAVA_HEAP_RETURN;
  void  fixup_mapped_heap_regions() NOT_CDS_JAVA_HEAP_RETURN;
  void  patch_archived_heap_embedded_pointers() NOT_CDS_JAVA_HEAP_RETURN;
  void  patch_archived_heap_embedded_pointers(MemRegion* ranges, int num_ranges,
                                              int first_region_idx) NOT_CDS_JAVA_HEAP_RETURN;
  bool  has_heap_regions()  NOT_CDS_JAVA_HEAP_RETURN_(false);
  MemRegion get_heap_regions_range_with_current_oop_encoding_mode() NOT_CDS_JAVA_HEAP_RETURN_(MemRegion());
  void  unmap_regions(int regions[], char* saved_base[], size_t len);
  void  unmap_region(int i);
  bool  verify_region_checksum(int i);
  void  close();
  bool  is_open() { return _file_open; }
  ReservedSpace reserve_shared_memory();

  // JVM/TI RedefineClasses() support:
  // Remap the shared readonly space to shared readwrite, private.
  bool  remap_shared_readonly_as_readwrite();

  // Errors.
  static void fail_stop(const char *msg, ...) ATTRIBUTE_PRINTF(1, 2);
  static void fail_continue(const char *msg, ...) ATTRIBUTE_PRINTF(1, 2);
  static bool memory_mapping_failed() {
    CDS_ONLY(return _memory_mapping_failed;)
    NOT_CDS(return false;)
  }
  bool is_in_shared_region(const void* p, int idx) NOT_CDS_RETURN_(false);

  // Stop CDS sharing and unmap CDS regions.
  static void stop_sharing_and_unmap(const char* msg);

  static void allocate_shared_path_table();
  static int add_shared_classpaths(int i, const char* which, ClassPathEntry *cpe, TRAPS);
  static void check_nonempty_dir_in_shared_path_table();
  bool validate_shared_path_table();
  void validate_non_existent_class_paths();
  static void update_jar_manifest(ClassPathEntry *cpe, SharedClassPathEntry* ent, TRAPS);
  static int num_non_existent_class_paths();
  static void record_non_existent_class_path_entry(const char* path);

#if INCLUDE_JVMTI
  static ClassFileStream* open_stream_for_jvmti(InstanceKlass* ik, Handle class_loader, TRAPS);
#endif

  static SharedClassPathEntry* shared_path(int index) {
    return _shared_path_table.path_at(index);
  }

  static const char* shared_path_name(int index) {
    assert(index >= 0, "Sanity");
    return shared_path(index)->name();
  }

  static int get_number_of_shared_paths() {
    return _shared_path_table.size();
  }

  char* region_addr(int idx);

 private:
  char* skip_first_path_entry(const char* path) NOT_CDS_RETURN_(NULL);
  int   num_paths(const char* path) NOT_CDS_RETURN_(0);
  GrowableArray<const char*>* create_path_array(const char* path) NOT_CDS_RETURN_(NULL);
  bool  fail(const char* msg, const char* name) NOT_CDS_RETURN_(false);
  bool  check_paths(int shared_path_start_idx, int num_paths,
                    GrowableArray<const char*>* rp_array) NOT_CDS_RETURN_(false);
  bool  validate_boot_class_paths() NOT_CDS_RETURN_(false);
  bool  validate_app_class_paths(int shared_app_paths_len) NOT_CDS_RETURN_(false);
  bool  map_heap_data(MemRegion **heap_mem, int first, int max, int* num,
                      bool is_open = false) NOT_CDS_JAVA_HEAP_RETURN_(false);
  bool  region_crc_check(char* buf, size_t size, int expected_crc) NOT_CDS_RETURN_(false);
  void  dealloc_archive_heap_regions(MemRegion* regions, int num, bool is_open) NOT_CDS_JAVA_HEAP_RETURN;

  FileMapRegion* space_at(int i) const {
    return header()->space_at(i);
  }

  // The starting address of spc, as calculated with CompressedOop::decode_non_null()
  address start_address_as_decoded_with_current_oop_encoding_mode(FileMapRegion* spc) {
    return decode_start_address(spc, true);
  }

  // The starting address of spc, as calculated with HeapShared::decode_from_archive()
  address start_address_as_decoded_from_archive(FileMapRegion* spc) {
    return decode_start_address(spc, false);
  }

  address decode_start_address(FileMapRegion* spc, bool with_current_oop_encoding_mode);

#if INCLUDE_JVMTI
  static ClassPathEntry** _classpath_entries_for_jvmti;
  static ClassPathEntry* get_classpath_entry_for_jvmti(int i, TRAPS);
#endif
};

#endif // SHARE_MEMORY_FILEMAP_HPP
