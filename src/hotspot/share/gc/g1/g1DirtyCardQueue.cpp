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

#include "precompiled.hpp"
#include "gc/g1/g1BufferNodeList.hpp"
#include "gc/g1/g1CardTableEntryClosure.hpp"
#include "gc/g1/g1CollectedHeap.inline.hpp"
#include "gc/g1/g1DirtyCardQueue.hpp"
#include "gc/g1/g1FreeIdSet.hpp"
#include "gc/g1/g1RedirtyCardsQueue.hpp"
#include "gc/g1/g1RemSet.hpp"
#include "gc/g1/g1ThreadLocalData.hpp"
#include "gc/g1/heapRegionRemSet.hpp"
#include "gc/shared/suspendibleThreadSet.hpp"
#include "gc/shared/workgroup.hpp"
#include "runtime/atomic.hpp"
#include "runtime/flags/flagSetting.hpp"
#include "runtime/mutexLocker.hpp"
#include "runtime/os.hpp"
#include "runtime/safepoint.hpp"
#include "runtime/thread.inline.hpp"
#include "runtime/threadSMR.hpp"

G1DirtyCardQueue::G1DirtyCardQueue(G1DirtyCardQueueSet* qset) :
  // Dirty card queues are always active, so we create them with their
  // active field set to true.
  PtrQueue(qset, true /* active */)
{ }

G1DirtyCardQueue::~G1DirtyCardQueue() {
  flush();
}

void G1DirtyCardQueue::handle_completed_buffer() {
  assert(_buf != NULL, "precondition");
  BufferNode* node = BufferNode::make_node_from_buffer(_buf, index());
  G1DirtyCardQueueSet* dcqs = dirty_card_qset();
  if (dcqs->process_or_enqueue_completed_buffer(node)) {
    reset();                    // Buffer fully processed, reset index.
  } else {
    allocate_buffer();          // Buffer enqueued, get a new one.
  }
}

// Assumed to be zero by concurrent threads.
static uint par_ids_start() { return 0; }

G1DirtyCardQueueSet::G1DirtyCardQueueSet(Monitor* cbl_mon,
                                         BufferNode::Allocator* allocator) :
  PtrQueueSet(allocator),
  _cbl_mon(cbl_mon),
  _completed_buffers_head(NULL),
  _completed_buffers_tail(NULL),
  _num_cards(0),
  _process_cards_threshold(ProcessCardsThresholdNever),
  _process_completed_buffers(false),
  _max_cards(MaxCardsUnlimited),
  _max_cards_padding(0),
  _free_ids(par_ids_start(), num_par_ids()),
  _mutator_refined_cards_counters(NEW_C_HEAP_ARRAY(size_t, num_par_ids(), mtGC))
{
  ::memset(_mutator_refined_cards_counters, 0, num_par_ids() * sizeof(size_t));
  _all_active = true;
}

G1DirtyCardQueueSet::~G1DirtyCardQueueSet() {
  abandon_completed_buffers();
  FREE_C_HEAP_ARRAY(size_t, _mutator_refined_cards_counters);
}

// Determines how many mutator threads can process the buffers in parallel.
uint G1DirtyCardQueueSet::num_par_ids() {
  return (uint)os::initial_active_processor_count();
}

size_t G1DirtyCardQueueSet::total_mutator_refined_cards() const {
  size_t sum = 0;
  for (uint i = 0; i < num_par_ids(); ++i) {
    sum += _mutator_refined_cards_counters[i];
  }
  return sum;
}

void G1DirtyCardQueueSet::handle_zero_index_for_thread(Thread* t) {
  G1ThreadLocalData::dirty_card_queue(t).handle_zero_index();
}

void G1DirtyCardQueueSet::enqueue_completed_buffer(BufferNode* cbn) {
  MonitorLocker ml(_cbl_mon, Mutex::_no_safepoint_check_flag);
  cbn->set_next(NULL);
  if (_completed_buffers_tail == NULL) {
    assert(_completed_buffers_head == NULL, "Well-formedness");
    _completed_buffers_head = cbn;
    _completed_buffers_tail = cbn;
  } else {
    _completed_buffers_tail->set_next(cbn);
    _completed_buffers_tail = cbn;
  }
  _num_cards += buffer_size() - cbn->index();

  if (!process_completed_buffers() &&
      (num_cards() > process_cards_threshold())) {
    set_process_completed_buffers(true);
    ml.notify_all();
  }
  verify_num_cards();
}

BufferNode* G1DirtyCardQueueSet::get_completed_buffer(size_t stop_at) {
  MutexLocker x(_cbl_mon, Mutex::_no_safepoint_check_flag);

  if (num_cards() <= stop_at) {
    return NULL;
  }

  assert(num_cards() > 0, "invariant");
  assert(_completed_buffers_head != NULL, "invariant");
  assert(_completed_buffers_tail != NULL, "invariant");

  BufferNode* bn = _completed_buffers_head;
  _num_cards -= buffer_size() - bn->index();
  _completed_buffers_head = bn->next();
  if (_completed_buffers_head == NULL) {
    assert(num_cards() == 0, "invariant");
    _completed_buffers_tail = NULL;
    set_process_completed_buffers(false);
  }
  verify_num_cards();
  bn->set_next(NULL);
  return bn;
}

#ifdef ASSERT
void G1DirtyCardQueueSet::verify_num_cards() const {
  size_t actual = 0;
  BufferNode* cur = _completed_buffers_head;
  while (cur != NULL) {
    actual += buffer_size() - cur->index();
    cur = cur->next();
  }
  assert(actual == _num_cards,
         "Num entries in completed buffers should be " SIZE_FORMAT " but are " SIZE_FORMAT,
         _num_cards, actual);
}
#endif

void G1DirtyCardQueueSet::abandon_completed_buffers() {
  BufferNode* buffers_to_delete = NULL;
  {
    MutexLocker x(_cbl_mon, Mutex::_no_safepoint_check_flag);
    buffers_to_delete = _completed_buffers_head;
    _completed_buffers_head = NULL;
    _completed_buffers_tail = NULL;
    _num_cards = 0;
    set_process_completed_buffers(false);
  }
  while (buffers_to_delete != NULL) {
    BufferNode* bn = buffers_to_delete;
    buffers_to_delete = bn->next();
    bn->set_next(NULL);
    deallocate_buffer(bn);
  }
}

void G1DirtyCardQueueSet::notify_if_necessary() {
  MonitorLocker ml(_cbl_mon, Mutex::_no_safepoint_check_flag);
  if (num_cards() > process_cards_threshold()) {
    set_process_completed_buffers(true);
    ml.notify_all();
  }
}

// Merge lists of buffers. Notify the processing threads.
// The source queue is emptied as a result. The queues
// must share the monitor.
void G1DirtyCardQueueSet::merge_bufferlists(G1RedirtyCardsQueueSet* src) {
  assert(allocator() == src->allocator(), "precondition");
  const G1BufferNodeList from = src->take_all_completed_buffers();
  if (from._head == NULL) return;

  MutexLocker x(_cbl_mon, Mutex::_no_safepoint_check_flag);
  if (_completed_buffers_tail == NULL) {
    assert(_completed_buffers_head == NULL, "Well-formedness");
    _completed_buffers_head = from._head;
    _completed_buffers_tail = from._tail;
  } else {
    assert(_completed_buffers_head != NULL, "Well formedness");
    _completed_buffers_tail->set_next(from._head);
    _completed_buffers_tail = from._tail;
  }
  _num_cards += from._entry_count;

  assert(_completed_buffers_head == NULL && _completed_buffers_tail == NULL ||
         _completed_buffers_head != NULL && _completed_buffers_tail != NULL,
         "Sanity");
  verify_num_cards();
}

G1BufferNodeList G1DirtyCardQueueSet::take_all_completed_buffers() {
  MutexLocker x(_cbl_mon, Mutex::_no_safepoint_check_flag);
  G1BufferNodeList result(_completed_buffers_head, _completed_buffers_tail, _num_cards);
  _completed_buffers_head = NULL;
  _completed_buffers_tail = NULL;
  _num_cards = 0;
  return result;
}

bool G1DirtyCardQueueSet::refine_buffer(BufferNode* node,
                                        uint worker_id,
                                        size_t* total_refined_cards) {
  G1RemSet* rem_set = G1CollectedHeap::heap()->rem_set();
  size_t size = buffer_size();
  void** buffer = BufferNode::make_buffer_from_node(node);
  size_t i = node->index();
  assert(i <= size, "invariant");
  for ( ; (i < size) && !SuspendibleThreadSet::should_yield(); ++i) {
    CardTable::CardValue* cp = static_cast<CardTable::CardValue*>(buffer[i]);
    rem_set->refine_card_concurrently(cp, worker_id);
  }
  *total_refined_cards += (i - node->index());
  node->set_index(i);
  return i == size;
}

#ifndef ASSERT
#define assert_fully_consumed(node, buffer_size)
#else
#define assert_fully_consumed(node, buffer_size)                \
  do {                                                          \
    size_t _afc_index = (node)->index();                        \
    size_t _afc_size = (buffer_size);                           \
    assert(_afc_index == _afc_size,                             \
           "Buffer was not fully consumed as claimed: index: "  \
           SIZE_FORMAT ", size: " SIZE_FORMAT,                  \
            _afc_index, _afc_size);                             \
  } while (0)
#endif // ASSERT

bool G1DirtyCardQueueSet::process_or_enqueue_completed_buffer(BufferNode* node) {
  if (Thread::current()->is_Java_thread()) {
    // If the number of buffers exceeds the limit, make this Java
    // thread do the processing itself.  We don't lock to access
    // buffer count or padding; it is fine to be imprecise here.  The
    // add of padding could overflow, which is treated as unlimited.
    size_t limit = max_cards() + max_cards_padding();
    if ((num_cards() > limit) && (limit >= max_cards())) {
      if (mut_process_buffer(node)) {
        return true;
      }
    }
  }
  enqueue_completed_buffer(node);
  return false;
}

bool G1DirtyCardQueueSet::mut_process_buffer(BufferNode* node) {
  uint worker_id = _free_ids.claim_par_id(); // temporarily claim an id
  uint counter_index = worker_id - par_ids_start();
  size_t* counter = &_mutator_refined_cards_counters[counter_index];
  bool result = refine_buffer(node, worker_id, counter);
  _free_ids.release_par_id(worker_id); // release the id

  if (result) {
    assert_fully_consumed(node, buffer_size());
  }
  return result;
}

bool G1DirtyCardQueueSet::refine_completed_buffer_concurrently(uint worker_id,
                                                               size_t stop_at,
                                                               size_t* total_refined_cards) {
  BufferNode* node = get_completed_buffer(stop_at);
  if (node == NULL) {
    return false;
  } else if (refine_buffer(node, worker_id, total_refined_cards)) {
    assert_fully_consumed(node, buffer_size());
    // Done with fully processed buffer.
    deallocate_buffer(node);
    return true;
  } else {
    // Return partially processed buffer to the queue.
    enqueue_completed_buffer(node);
    return true;
  }
}

void G1DirtyCardQueueSet::abandon_logs() {
  assert(SafepointSynchronize::is_at_safepoint(), "Must be at safepoint.");
  abandon_completed_buffers();

  // Since abandon is done only at safepoints, we can safely manipulate
  // these queues.
  struct AbandonThreadLogClosure : public ThreadClosure {
    virtual void do_thread(Thread* t) {
      G1ThreadLocalData::dirty_card_queue(t).reset();
    }
  } closure;
  Threads::threads_do(&closure);

  G1BarrierSet::shared_dirty_card_queue().reset();
}

void G1DirtyCardQueueSet::concatenate_logs() {
  // Iterate over all the threads, if we find a partial log add it to
  // the global list of logs.  Temporarily turn off the limit on the number
  // of outstanding buffers.
  assert(SafepointSynchronize::is_at_safepoint(), "Must be at safepoint.");
  size_t old_limit = max_cards();
  set_max_cards(MaxCardsUnlimited);

  struct ConcatenateThreadLogClosure : public ThreadClosure {
    virtual void do_thread(Thread* t) {
      G1DirtyCardQueue& dcq = G1ThreadLocalData::dirty_card_queue(t);
      if (!dcq.is_empty()) {
        dcq.flush();
      }
    }
  } closure;
  Threads::threads_do(&closure);

  G1BarrierSet::shared_dirty_card_queue().flush();
  set_max_cards(old_limit);
}
