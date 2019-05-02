/*
 * Copyright (c) 2018, 2019, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_GC_SHARED_PARALLELCLEANING_HPP
#define SHARE_GC_SHARED_PARALLELCLEANING_HPP

#include "classfile/classLoaderDataGraph.hpp"
#include "code/codeCache.hpp"
#include "gc/shared/oopStorageParState.hpp"
#include "gc/shared/stringdedup/stringDedup.hpp"
#include "gc/shared/workgroup.hpp"

class ParallelCleaningTask;

class StringDedupCleaningTask : public AbstractGangTask {
  StringDedupUnlinkOrOopsDoClosure _dedup_closure;

public:
  StringDedupCleaningTask(BoolObjectClosure* is_alive, OopClosure* keep_alive, bool resize_table);
  ~StringDedupCleaningTask();

  void work(uint worker_id);
};

class CodeCacheUnloadingTask {

  CodeCache::UnloadingScope _unloading_scope;
  const bool                _unloading_occurred;
  const uint                _num_workers;

  // Variables used to claim nmethods.
  CompiledMethod* _first_nmethod;
  CompiledMethod* volatile _claimed_nmethod;

public:
  CodeCacheUnloadingTask(uint num_workers, BoolObjectClosure* is_alive, bool unloading_occurred);
  ~CodeCacheUnloadingTask();

private:
  static const int MaxClaimNmethods = 16;
  void claim_nmethods(CompiledMethod** claimed_nmethods, int *num_claimed_nmethods);

public:
  // Cleaning and unloading of nmethods.
  void work(uint worker_id);
};


class KlassCleaningTask : public StackObj {
  volatile int                            _clean_klass_tree_claimed;
  ClassLoaderDataGraphKlassIteratorAtomic _klass_iterator;

public:
  KlassCleaningTask();

private:
  bool claim_clean_klass_tree_task();
  InstanceKlass* claim_next_klass();

public:

  void clean_klass(InstanceKlass* ik) {
    ik->clean_weak_instanceklass_links();
  }

  void work();
};

#if INCLUDE_JVMCI
class JVMCICleaningTask : public StackObj {
  volatile int       _cleaning_claimed;

public:
  JVMCICleaningTask();
  // Clean JVMCI metadata handles.
  void work(bool unloading_occurred);

private:
  bool claim_cleaning_task();
};
#endif

// Do cleanup of some weakly held data in the same parallel task.
// Assumes a non-moving context.
class ParallelCleaningTask : public AbstractGangTask {
private:
  bool                    _unloading_occurred;
  StringDedupCleaningTask _string_dedup_task;
  CodeCacheUnloadingTask  _code_cache_task;
#if INCLUDE_JVMCI
  JVMCICleaningTask       _jvmci_cleaning_task;
#endif
  KlassCleaningTask       _klass_cleaning_task;

public:
  // The constructor is run in the VMThread.
  ParallelCleaningTask(BoolObjectClosure* is_alive,
                       uint num_workers,
                       bool unloading_occurred,
                       bool resize_dedup_table);

  void work(uint worker_id);
};

#endif // SHARE_GC_SHARED_PARALLELCLEANING_HPP
