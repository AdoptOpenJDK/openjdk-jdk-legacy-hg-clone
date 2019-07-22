/*
 * Copyright (c) 2001, 2019, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_GC_G1_G1DIRTYCARDQUEUE_HPP
#define SHARE_GC_G1_G1DIRTYCARDQUEUE_HPP

#include "gc/shared/ptrQueue.hpp"
#include "memory/allocation.hpp"

class G1CardTableEntryClosure;
class G1DirtyCardQueueSet;
class G1FreeIdSet;
class G1RedirtyCardsQueueSet;
class Thread;
class Monitor;

// A ptrQueue whose elements are "oops", pointers to object heads.
class G1DirtyCardQueue: public PtrQueue {
protected:
  virtual void handle_completed_buffer();

public:
  G1DirtyCardQueue(G1DirtyCardQueueSet* qset);

  // Flush before destroying; queue may be used to capture pending work while
  // doing something else, with auto-flush on completion.
  ~G1DirtyCardQueue();

  // Process queue entries and release resources.
  void flush() { flush_impl(); }

  inline G1DirtyCardQueueSet* dirty_card_qset() const;

  // Compiler support.
  static ByteSize byte_offset_of_index() {
    return PtrQueue::byte_offset_of_index<G1DirtyCardQueue>();
  }
  using PtrQueue::byte_width_of_index;

  static ByteSize byte_offset_of_buf() {
    return PtrQueue::byte_offset_of_buf<G1DirtyCardQueue>();
  }
  using PtrQueue::byte_width_of_buf;

};

class G1DirtyCardQueueSet: public PtrQueueSet {
  Monitor* _cbl_mon;  // Protects the fields below.
  BufferNode* _completed_buffers_head;
  BufferNode* _completed_buffers_tail;
  volatile size_t _n_completed_buffers;

  size_t _process_completed_buffers_threshold;
  volatile bool _process_completed_buffers;

  // If true, notify_all on _cbl_mon when the threshold is reached.
  bool _notify_when_complete;

  void assert_completed_buffers_list_len_correct_locked() NOT_DEBUG_RETURN;

  void abandon_completed_buffers();

  // Apply the closure to the elements of "node" from it's index to
  // buffer_size.  If all closure applications return true, then
  // returns true.  Stops processing after the first closure
  // application that returns false, and returns false from this
  // function.  The node's index is updated to exclude the processed
  // elements, e.g. up to the element for which the closure returned
  // false, or one past the last element if the closure always
  // returned true.
  bool apply_closure_to_buffer(G1CardTableEntryClosure* cl,
                               BufferNode* node,
                               uint worker_i = 0);

  // If there are more than stop_at completed buffers, pop one, apply
  // the specified closure to its active elements, and return true.
  // Otherwise return false.
  //
  // A completely processed buffer is freed.  However, if a closure
  // invocation returns false, processing is stopped and the partially
  // processed buffer (with its index updated to exclude the processed
  // elements, e.g. up to the element for which the closure returned
  // false) is returned to the completed buffer set.
  //
  // If during_pause is true, stop_at must be zero, and the closure
  // must never return false.
  bool apply_closure_to_completed_buffer(G1CardTableEntryClosure* cl,
                                         uint worker_i,
                                         size_t stop_at,
                                         bool during_pause);

  bool mut_process_buffer(BufferNode* node);

  // If the queue contains more buffers than configured here, the
  // mutator must start doing some of the concurrent refinement work,
  size_t _max_completed_buffers;
  size_t _completed_buffers_padding;
  static const size_t MaxCompletedBuffersUnlimited = SIZE_MAX;

  G1FreeIdSet* _free_ids;

  // The number of completed buffers processed by mutator and rs thread,
  // respectively.
  jint _processed_buffers_mut;
  jint _processed_buffers_rs_thread;

public:
  G1DirtyCardQueueSet(bool notify_when_complete = true);
  ~G1DirtyCardQueueSet();

  void initialize(Monitor* cbl_mon,
                  BufferNode::Allocator* allocator,
                  bool init_free_ids = false);

  // The number of parallel ids that can be claimed to allow collector or
  // mutator threads to do card-processing work.
  static uint num_par_ids();

  static void handle_zero_index_for_thread(Thread* t);

  // Either process the entire buffer and return true, or enqueue the
  // buffer and return false.  If the buffer is completely processed,
  // it can be reused in place.
  bool process_or_enqueue_completed_buffer(BufferNode* node);

  virtual void enqueue_completed_buffer(BufferNode* node);

  // If the number of completed buffers is > stop_at, then remove and
  // return a completed buffer from the list.  Otherwise, return NULL.
  BufferNode* get_completed_buffer(size_t stop_at = 0);

  // The number of buffers in the list.  Racy...
  size_t completed_buffers_num() const { return _n_completed_buffers; }

  bool process_completed_buffers() { return _process_completed_buffers; }
  void set_process_completed_buffers(bool x) { _process_completed_buffers = x; }

  // Get/Set the number of completed buffers that triggers log processing.
  // Log processing should be done when the number of buffers exceeds the
  // threshold.
  void set_process_completed_buffers_threshold(size_t sz) {
    _process_completed_buffers_threshold = sz;
  }
  size_t process_completed_buffers_threshold() const {
    return _process_completed_buffers_threshold;
  }
  static const size_t ProcessCompletedBuffersThresholdNever = SIZE_MAX;

  // Notify the consumer if the number of buffers crossed the threshold
  void notify_if_necessary();

  void merge_bufferlists(G1RedirtyCardsQueueSet* src);

  // Apply G1RefineCardConcurrentlyClosure to completed buffers until there are stop_at
  // completed buffers remaining.
  bool refine_completed_buffer_concurrently(uint worker_i, size_t stop_at);

  // Apply the given closure to all completed buffers. The given closure's do_card_ptr
  // must never return false. Must only be called during GC.
  bool apply_closure_during_gc(G1CardTableEntryClosure* cl, uint worker_i);

  // If a full collection is happening, reset partial logs, and release
  // completed ones: the full collection will make them all irrelevant.
  void abandon_logs();

  // If any threads have partial logs, add them to the global list of logs.
  void concatenate_logs();

  void set_max_completed_buffers(size_t m) {
    _max_completed_buffers = m;
  }
  size_t max_completed_buffers() const {
    return _max_completed_buffers;
  }

  void set_completed_buffers_padding(size_t padding) {
    _completed_buffers_padding = padding;
  }
  size_t completed_buffers_padding() const {
    return _completed_buffers_padding;
  }

  jint processed_buffers_mut() {
    return _processed_buffers_mut;
  }
  jint processed_buffers_rs_thread() {
    return _processed_buffers_rs_thread;
  }

};

inline G1DirtyCardQueueSet* G1DirtyCardQueue::dirty_card_qset() const {
  return static_cast<G1DirtyCardQueueSet*>(qset());
}

#endif // SHARE_GC_G1_G1DIRTYCARDQUEUE_HPP
