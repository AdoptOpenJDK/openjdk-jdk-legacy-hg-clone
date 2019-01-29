/*
 * Copyright (c) 2015, 2018, Red Hat, Inc. All rights reserved.
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

#ifndef SHARE_GC_SHENANDOAH_SHENANDOAHBARRIERSET_INLINE_HPP
#define SHARE_GC_SHENANDOAH_SHENANDOAHBARRIERSET_INLINE_HPP

#include "gc/shared/barrierSet.hpp"
#include "gc/shenandoah/shenandoahBarrierSet.hpp"
#include "gc/shenandoah/shenandoahBrooksPointer.inline.hpp"
#include "gc/shenandoah/shenandoahHeap.inline.hpp"

bool ShenandoahBarrierSet::need_update_refs_barrier() {
  return _heap->is_update_refs_in_progress() ||
         _heap->is_concurrent_traversal_in_progress() ||
         (_heap->is_concurrent_mark_in_progress() && _heap->has_forwarded_objects());
}

inline oop ShenandoahBarrierSet::resolve_forwarded_not_null(oop p) {
  return ShenandoahBrooksPointer::forwardee(p);
}

inline oop ShenandoahBarrierSet::resolve_forwarded(oop p) {
  if (((HeapWord*) p) != NULL) {
    return resolve_forwarded_not_null(p);
  } else {
    return p;
  }
}

template <DecoratorSet decorators, typename BarrierSetT>
template <typename T>
inline oop ShenandoahBarrierSet::AccessBarrier<decorators, BarrierSetT>::oop_atomic_cmpxchg_in_heap(oop new_value, T* addr, oop compare_value) {
  oop res;
  oop expected = compare_value;
  do {
    compare_value = expected;
    res = Raw::oop_atomic_cmpxchg(new_value, addr, compare_value);
    expected = res;
  } while ((! oopDesc::equals_raw(compare_value, expected)) && oopDesc::equals_raw(resolve_forwarded(compare_value), resolve_forwarded(expected)));
  if (oopDesc::equals_raw(expected, compare_value)) {
    if (ShenandoahSATBBarrier && !CompressedOops::is_null(compare_value)) {
      ShenandoahBarrierSet::barrier_set()->enqueue(compare_value);
    }
  }
  return res;
}

template <DecoratorSet decorators, typename BarrierSetT>
template <typename T>
inline oop ShenandoahBarrierSet::AccessBarrier<decorators, BarrierSetT>::oop_atomic_xchg_in_heap(oop new_value, T* addr) {
  oop previous = Raw::oop_atomic_xchg(new_value, addr);
  if (ShenandoahSATBBarrier) {
    if (!CompressedOops::is_null(previous)) {
      ShenandoahBarrierSet::barrier_set()->enqueue(previous);
    }
  }
  return previous;
}

template <DecoratorSet decorators, typename BarrierSetT>
template <typename T>
void ShenandoahBarrierSet::AccessBarrier<decorators, BarrierSetT>::arraycopy_in_heap(arrayOop src_obj, size_t src_offset_in_bytes, T* src_raw,
                                                                                     arrayOop dst_obj, size_t dst_offset_in_bytes, T* dst_raw,
                                                                                     size_t length) {
  if (!CompressedOops::is_null(src_obj)) {
    src_obj = arrayOop(ShenandoahBarrierSet::barrier_set()->read_barrier(src_obj));
  }
  if (!CompressedOops::is_null(dst_obj)) {
    dst_obj = arrayOop(ShenandoahBarrierSet::barrier_set()->write_barrier(dst_obj));
  }
  Raw::arraycopy(src_obj, src_offset_in_bytes, src_raw, dst_obj, dst_offset_in_bytes, dst_raw, length);
}

template <typename T>
bool ShenandoahBarrierSet::arraycopy_loop_1(T* src, T* dst, size_t length, Klass* bound,
                                            bool checkcast, bool satb, bool disjoint,
                                            ShenandoahBarrierSet::ArrayCopyStoreValMode storeval_mode) {
  if (checkcast) {
    return arraycopy_loop_2<T, true>(src, dst, length, bound, satb, disjoint, storeval_mode);
  } else {
    return arraycopy_loop_2<T, false>(src, dst, length, bound, satb, disjoint, storeval_mode);
  }
}

template <typename T, bool CHECKCAST>
bool ShenandoahBarrierSet::arraycopy_loop_2(T* src, T* dst, size_t length, Klass* bound,
                                            bool satb, bool disjoint,
                                            ShenandoahBarrierSet::ArrayCopyStoreValMode storeval_mode) {
  if (satb) {
    return arraycopy_loop_3<T, CHECKCAST, true>(src, dst, length, bound, disjoint, storeval_mode);
  } else {
    return arraycopy_loop_3<T, CHECKCAST, false>(src, dst, length, bound, disjoint, storeval_mode);
  }
}

template <typename T, bool CHECKCAST, bool SATB>
bool ShenandoahBarrierSet::arraycopy_loop_3(T* src, T* dst, size_t length, Klass* bound, bool disjoint,
                                            ShenandoahBarrierSet::ArrayCopyStoreValMode storeval_mode) {
  switch (storeval_mode) {
    case NONE:
      return arraycopy_loop<T, CHECKCAST, SATB, NONE>(src, dst, length, bound, disjoint);
    case READ_BARRIER:
      return arraycopy_loop<T, CHECKCAST, SATB, READ_BARRIER>(src, dst, length, bound, disjoint);
    case WRITE_BARRIER:
      return arraycopy_loop<T, CHECKCAST, SATB, WRITE_BARRIER>(src, dst, length, bound, disjoint);
    default:
      ShouldNotReachHere();
      return true; // happy compiler
  }
}

template <typename T, bool CHECKCAST, bool SATB, ShenandoahBarrierSet::ArrayCopyStoreValMode STOREVAL_MODE>
bool ShenandoahBarrierSet::arraycopy_loop(T* src, T* dst, size_t length, Klass* bound, bool disjoint) {
  Thread* thread = Thread::current();

  ShenandoahEvacOOMScope oom_evac_scope;

  // We need to handle four cases:
  //
  // a) src < dst, conjoint, can only copy backward only
  //   [...src...]
  //         [...dst...]
  //
  // b) src < dst, disjoint, can only copy forward, because types may mismatch
  //   [...src...]
  //              [...dst...]
  //
  // c) src > dst, conjoint, can copy forward only
  //         [...src...]
  //   [...dst...]
  //
  // d) src > dst, disjoint, can only copy forward, because types may mismatch
  //              [...src...]
  //   [...dst...]
  //
  if (src > dst || disjoint) {
    // copy forward:
    T* cur_src = src;
    T* cur_dst = dst;
    T* src_end = src + length;
    for (; cur_src < src_end; cur_src++, cur_dst++) {
      if (!arraycopy_element<T, CHECKCAST, SATB, STOREVAL_MODE>(cur_src, cur_dst, bound, thread)) {
        return false;
      }
    }
  } else {
    // copy backward:
    T* cur_src = src + length - 1;
    T* cur_dst = dst + length - 1;
    for (; cur_src >= src; cur_src--, cur_dst--) {
      if (!arraycopy_element<T, CHECKCAST, SATB, STOREVAL_MODE>(cur_src, cur_dst, bound, thread)) {
        return false;
      }
    }
  }
  return true;
}

template <typename T, bool CHECKCAST, bool SATB, ShenandoahBarrierSet::ArrayCopyStoreValMode STOREVAL_MODE>
bool ShenandoahBarrierSet::arraycopy_element(T* cur_src, T* cur_dst, Klass* bound, Thread* thread) {
  T o = RawAccess<>::oop_load(cur_src);

  if (SATB) {
    T prev = RawAccess<>::oop_load(cur_dst);
    if (!CompressedOops::is_null(prev)) {
      oop prev_obj = CompressedOops::decode_not_null(prev);
      enqueue(prev_obj);
    }
  }

  if (!CompressedOops::is_null(o)) {
    oop obj = CompressedOops::decode_not_null(o);

    if (CHECKCAST) {
      assert(bound != NULL, "need element klass for checkcast");
      if (!oopDesc::is_instanceof_or_null(obj, bound)) {
        return false;
      }
    }

    switch (STOREVAL_MODE) {
    case NONE:
      break;
    case READ_BARRIER:
      obj = ShenandoahBarrierSet::resolve_forwarded_not_null(obj);
      break;
    case WRITE_BARRIER:
      if (_heap->in_collection_set(obj)) {
        oop forw = ShenandoahBarrierSet::resolve_forwarded_not_null(obj);
        if (oopDesc::equals_raw(forw, obj)) {
          forw = _heap->evacuate_object(forw, thread);
        }
        obj = forw;
      }
      enqueue(obj);
      break;
    default:
      ShouldNotReachHere();
    }

    RawAccess<IS_NOT_NULL>::oop_store(cur_dst, obj);
  } else {
    // Store null.
    RawAccess<>::oop_store(cur_dst, o);
  }
  return true;
}

// Clone barrier support
template <DecoratorSet decorators, typename BarrierSetT>
void ShenandoahBarrierSet::AccessBarrier<decorators, BarrierSetT>::clone_in_heap(oop src, oop dst, size_t size) {
  src = arrayOop(ShenandoahBarrierSet::barrier_set()->read_barrier(src));
  dst = arrayOop(ShenandoahBarrierSet::barrier_set()->write_barrier(dst));
  Raw::clone(src, dst, size);
  ShenandoahBarrierSet::barrier_set()->write_region(MemRegion((HeapWord*) dst, size));
}

template <DecoratorSet decorators, typename BarrierSetT>
template <typename T>
bool ShenandoahBarrierSet::AccessBarrier<decorators, BarrierSetT>::oop_arraycopy_in_heap(arrayOop src_obj, size_t src_offset_in_bytes, T* src_raw,
                                                                                         arrayOop dst_obj, size_t dst_offset_in_bytes, T* dst_raw,
                                                                                         size_t length) {
  ShenandoahHeap* heap = ShenandoahHeap::heap();
  if (!CompressedOops::is_null(src_obj)) {
    src_obj = arrayOop(ShenandoahBarrierSet::barrier_set()->read_barrier(src_obj));
  }
  if (!CompressedOops::is_null(dst_obj)) {
    dst_obj = arrayOop(ShenandoahBarrierSet::barrier_set()->write_barrier(dst_obj));
  }

  bool satb = ShenandoahSATBBarrier && heap->is_concurrent_mark_in_progress();
  bool checkcast = HasDecorator<decorators, ARRAYCOPY_CHECKCAST>::value;
  bool disjoint = HasDecorator<decorators, ARRAYCOPY_DISJOINT>::value;
  ArrayCopyStoreValMode storeval_mode;
  if (heap->has_forwarded_objects()) {
    if (heap->is_concurrent_traversal_in_progress()) {
      storeval_mode = WRITE_BARRIER;
    } else if (heap->is_concurrent_mark_in_progress() || heap->is_update_refs_in_progress()) {
      storeval_mode = READ_BARRIER;
    } else {
      assert(heap->is_idle() || heap->is_evacuation_in_progress(), "must not have anything in progress");
      storeval_mode = NONE; // E.g. during evac or outside cycle
    }
  } else {
    assert(heap->is_stable() || heap->is_concurrent_mark_in_progress(), "must not have anything in progress");
    storeval_mode = NONE;
  }

  if (!satb && !checkcast && storeval_mode == NONE) {
    // Short-circuit to bulk copy.
    return Raw::oop_arraycopy(src_obj, src_offset_in_bytes, src_raw, dst_obj, dst_offset_in_bytes, dst_raw, length);
  }

  src_raw = arrayOopDesc::obj_offset_to_raw(src_obj, src_offset_in_bytes, src_raw);
  dst_raw = arrayOopDesc::obj_offset_to_raw(dst_obj, dst_offset_in_bytes, dst_raw);

  Klass* bound = objArrayOop(dst_obj)->element_klass();
  ShenandoahBarrierSet* bs = ShenandoahBarrierSet::barrier_set();
  return bs->arraycopy_loop_1(src_raw, dst_raw, length, bound, checkcast, satb, disjoint, storeval_mode);
}

#endif // SHARE_GC_SHENANDOAH_SHENANDOAHBARRIERSET_INLINE_HPP
