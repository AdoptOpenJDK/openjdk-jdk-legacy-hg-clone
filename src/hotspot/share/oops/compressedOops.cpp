/*
 * Copyright (c) 2019, Oracle and/or its affiliates. All rights reserved.
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
#include "aot/aotLoader.hpp"
#include "logging/log.hpp"
#include "logging/logStream.hpp"
#include "memory/memRegion.hpp"
#include "memory/universe.hpp"
#include "oops/compressedOops.hpp"
#include "gc/shared/collectedHeap.hpp"
#include "runtime/globals.hpp"

// For UseCompressedOops.
NarrowPtrStruct CompressedOops::_narrow_oop = { NULL, 0, true };

address CompressedOops::_narrow_ptrs_base;

// Choose the heap base address and oop encoding mode
// when compressed oops are used:
// Unscaled  - Use 32-bits oops without encoding when
//     NarrowOopHeapBaseMin + heap_size < 4Gb
// ZeroBased - Use zero based compressed oops with encoding when
//     NarrowOopHeapBaseMin + heap_size < 32Gb
// HeapBased - Use compressed oops with heap base + encoding.
void CompressedOops::initialize() {
#ifdef _LP64
  if (UseCompressedOops) {
    // Subtract a page because something can get allocated at heap base.
    // This also makes implicit null checking work, because the
    // memory+1 page below heap_base needs to cause a signal.
    // See needs_explicit_null_check.
    // Only set the heap base for compressed oops because it indicates
    // compressed oops for pstack code.
    if ((uint64_t)Universe::heap()->reserved_region().end() > UnscaledOopHeapMax) {
      // Didn't reserve heap below 4Gb.  Must shift.
      set_shift(LogMinObjAlignmentInBytes);
    }
    if ((uint64_t)Universe::heap()->reserved_region().end() <= OopEncodingHeapMax) {
      // Did reserve heap below 32Gb. Can use base == 0;
      set_base(0);
    }
    AOTLoader::set_narrow_oop_shift();

    set_ptrs_base(base());

    LogTarget(Info, gc, heap, coops) lt;
    if (lt.is_enabled()) {
      ResourceMark rm;
      LogStream ls(lt);
      print_mode(&ls);
    }

    // Tell tests in which mode we run.
    Arguments::PropertyList_add(new SystemProperty("java.vm.compressedOopsMode",
                                                   mode_to_string(mode()),
                                                   false));
  }
  // base() is one page below the heap.
  assert((intptr_t)base() <= (intptr_t)(Universe::heap()->base() - os::vm_page_size()) ||
         base() == NULL, "invalid value");
  assert(shift() == LogMinObjAlignmentInBytes ||
         shift() == 0, "invalid value");
#endif
}

void CompressedOops::set_base(address base) {
  assert(UseCompressedOops, "no compressed oops?");
  _narrow_oop._base    = base;
}

void CompressedOops::set_shift(int shift) {
  _narrow_oop._shift   = shift;
}

void CompressedOops::set_use_implicit_null_checks(bool use) {
  assert(UseCompressedOops, "no compressed ptrs?");
  _narrow_oop._use_implicit_null_checks   = use;
}

void CompressedOops::set_ptrs_base(address addr) {
  _narrow_ptrs_base = addr;
}

CompressedOops::Mode CompressedOops::mode() {
  if (base_disjoint()) {
    return DisjointBaseNarrowOop;
  }

  if (base() != 0) {
    return HeapBasedNarrowOop;
  }

  if (shift() != 0) {
    return ZeroBasedNarrowOop;
  }

  return UnscaledNarrowOop;
}

const char* CompressedOops::mode_to_string(Mode mode) {
  switch (mode) {
    case UnscaledNarrowOop:
      return "32-bit";
    case ZeroBasedNarrowOop:
      return "Zero based";
    case DisjointBaseNarrowOop:
      return "Non-zero disjoint base";
    case HeapBasedNarrowOop:
      return "Non-zero based";
    default:
      ShouldNotReachHere();
      return "";
  }
}

// Test whether bits of addr and possible offsets into the heap overlap.
bool CompressedOops::is_disjoint_heap_base_address(address addr) {
  return (((uint64_t)(intptr_t)addr) &
          (((uint64_t)UCONST64(0xFFFFffffFFFFffff)) >> (32-LogMinObjAlignmentInBytes))) == 0;
}

// Check for disjoint base compressed oops.
bool CompressedOops::base_disjoint() {
  return _narrow_oop._base != NULL && is_disjoint_heap_base_address(_narrow_oop._base);
}

// Check for real heapbased compressed oops.
// We must subtract the base as the bits overlap.
// If we negate above function, we also get unscaled and zerobased.
bool CompressedOops::base_overlaps() {
  return _narrow_oop._base != NULL && !is_disjoint_heap_base_address(_narrow_oop._base);
}

void CompressedOops::print_mode(outputStream* st) {
  st->print("Heap address: " PTR_FORMAT ", size: " SIZE_FORMAT " MB",
            p2i(Universe::heap()->base()), Universe::heap()->reserved_region().byte_size()/M);

  st->print(", Compressed Oops mode: %s", mode_to_string(mode()));

  if (base() != 0) {
    st->print(": " PTR_FORMAT, p2i(base()));
  }

  if (shift() != 0) {
    st->print(", Oop shift amount: %d", shift());
  }

  if (!use_implicit_null_checks()) {
    st->print(", no protected page in front of the heap");
  }
  st->cr();
}

// For UseCompressedClassPointers.
NarrowPtrStruct CompressedKlassPointers::_narrow_klass = { NULL, 0, true };

// CompressedClassSpaceSize set to 1GB, but appear 3GB away from _narrow_ptrs_base during CDS dump.
uint64_t CompressedKlassPointers::_narrow_klass_range = (uint64_t(max_juint)+1);;

void CompressedKlassPointers::set_base(address base) {
  assert(UseCompressedClassPointers, "no compressed klass ptrs?");
  _narrow_klass._base   = base;
}

void CompressedKlassPointers::set_shift(int shift)       {
  assert(shift == 0 || shift == LogKlassAlignmentInBytes, "invalid shift for klass ptrs");
  _narrow_klass._shift   = shift;
}

void CompressedKlassPointers::set_range(uint64_t range) {
  assert(UseCompressedClassPointers, "no compressed klass ptrs?");
  _narrow_klass_range = range;
}
