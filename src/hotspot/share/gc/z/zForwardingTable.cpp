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
 */

#include "precompiled.hpp"
#include "gc/z/zForwarding.inline.hpp"
#include "gc/z/zForwardingTable.inline.hpp"
#include "gc/z/zGlobals.hpp"
#include "gc/z/zGranuleMap.inline.hpp"
#include "utilities/debug.hpp"

ZForwardingTable::ZForwardingTable() :
    _map(ZAddressOffsetMax) {}

void ZForwardingTable::insert(ZForwarding* forwarding) {
  const uintptr_t offset = forwarding->start();
  const size_t size = forwarding->size();

  assert(_map.get(offset) == NULL, "Invalid entry");
  _map.put(offset, size, forwarding);
}

void ZForwardingTable::remove(ZForwarding* forwarding) {
  const uintptr_t offset = forwarding->start();
  const size_t size = forwarding->size();

  assert(_map.get(offset) == forwarding, "Invalid entry");
  _map.put(offset, size, NULL);
}
