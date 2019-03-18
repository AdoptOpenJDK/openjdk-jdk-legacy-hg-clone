/*
 * Copyright (c) 2015, 2019, Oracle and/or its affiliates. All rights reserved.
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
 */

#include "precompiled.hpp"
#include "gc/z/zForwarding.inline.hpp"
#include "gc/z/zUtils.inline.hpp"
#include "memory/allocation.hpp"
#include "utilities/debug.hpp"

ZForwarding::ZForwarding(uintptr_t start, size_t object_alignment_shift, uint32_t nentries) :
    _start(start),
    _object_alignment_shift(object_alignment_shift),
    _nentries(nentries),
    _refcount(1),
    _pinned(false) {}

ZForwarding* ZForwarding::create(uintptr_t start, size_t object_alignment_shift, uint32_t live_objects) {
  assert(live_objects > 0, "Invalid value");

  // Allocate table for linear probing. The size of the table must be
  // a power of two to allow for quick and inexpensive indexing/masking.
  // The table is sized to have a load factor of 50%, i.e. sized to have
  // double the number of entries actually inserted.
  const uint32_t nentries = ZUtils::round_up_power_of_2(live_objects * 2);

  const size_t size = sizeof(ZForwarding) + (sizeof(ZForwardingEntry) * nentries);
  uint8_t* const addr = NEW_C_HEAP_ARRAY(uint8_t, size, mtGC);
  ZForwardingEntry* const entries = ::new (addr + sizeof(ZForwarding)) ZForwardingEntry[nentries];
  ZForwarding* const forwarding = ::new (addr) ZForwarding(start, object_alignment_shift, nentries);
  return forwarding;
}

void ZForwarding::destroy(ZForwarding* forwarding) {
  FREE_C_HEAP_ARRAY(uint8_t, forwarding);
}

void ZForwarding::verify(uint32_t object_max_count, uint32_t live_objects) const {
  uint32_t count = 0;

  for (ZForwardingCursor i = 0; i < _nentries; i++) {
    const ZForwardingEntry entry = at(&i);
    if (entry.is_empty()) {
      // Skip empty entries
      continue;
    }

    // Check from index
    guarantee(entry.from_index() < object_max_count, "Invalid from index");

    // Check for duplicates
    for (ZForwardingCursor j = i + 1; j < _nentries; j++) {
      const ZForwardingEntry other = at(&j);
      guarantee(entry.from_index() != other.from_index(), "Duplicate from");
      guarantee(entry.to_offset() != other.to_offset(), "Duplicate to");
    }

    count++;
  }

  // Check number of non-null entries
  guarantee(live_objects == count, "Count mismatch");
}
