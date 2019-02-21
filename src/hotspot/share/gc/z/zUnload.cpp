/*
 * Copyright (c) 2018, Oracle and/or its affiliates. All rights reserved.
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
#include "classfile/classLoaderDataGraph.hpp"
#include "classfile/systemDictionary.hpp"
#include "code/codeBehaviours.hpp"
#include "code/codeCache.hpp"
#include "code/dependencyContext.hpp"
#include "gc/shared/gcBehaviours.hpp"
#include "gc/shared/suspendibleThreadSet.hpp"
#include "gc/z/zLock.inline.hpp"
#include "gc/z/zNMethod.hpp"
#include "gc/z/zOopClosures.hpp"
#include "gc/z/zStat.hpp"
#include "gc/z/zUnload.hpp"
#include "oops/access.inline.hpp"

static const ZStatSubPhase ZSubPhaseConcurrentClassesUnload("Concurrent Classes Unload");

class ZIsUnloadingOopClosure : public OopClosure {
private:
  ZPhantomIsAliveObjectClosure _is_alive;
  bool                         _is_unloading;

public:
  ZIsUnloadingOopClosure() :
      _is_alive(),
      _is_unloading(false) {}

  virtual void do_oop(oop* p) {
    const oop o = RawAccess<>::oop_load(p);
    if (o != NULL && !_is_alive.do_object_b(o)) {
      _is_unloading = true;
    }
  }

  virtual void do_oop(narrowOop* p) {
    ShouldNotReachHere();
  }

  bool is_unloading() const {
    return _is_unloading;
  }
};

class ZIsUnloadingBehaviour : public IsUnloadingBehaviour {
private:
  bool is_unloading(nmethod* nm) const {
    ZIsUnloadingOopClosure cl;
    nm->oops_do(&cl, true /* allow_zombie */);
    return cl.is_unloading();
  }

public:
  virtual bool is_unloading(CompiledMethod* method) const {
    nmethod* const nm = method->as_nmethod();
    ZReentrantLock* const lock = ZNMethod::lock_for_nmethod(nm);
    if (lock == NULL) {
      return is_unloading(nm);
    } else {
      ZLocker<ZReentrantLock> locker(lock);
      return is_unloading(nm);
    }
  }
};

class ZCompiledICProtectionBehaviour : public CompiledICProtectionBehaviour {
public:
  virtual bool lock(CompiledMethod* method) {
    nmethod* const nm = method->as_nmethod();
    ZReentrantLock* const lock = ZNMethod::lock_for_nmethod(nm);
    if (lock != NULL) {
      lock->lock();
    }
    return true;
  }

  virtual void unlock(CompiledMethod* method) {
    nmethod* const nm = method->as_nmethod();
    ZReentrantLock* const lock = ZNMethod::lock_for_nmethod(nm);
    if (lock != NULL) {
      lock->unlock();
    }
  }

  virtual bool is_safe(CompiledMethod* method) {
    if (SafepointSynchronize::is_at_safepoint()) {
      return true;
    }

    nmethod* const nm = method->as_nmethod();
    ZReentrantLock* const lock = ZNMethod::lock_for_nmethod(nm);
    return lock == NULL || lock->is_owned();
  }
};

ZUnload::ZUnload(ZWorkers* workers) :
    _workers(workers) {

  if (!ClassUnloading) {
    return;
  }

  static ZIsUnloadingBehaviour is_unloading_behaviour;
  IsUnloadingBehaviour::set_current(&is_unloading_behaviour);

  static ZCompiledICProtectionBehaviour ic_protection_behaviour;
  CompiledICProtectionBehaviour::set_current(&ic_protection_behaviour);
}

void ZUnload::prepare() {
  if (!ClassUnloading) {
    return;
  }

  CodeCache::increment_unloading_cycle();
  DependencyContext::cleaning_start();
}

void ZUnload::unlink() {
  SuspendibleThreadSetJoiner sts;
  bool unloading_occurred;

  {
    MutexLockerEx ml(ClassLoaderDataGraph_lock);
    unloading_occurred = SystemDictionary::do_unloading(ZStatPhase::timer());
  }

  Klass::clean_weak_klass_links(unloading_occurred);

  ZNMethod::unlink(_workers, unloading_occurred);

  DependencyContext::cleaning_end();
}

void ZUnload::purge() {
  {
    SuspendibleThreadSetJoiner sts;
    ZNMethod::purge(_workers);
  }

  ClassLoaderDataGraph::purge();
  CodeCache::purge_exception_caches();
}

class ZUnloadRendezvousClosure : public ThreadClosure {
public:
  void do_thread(Thread* thread) {}
};

void ZUnload::unload() {
  if (!ClassUnloading) {
    return;
  }

  ZStatTimer timer(ZSubPhaseConcurrentClassesUnload);

  // Unlink stale metadata and nmethods
  unlink();

  // Make sure stale metadata and nmethods are no longer observable
  ZUnloadRendezvousClosure cl;
  Handshake::execute(&cl);

  // Purge stale metadata and nmethods that were unlinked
  purge();
}

void ZUnload::finish() {
  // Resize and verify metaspace
  MetaspaceGC::compute_new_size();
  MetaspaceUtils::verify_metrics();
}
