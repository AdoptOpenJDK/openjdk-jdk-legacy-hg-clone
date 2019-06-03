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
#include "runtime/flags/jvmFlagWriteableList.hpp"
#include "runtime/os.hpp"

bool JVMFlagWriteable::is_writeable(void) {
  return _writeable;
}

void JVMFlagWriteable::mark_once(void) {
  if (_type == Once) {
    _writeable = false;
  }
}

void JVMFlagWriteable::mark_startup(void) {
  if (_type == JVMFlagWriteable::CommandLineOnly) {
    _writeable = false;
  }
}

// No control emitting
void emit_writeable_no(...)                         { /* NOP */ }

// No control emitting if type argument is NOT provided
void emit_writeable_bool(const char* /*name*/)      { /* NOP */ }
void emit_writeable_ccstr(const char* /*name*/)     { /* NOP */ }
void emit_writeable_ccstrlist(const char* /*name*/) { /* NOP */ }
void emit_writeable_int(const char* /*name*/)       { /* NOP */ }
void emit_writeable_intx(const char* /*name*/)      { /* NOP */ }
void emit_writeable_uint(const char* /*name*/)      { /* NOP */ }
void emit_writeable_uintx(const char* /*name*/)     { /* NOP */ }
void emit_writeable_uint64_t(const char* /*name*/)  { /* NOP */ }
void emit_writeable_size_t(const char* /*name*/)    { /* NOP */ }
void emit_writeable_double(const char* /*name*/)    { /* NOP */ }

// JVMFlagWriteable emitting code functions if range arguments are provided
void emit_writeable_bool(const char* name, JVMFlagWriteable::WriteableType type) {
  JVMFlagWriteableList::add(new JVMFlagWriteable(name, type));
}
void emit_writeable_int(const char* name, JVMFlagWriteable::WriteableType type) {
  JVMFlagWriteableList::add(new JVMFlagWriteable(name, type));
}
void emit_writeable_intx(const char* name, JVMFlagWriteable::WriteableType type) {
  JVMFlagWriteableList::add(new JVMFlagWriteable(name, type));
}
void emit_writeable_uint(const char* name, JVMFlagWriteable::WriteableType type) {
  JVMFlagWriteableList::add(new JVMFlagWriteable(name, type));
}
void emit_writeable_uintx(const char* name, JVMFlagWriteable::WriteableType type) {
  JVMFlagWriteableList::add(new JVMFlagWriteable(name, type));
}
void emit_writeable_uint64_t(const char* name, JVMFlagWriteable::WriteableType type) {
  JVMFlagWriteableList::add(new JVMFlagWriteable(name, type));
}
void emit_writeable_size_t(const char* name, JVMFlagWriteable::WriteableType type) {
  JVMFlagWriteableList::add(new JVMFlagWriteable(name, type));
}
void emit_writeable_double(const char* name, JVMFlagWriteable::WriteableType type) {
  JVMFlagWriteableList::add(new JVMFlagWriteable(name, type));
}

// Generate code to call emit_writeable_xxx function
#define EMIT_WRITEABLE_START       (void)(0
#define EMIT_WRITEABLE(type, name) ); emit_writeable_##type(#name
#define EMIT_WRITEABLE_PRODUCT_FLAG(type, name, value, doc)      EMIT_WRITEABLE(type, name)
#define EMIT_WRITEABLE_DIAGNOSTIC_FLAG(type, name, value, doc)   EMIT_WRITEABLE(type, name)
#define EMIT_WRITEABLE_EXPERIMENTAL_FLAG(type, name, value, doc) EMIT_WRITEABLE(type, name)
#define EMIT_WRITEABLE_MANAGEABLE_FLAG(type, name, value, doc)   EMIT_WRITEABLE(type, name)
#define EMIT_WRITEABLE_PRODUCT_RW_FLAG(type, name, value, doc)   EMIT_WRITEABLE(type, name)
#define EMIT_WRITEABLE_PD_PRODUCT_FLAG(type, name, doc)          EMIT_WRITEABLE(type, name)
#define EMIT_WRITEABLE_DEVELOPER_FLAG(type, name, value, doc)    EMIT_WRITEABLE(type, name)
#define EMIT_WRITEABLE_PD_DEVELOPER_FLAG(type, name, doc)        EMIT_WRITEABLE(type, name)
#define EMIT_WRITEABLE_PD_DIAGNOSTIC_FLAG(type, name, doc)       EMIT_WRITEABLE(type, name)
#define EMIT_WRITEABLE_NOTPRODUCT_FLAG(type, name, value, doc)   EMIT_WRITEABLE(type, name)
#define EMIT_WRITEABLE_LP64_PRODUCT_FLAG(type, name, value, doc) EMIT_WRITEABLE(type, name)
#define EMIT_WRITEABLE_END         );

// Generate type argument to pass into emit_writeable_xxx functions
#define EMIT_WRITEABLE_CHECK(a)                                  , JVMFlagWriteable::a

#define INITIAL_WRITEABLES_SIZE 2
GrowableArray<JVMFlagWriteable*>* JVMFlagWriteableList::_controls = NULL;

void JVMFlagWriteableList::init(void) {

  _controls = new (ResourceObj::C_HEAP, mtArguments) GrowableArray<JVMFlagWriteable*>(INITIAL_WRITEABLES_SIZE, true);

  EMIT_WRITEABLE_START

  ALL_FLAGS(EMIT_WRITEABLE_DEVELOPER_FLAG,
            EMIT_WRITEABLE_PD_DEVELOPER_FLAG,
            EMIT_WRITEABLE_PRODUCT_FLAG,
            EMIT_WRITEABLE_PD_PRODUCT_FLAG,
            EMIT_WRITEABLE_DIAGNOSTIC_FLAG,
            EMIT_WRITEABLE_PD_DIAGNOSTIC_FLAG,
            EMIT_WRITEABLE_EXPERIMENTAL_FLAG,
            EMIT_WRITEABLE_NOTPRODUCT_FLAG,
            EMIT_WRITEABLE_MANAGEABLE_FLAG,
            EMIT_WRITEABLE_PRODUCT_RW_FLAG,
            EMIT_WRITEABLE_LP64_PRODUCT_FLAG,
            IGNORE_RANGE,
            IGNORE_CONSTRAINT,
            EMIT_WRITEABLE_CHECK)

  EMIT_WRITEABLES_FOR_GLOBALS_EXT

  EMIT_WRITEABLE_END
}

JVMFlagWriteable* JVMFlagWriteableList::find(const char* name) {
  JVMFlagWriteable* found = NULL;
  for (int i=0; i<length(); i++) {
    JVMFlagWriteable* writeable = at(i);
    if (strcmp(writeable->name(), name) == 0) {
      found = writeable;
      break;
    }
  }
  return found;
}

void JVMFlagWriteableList::mark_startup(void) {
  for (int i=0; i<length(); i++) {
    JVMFlagWriteable* writeable = at(i);
    writeable->mark_startup();
  }
}
