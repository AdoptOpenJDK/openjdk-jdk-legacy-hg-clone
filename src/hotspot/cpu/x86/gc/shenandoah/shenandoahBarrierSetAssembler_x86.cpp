/*
 * Copyright (c) 2018, 2019, Red Hat, Inc. All rights reserved.
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
#include "gc/shenandoah/shenandoahBarrierSetAssembler.hpp"
#include "gc/shenandoah/shenandoahForwarding.hpp"
#include "gc/shenandoah/shenandoahHeap.inline.hpp"
#include "gc/shenandoah/shenandoahHeapRegion.hpp"
#include "gc/shenandoah/shenandoahHeuristics.hpp"
#include "gc/shenandoah/shenandoahRuntime.hpp"
#include "gc/shenandoah/shenandoahThreadLocalData.hpp"
#include "interpreter/interpreter.hpp"
#include "interpreter/interp_masm.hpp"
#include "runtime/sharedRuntime.hpp"
#include "runtime/thread.hpp"
#include "utilities/macros.hpp"
#ifdef COMPILER1
#include "c1/c1_LIRAssembler.hpp"
#include "c1/c1_MacroAssembler.hpp"
#include "gc/shenandoah/c1/shenandoahBarrierSetC1.hpp"
#endif

#define __ masm->

address ShenandoahBarrierSetAssembler::_shenandoah_lrb = NULL;

void ShenandoahBarrierSetAssembler::arraycopy_prologue(MacroAssembler* masm, DecoratorSet decorators, BasicType type,
                                                       Register src, Register dst, Register count) {

  bool dest_uninitialized = (decorators & IS_DEST_UNINITIALIZED) != 0;

  if (is_reference_type(type)) {

    if ((ShenandoahSATBBarrier && !dest_uninitialized) || ShenandoahLoadRefBarrier) {
#ifdef _LP64
      Register thread = r15_thread;
#else
      Register thread = rax;
      if (thread == src || thread == dst || thread == count) {
        thread = rbx;
      }
      if (thread == src || thread == dst || thread == count) {
        thread = rcx;
      }
      if (thread == src || thread == dst || thread == count) {
        thread = rdx;
      }
      __ push(thread);
      __ get_thread(thread);
#endif
      assert_different_registers(src, dst, count, thread);

      Label done;
      // Short-circuit if count == 0.
      __ testptr(count, count);
      __ jcc(Assembler::zero, done);

      // Avoid runtime call when not marking.
      Address gc_state(thread, in_bytes(ShenandoahThreadLocalData::gc_state_offset()));
      int flags = ShenandoahHeap::HAS_FORWARDED;
      if (!dest_uninitialized) {
        flags |= ShenandoahHeap::MARKING;
      }
      __ testb(gc_state, flags);
      __ jcc(Assembler::zero, done);

      __ pusha();                      // push registers
#ifdef _LP64
      assert(src == rdi, "expected");
      assert(dst == rsi, "expected");
      assert(count == rdx, "expected");
      if (UseCompressedOops) {
        if (dest_uninitialized) {
          __ call_VM_leaf(CAST_FROM_FN_PTR(address, ShenandoahRuntime::write_ref_array_pre_duinit_narrow_oop_entry), src, dst, count);
        } else {
          __ call_VM_leaf(CAST_FROM_FN_PTR(address, ShenandoahRuntime::write_ref_array_pre_narrow_oop_entry), src, dst, count);
        }
      } else
#endif
      {
        if (dest_uninitialized) {
          __ call_VM_leaf(CAST_FROM_FN_PTR(address, ShenandoahRuntime::write_ref_array_pre_duinit_oop_entry), src, dst, count);
        } else {
          __ call_VM_leaf(CAST_FROM_FN_PTR(address, ShenandoahRuntime::write_ref_array_pre_oop_entry), src, dst, count);
        }
      }
      __ popa();
      __ bind(done);
      NOT_LP64(__ pop(thread);)
    }
  }

}

void ShenandoahBarrierSetAssembler::shenandoah_write_barrier_pre(MacroAssembler* masm,
                                                                 Register obj,
                                                                 Register pre_val,
                                                                 Register thread,
                                                                 Register tmp,
                                                                 bool tosca_live,
                                                                 bool expand_call) {

  if (ShenandoahSATBBarrier) {
    satb_write_barrier_pre(masm, obj, pre_val, thread, tmp, tosca_live, expand_call);
  }
}

void ShenandoahBarrierSetAssembler::satb_write_barrier_pre(MacroAssembler* masm,
                                                           Register obj,
                                                           Register pre_val,
                                                           Register thread,
                                                           Register tmp,
                                                           bool tosca_live,
                                                           bool expand_call) {
  // If expand_call is true then we expand the call_VM_leaf macro
  // directly to skip generating the check by
  // InterpreterMacroAssembler::call_VM_leaf_base that checks _last_sp.

#ifdef _LP64
  assert(thread == r15_thread, "must be");
#endif // _LP64

  Label done;
  Label runtime;

  assert(pre_val != noreg, "check this code");

  if (obj != noreg) {
    assert_different_registers(obj, pre_val, tmp);
    assert(pre_val != rax, "check this code");
  }

  Address in_progress(thread, in_bytes(ShenandoahThreadLocalData::satb_mark_queue_active_offset()));
  Address index(thread, in_bytes(ShenandoahThreadLocalData::satb_mark_queue_index_offset()));
  Address buffer(thread, in_bytes(ShenandoahThreadLocalData::satb_mark_queue_buffer_offset()));

  Address gc_state(thread, in_bytes(ShenandoahThreadLocalData::gc_state_offset()));
  __ testb(gc_state, ShenandoahHeap::MARKING | ShenandoahHeap::TRAVERSAL);
  __ jcc(Assembler::zero, done);

  // Do we need to load the previous value?
  if (obj != noreg) {
    __ load_heap_oop(pre_val, Address(obj, 0), noreg, noreg, AS_RAW);
  }

  // Is the previous value null?
  __ cmpptr(pre_val, (int32_t) NULL_WORD);
  __ jcc(Assembler::equal, done);

  // Can we store original value in the thread's buffer?
  // Is index == 0?
  // (The index field is typed as size_t.)

  __ movptr(tmp, index);                   // tmp := *index_adr
  __ cmpptr(tmp, 0);                       // tmp == 0?
  __ jcc(Assembler::equal, runtime);       // If yes, goto runtime

  __ subptr(tmp, wordSize);                // tmp := tmp - wordSize
  __ movptr(index, tmp);                   // *index_adr := tmp
  __ addptr(tmp, buffer);                  // tmp := tmp + *buffer_adr

  // Record the previous value
  __ movptr(Address(tmp, 0), pre_val);
  __ jmp(done);

  __ bind(runtime);
  // save the live input values
  if(tosca_live) __ push(rax);

  if (obj != noreg && obj != rax)
    __ push(obj);

  if (pre_val != rax)
    __ push(pre_val);

  // Calling the runtime using the regular call_VM_leaf mechanism generates
  // code (generated by InterpreterMacroAssember::call_VM_leaf_base)
  // that checks that the *(ebp+frame::interpreter_frame_last_sp) == NULL.
  //
  // If we care generating the pre-barrier without a frame (e.g. in the
  // intrinsified Reference.get() routine) then ebp might be pointing to
  // the caller frame and so this check will most likely fail at runtime.
  //
  // Expanding the call directly bypasses the generation of the check.
  // So when we do not have have a full interpreter frame on the stack
  // expand_call should be passed true.

  NOT_LP64( __ push(thread); )

#ifdef _LP64
  // We move pre_val into c_rarg0 early, in order to avoid smashing it, should
  // pre_val be c_rarg1 (where the call prologue would copy thread argument).
  // Note: this should not accidentally smash thread, because thread is always r15.
  assert(thread != c_rarg0, "smashed arg");
  if (c_rarg0 != pre_val) {
    __ mov(c_rarg0, pre_val);
  }
#endif

  if (expand_call) {
    LP64_ONLY( assert(pre_val != c_rarg1, "smashed arg"); )
#ifdef _LP64
    if (c_rarg1 != thread) {
      __ mov(c_rarg1, thread);
    }
    // Already moved pre_val into c_rarg0 above
#else
    __ push(thread);
    __ push(pre_val);
#endif
    __ MacroAssembler::call_VM_leaf_base(CAST_FROM_FN_PTR(address, ShenandoahRuntime::write_ref_field_pre_entry), 2);
  } else {
    __ call_VM_leaf(CAST_FROM_FN_PTR(address, ShenandoahRuntime::write_ref_field_pre_entry), LP64_ONLY(c_rarg0) NOT_LP64(pre_val), thread);
  }

  NOT_LP64( __ pop(thread); )

  // save the live input values
  if (pre_val != rax)
    __ pop(pre_val);

  if (obj != noreg && obj != rax)
    __ pop(obj);

  if(tosca_live) __ pop(rax);

  __ bind(done);
}

void ShenandoahBarrierSetAssembler::resolve_forward_pointer(MacroAssembler* masm, Register dst, Register tmp) {
  assert(ShenandoahCASBarrier, "should be enabled");
  Label is_null;
  __ testptr(dst, dst);
  __ jcc(Assembler::zero, is_null);
  resolve_forward_pointer_not_null(masm, dst, tmp);
  __ bind(is_null);
}

void ShenandoahBarrierSetAssembler::resolve_forward_pointer_not_null(MacroAssembler* masm, Register dst, Register tmp) {
  assert(ShenandoahCASBarrier || ShenandoahLoadRefBarrier, "should be enabled");
  // The below loads the mark word, checks if the lowest two bits are
  // set, and if so, clear the lowest two bits and copy the result
  // to dst. Otherwise it leaves dst alone.
  // Implementing this is surprisingly awkward. I do it here by:
  // - Inverting the mark word
  // - Test lowest two bits == 0
  // - If so, set the lowest two bits
  // - Invert the result back, and copy to dst

  bool borrow_reg = (tmp == noreg);
  if (borrow_reg) {
    // No free registers available. Make one useful.
    tmp = LP64_ONLY(rscratch1) NOT_LP64(rdx);
    if (tmp == dst) {
      tmp = LP64_ONLY(rscratch2) NOT_LP64(rcx);
    }
    __ push(tmp);
  }

  assert_different_registers(dst, tmp);

  Label done;
  __ movptr(tmp, Address(dst, oopDesc::mark_offset_in_bytes()));
  __ notptr(tmp);
  __ testb(tmp, markWord::marked_value);
  __ jccb(Assembler::notZero, done);
  __ orptr(tmp, markWord::marked_value);
  __ notptr(tmp);
  __ mov(dst, tmp);
  __ bind(done);

  if (borrow_reg) {
    __ pop(tmp);
  }
}


void ShenandoahBarrierSetAssembler::load_reference_barrier_not_null(MacroAssembler* masm, Register dst) {
  assert(ShenandoahLoadRefBarrier, "Should be enabled");

  Label done;

#ifdef _LP64
  Register thread = r15_thread;
#else
  Register thread = rcx;
  if (thread == dst) {
    thread = rbx;
  }
  __ push(thread);
  __ get_thread(thread);
#endif
  assert_different_registers(dst, thread);

  Address gc_state(thread, in_bytes(ShenandoahThreadLocalData::gc_state_offset()));
  __ testb(gc_state, ShenandoahHeap::HAS_FORWARDED);
  __ jccb(Assembler::zero, done);

   if (dst != rax) {
     __ xchgptr(dst, rax); // Move obj into rax and save rax into obj.
   }

   __ call(RuntimeAddress(CAST_FROM_FN_PTR(address, ShenandoahBarrierSetAssembler::shenandoah_lrb())));

   if (dst != rax) {
     __ xchgptr(rax, dst); // Swap back obj with rax.
   }

  __ bind(done);

#ifndef _LP64
  __ pop(thread);
#endif
}

void ShenandoahBarrierSetAssembler::load_reference_barrier_native(MacroAssembler* masm, Register dst, Address src) {
  if (!ShenandoahLoadRefBarrier) {
    return;
  }

  Label done;
  Label not_null;
  Label slow_path;
  __ block_comment("load_reference_barrier_native { ");

  // null check
  __ testptr(dst, dst);
  __ jcc(Assembler::notZero, not_null);
  __ jmp(done);
  __ bind(not_null);


#ifdef _LP64
  Register thread = r15_thread;
#else
  Register thread = rcx;
  if (thread == dst) {
    thread = rbx;
  }
  __ push(thread);
  __ get_thread(thread);
#endif
  assert_different_registers(dst, thread);

  Address gc_state(thread, in_bytes(ShenandoahThreadLocalData::gc_state_offset()));
  __ testb(gc_state, ShenandoahHeap::EVACUATION);
#ifndef _LP64
  __ pop(thread);
#endif
  __ jccb(Assembler::notZero, slow_path);
  __ jmp(done);
  __ bind(slow_path);

  if (dst != rax) {
    __ push(rax);
  }
  __ push(rcx);
  __ push(rdx);
  __ push(rdi);
  __ push(rsi);
#ifdef _LP64
  __ push(r8);
  __ push(r9);
  __ push(r10);
  __ push(r11);
  __ push(r12);
  __ push(r13);
  __ push(r14);
  __ push(r15);
#endif

  assert_different_registers(dst, rsi);
  __ lea(rsi, src);
  __ call_VM_leaf(CAST_FROM_FN_PTR(address, ShenandoahRuntime::load_reference_barrier_native), dst, rsi);

#ifdef _LP64
  __ pop(r15);
  __ pop(r14);
  __ pop(r13);
  __ pop(r12);
  __ pop(r11);
  __ pop(r10);
  __ pop(r9);
  __ pop(r8);
#endif
  __ pop(rsi);
  __ pop(rdi);
  __ pop(rdx);
  __ pop(rcx);

  if (dst != rax) {
    __ movptr(dst, rax);
    __ pop(rax);
  }

  __ bind(done);
  __ block_comment("load_reference_barrier_native { ");
}

void ShenandoahBarrierSetAssembler::storeval_barrier(MacroAssembler* masm, Register dst, Register tmp) {
  if (ShenandoahStoreValEnqueueBarrier) {
    storeval_barrier_impl(masm, dst, tmp);
  }
}

void ShenandoahBarrierSetAssembler::storeval_barrier_impl(MacroAssembler* masm, Register dst, Register tmp) {
  assert(ShenandoahStoreValEnqueueBarrier, "should be enabled");

  if (dst == noreg) return;

  if (ShenandoahStoreValEnqueueBarrier) {
    // The set of registers to be saved+restored is the same as in the write-barrier above.
    // Those are the commonly used registers in the interpreter.
    __ pusha();
    // __ push_callee_saved_registers();
    __ subptr(rsp, 2 * Interpreter::stackElementSize);
    __ movdbl(Address(rsp, 0), xmm0);

#ifdef _LP64
    Register thread = r15_thread;
#else
    Register thread = rcx;
    if (thread == dst || thread == tmp) {
      thread = rdi;
    }
    if (thread == dst || thread == tmp) {
      thread = rbx;
    }
    __ get_thread(thread);
#endif
    assert_different_registers(dst, tmp, thread);

    satb_write_barrier_pre(masm, noreg, dst, thread, tmp, true, false);
    __ movdbl(xmm0, Address(rsp, 0));
    __ addptr(rsp, 2 * Interpreter::stackElementSize);
    //__ pop_callee_saved_registers();
    __ popa();
  }
}

void ShenandoahBarrierSetAssembler::load_reference_barrier(MacroAssembler* masm, Register dst) {
  if (ShenandoahLoadRefBarrier) {
    Label done;
    __ testptr(dst, dst);
    __ jcc(Assembler::zero, done);
    load_reference_barrier_not_null(masm, dst);
    __ bind(done);
  }
}

void ShenandoahBarrierSetAssembler::load_at(MacroAssembler* masm, DecoratorSet decorators, BasicType type,
             Register dst, Address src, Register tmp1, Register tmp_thread) {
  bool on_oop = is_reference_type(type);
  bool on_weak = (decorators & ON_WEAK_OOP_REF) != 0;
  bool on_phantom = (decorators & ON_PHANTOM_OOP_REF) != 0;
  bool not_in_heap = (decorators & IN_NATIVE) != 0;
  bool on_reference = on_weak || on_phantom;
  bool is_traversal_mode = ShenandoahHeap::heap()->is_traversal_mode();
  bool keep_alive = ((decorators & AS_NO_KEEPALIVE) == 0) || is_traversal_mode;

  Register result_dst = dst;
  bool use_tmp1_for_dst = false;

  if (on_oop) {
    // We want to preserve src
    if (dst == src.base() || dst == src.index()) {
      // Use tmp1 for dst if possible, as it is not used in BarrierAssembler::load_at()
      if (tmp1->is_valid() && tmp1 != src.base() && tmp1 != src.index()) {
        dst = tmp1;
        use_tmp1_for_dst = true;
      } else {
        dst = rdi;
        __ push(dst);
      }
    }
    assert_different_registers(dst, src.base(), src.index());
  }

  BarrierSetAssembler::load_at(masm, decorators, type, dst, src, tmp1, tmp_thread);

  if (on_oop) {
    if (not_in_heap && !is_traversal_mode) {
      load_reference_barrier_native(masm, dst, src);
    } else {
      load_reference_barrier(masm, dst);
    }

    if (dst != result_dst) {
      __ movptr(result_dst, dst);

      if (!use_tmp1_for_dst) {
        __ pop(dst);
      }

      dst = result_dst;
    }

    if (ShenandoahKeepAliveBarrier && on_reference && keep_alive) {
      const Register thread = NOT_LP64(tmp_thread) LP64_ONLY(r15_thread);
      assert_different_registers(dst, tmp1, tmp_thread);
      NOT_LP64(__ get_thread(thread));
      // Generate the SATB pre-barrier code to log the value of
      // the referent field in an SATB buffer.
      shenandoah_write_barrier_pre(masm /* masm */,
                                   noreg /* obj */,
                                   dst /* pre_val */,
                                   thread /* thread */,
                                   tmp1 /* tmp */,
                                   true /* tosca_live */,
                                   true /* expand_call */);
    }
  }
}

void ShenandoahBarrierSetAssembler::store_at(MacroAssembler* masm, DecoratorSet decorators, BasicType type,
              Address dst, Register val, Register tmp1, Register tmp2) {

  bool on_oop = is_reference_type(type);
  bool in_heap = (decorators & IN_HEAP) != 0;
  bool as_normal = (decorators & AS_NORMAL) != 0;
  if (on_oop && in_heap) {
    bool needs_pre_barrier = as_normal;

    Register tmp3 = LP64_ONLY(r8) NOT_LP64(rsi);
    Register rthread = LP64_ONLY(r15_thread) NOT_LP64(rcx);
    // flatten object address if needed
    // We do it regardless of precise because we need the registers
    if (dst.index() == noreg && dst.disp() == 0) {
      if (dst.base() != tmp1) {
        __ movptr(tmp1, dst.base());
      }
    } else {
      __ lea(tmp1, dst);
    }

    assert_different_registers(val, tmp1, tmp2, tmp3, rthread);

#ifndef _LP64
    __ get_thread(rthread);
    InterpreterMacroAssembler *imasm = static_cast<InterpreterMacroAssembler*>(masm);
    imasm->save_bcp();
#endif

    if (needs_pre_barrier) {
      shenandoah_write_barrier_pre(masm /*masm*/,
                                   tmp1 /* obj */,
                                   tmp2 /* pre_val */,
                                   rthread /* thread */,
                                   tmp3  /* tmp */,
                                   val != noreg /* tosca_live */,
                                   false /* expand_call */);
    }
    if (val == noreg) {
      BarrierSetAssembler::store_at(masm, decorators, type, Address(tmp1, 0), val, noreg, noreg);
    } else {
      storeval_barrier(masm, val, tmp3);
      BarrierSetAssembler::store_at(masm, decorators, type, Address(tmp1, 0), val, noreg, noreg);
    }
    NOT_LP64(imasm->restore_bcp());
  } else {
    BarrierSetAssembler::store_at(masm, decorators, type, dst, val, tmp1, tmp2);
  }
}

void ShenandoahBarrierSetAssembler::try_resolve_jobject_in_native(MacroAssembler* masm, Register jni_env,
                                                                  Register obj, Register tmp, Label& slowpath) {
  Label done;
  // Resolve jobject
  BarrierSetAssembler::try_resolve_jobject_in_native(masm, jni_env, obj, tmp, slowpath);

  // Check for null.
  __ testptr(obj, obj);
  __ jcc(Assembler::zero, done);

  Address gc_state(jni_env, ShenandoahThreadLocalData::gc_state_offset() - JavaThread::jni_environment_offset());
  __ testb(gc_state, ShenandoahHeap::EVACUATION);
  __ jccb(Assembler::notZero, slowpath);
  __ bind(done);
}

// Special Shenandoah CAS implementation that handles false negatives
// due to concurrent evacuation.
void ShenandoahBarrierSetAssembler::cmpxchg_oop(MacroAssembler* masm,
                                                Register res, Address addr, Register oldval, Register newval,
                                                bool exchange, Register tmp1, Register tmp2) {
  assert(ShenandoahCASBarrier, "Should only be used when CAS barrier is enabled");
  assert(oldval == rax, "must be in rax for implicit use in cmpxchg");

  Label retry, done;

  // Remember oldval for retry logic below
#ifdef _LP64
  if (UseCompressedOops) {
    __ movl(tmp1, oldval);
  } else
#endif
  {
    __ movptr(tmp1, oldval);
  }

  // Step 1. Try to CAS with given arguments. If successful, then we are done,
  // and can safely return.
  if (os::is_MP()) __ lock();
#ifdef _LP64
  if (UseCompressedOops) {
    __ cmpxchgl(newval, addr);
  } else
#endif
  {
    __ cmpxchgptr(newval, addr);
  }
  __ jcc(Assembler::equal, done, true);

  // Step 2. CAS had failed. This may be a false negative.
  //
  // The trouble comes when we compare the to-space pointer with the from-space
  // pointer to the same object. To resolve this, it will suffice to resolve both
  // oldval and the value from memory -- this will give both to-space pointers.
  // If they mismatch, then it was a legitimate failure.
  //
#ifdef _LP64
  if (UseCompressedOops) {
    __ decode_heap_oop(tmp1);
  }
#endif
  resolve_forward_pointer(masm, tmp1);

#ifdef _LP64
  if (UseCompressedOops) {
    __ movl(tmp2, oldval);
    __ decode_heap_oop(tmp2);
  } else
#endif
  {
    __ movptr(tmp2, oldval);
  }
  resolve_forward_pointer(masm, tmp2);

  __ cmpptr(tmp1, tmp2);
  __ jcc(Assembler::notEqual, done, true);

  // Step 3. Try to CAS again with resolved to-space pointers.
  //
  // Corner case: it may happen that somebody stored the from-space pointer
  // to memory while we were preparing for retry. Therefore, we can fail again
  // on retry, and so need to do this in loop, always resolving the failure
  // witness.
  __ bind(retry);
  if (os::is_MP()) __ lock();
#ifdef _LP64
  if (UseCompressedOops) {
    __ cmpxchgl(newval, addr);
  } else
#endif
  {
    __ cmpxchgptr(newval, addr);
  }
  __ jcc(Assembler::equal, done, true);

#ifdef _LP64
  if (UseCompressedOops) {
    __ movl(tmp2, oldval);
    __ decode_heap_oop(tmp2);
  } else
#endif
  {
    __ movptr(tmp2, oldval);
  }
  resolve_forward_pointer(masm, tmp2);

  __ cmpptr(tmp1, tmp2);
  __ jcc(Assembler::equal, retry, true);

  // Step 4. If we need a boolean result out of CAS, check the flag again,
  // and promote the result. Note that we handle the flag from both the CAS
  // itself and from the retry loop.
  __ bind(done);
  if (!exchange) {
    assert(res != NULL, "need result register");
#ifdef _LP64
    __ setb(Assembler::equal, res);
    __ movzbl(res, res);
#else
    // Need something else to clean the result, because some registers
    // do not have byte encoding that movzbl wants. Cannot do the xor first,
    // because it modifies the flags.
    Label res_non_zero;
    __ movptr(res, 1);
    __ jcc(Assembler::equal, res_non_zero, true);
    __ xorptr(res, res);
    __ bind(res_non_zero);
#endif
  }
}

#undef __

#ifdef COMPILER1

#define __ ce->masm()->

void ShenandoahBarrierSetAssembler::gen_pre_barrier_stub(LIR_Assembler* ce, ShenandoahPreBarrierStub* stub) {
  ShenandoahBarrierSetC1* bs = (ShenandoahBarrierSetC1*)BarrierSet::barrier_set()->barrier_set_c1();
  // At this point we know that marking is in progress.
  // If do_load() is true then we have to emit the
  // load of the previous value; otherwise it has already
  // been loaded into _pre_val.

  __ bind(*stub->entry());
  assert(stub->pre_val()->is_register(), "Precondition.");

  Register pre_val_reg = stub->pre_val()->as_register();

  if (stub->do_load()) {
    ce->mem2reg(stub->addr(), stub->pre_val(), T_OBJECT, stub->patch_code(), stub->info(), false /*wide*/, false /*unaligned*/);
  }

  __ cmpptr(pre_val_reg, (int32_t)NULL_WORD);
  __ jcc(Assembler::equal, *stub->continuation());
  ce->store_parameter(stub->pre_val()->as_register(), 0);
  __ call(RuntimeAddress(bs->pre_barrier_c1_runtime_code_blob()->code_begin()));
  __ jmp(*stub->continuation());

}

void ShenandoahBarrierSetAssembler::gen_load_reference_barrier_stub(LIR_Assembler* ce, ShenandoahLoadReferenceBarrierStub* stub) {
  ShenandoahBarrierSetC1* bs = (ShenandoahBarrierSetC1*)BarrierSet::barrier_set()->barrier_set_c1();
  __ bind(*stub->entry());

  Register obj = stub->obj()->as_register();
  Register res = stub->result()->as_register();
  Register addr = stub->addr()->as_register();
  Register tmp1 = stub->tmp1()->as_register();
  Register tmp2 = stub->tmp2()->as_register();
  assert_different_registers(obj, res, addr, tmp1, tmp2);

  Label slow_path;

  assert(res == rax, "result must arrive in rax");

  if (res != obj) {
    __ mov(res, obj);
  }

  // Check for null.
  __ testptr(res, res);
  __ jcc(Assembler::zero, *stub->continuation());

  // Check for object being in the collection set.
  __ mov(tmp1, res);
  __ shrptr(tmp1, ShenandoahHeapRegion::region_size_bytes_shift_jint());
  __ movptr(tmp2, (intptr_t) ShenandoahHeap::in_cset_fast_test_addr());
#ifdef _LP64
  __ movbool(tmp2, Address(tmp2, tmp1, Address::times_1));
  __ testbool(tmp2);
#else
  // On x86_32, C1 register allocator can give us the register without 8-bit support.
  // Do the full-register access and test to avoid compilation failures.
  __ movptr(tmp2, Address(tmp2, tmp1, Address::times_1));
  __ testptr(tmp2, 0xFF);
#endif
  __ jcc(Assembler::zero, *stub->continuation());

  __ bind(slow_path);
  ce->store_parameter(res, 0);
  ce->store_parameter(addr, 1);
  __ call(RuntimeAddress(bs->load_reference_barrier_rt_code_blob()->code_begin()));

  __ jmp(*stub->continuation());
}

#undef __

#define __ sasm->

void ShenandoahBarrierSetAssembler::generate_c1_pre_barrier_runtime_stub(StubAssembler* sasm) {
  __ prologue("shenandoah_pre_barrier", false);
  // arg0 : previous value of memory

  __ push(rax);
  __ push(rdx);

  const Register pre_val = rax;
  const Register thread = NOT_LP64(rax) LP64_ONLY(r15_thread);
  const Register tmp = rdx;

  NOT_LP64(__ get_thread(thread);)

  Address queue_index(thread, in_bytes(ShenandoahThreadLocalData::satb_mark_queue_index_offset()));
  Address buffer(thread, in_bytes(ShenandoahThreadLocalData::satb_mark_queue_buffer_offset()));

  Label done;
  Label runtime;

  // Is SATB still active?
  Address gc_state(thread, in_bytes(ShenandoahThreadLocalData::gc_state_offset()));
  __ testb(gc_state, ShenandoahHeap::MARKING | ShenandoahHeap::TRAVERSAL);
  __ jcc(Assembler::zero, done);

  // Can we store original value in the thread's buffer?

  __ movptr(tmp, queue_index);
  __ testptr(tmp, tmp);
  __ jcc(Assembler::zero, runtime);
  __ subptr(tmp, wordSize);
  __ movptr(queue_index, tmp);
  __ addptr(tmp, buffer);

  // prev_val (rax)
  __ load_parameter(0, pre_val);
  __ movptr(Address(tmp, 0), pre_val);
  __ jmp(done);

  __ bind(runtime);

  __ save_live_registers_no_oop_map(true);

  // load the pre-value
  __ load_parameter(0, rcx);
  __ call_VM_leaf(CAST_FROM_FN_PTR(address, ShenandoahRuntime::write_ref_field_pre_entry), rcx, thread);

  __ restore_live_registers(true);

  __ bind(done);

  __ pop(rdx);
  __ pop(rax);

  __ epilogue();
}

void ShenandoahBarrierSetAssembler::generate_c1_load_reference_barrier_runtime_stub(StubAssembler* sasm) {
  __ prologue("shenandoah_load_reference_barrier", false);
  // arg0 : object to be resolved

  __ save_live_registers_no_oop_map(true);

#ifdef _LP64
  __ load_parameter(0, c_rarg0);
  __ load_parameter(1, c_rarg1);
  if (UseCompressedOops) {
    __ call_VM_leaf(CAST_FROM_FN_PTR(address, ShenandoahRuntime::load_reference_barrier_fixup_narrow), c_rarg0, c_rarg1);
  } else {
    __ call_VM_leaf(CAST_FROM_FN_PTR(address, ShenandoahRuntime::load_reference_barrier_fixup), c_rarg0, c_rarg1);
  }
#else
  __ load_parameter(0, rax);
  __ load_parameter(1, rbx);
  __ call_VM_leaf(CAST_FROM_FN_PTR(address, ShenandoahRuntime::load_reference_barrier_fixup), rax, rbx);
#endif

  __ restore_live_registers_except_rax(true);

  __ epilogue();
}

#undef __

#endif // COMPILER1

address ShenandoahBarrierSetAssembler::shenandoah_lrb() {
  assert(_shenandoah_lrb != NULL, "need load reference barrier stub");
  return _shenandoah_lrb;
}

#define __ cgen->assembler()->

address ShenandoahBarrierSetAssembler::generate_shenandoah_lrb(StubCodeGenerator* cgen) {
  __ align(CodeEntryAlignment);
  StubCodeMark mark(cgen, "StubRoutines", "shenandoah_lrb");
  address start = __ pc();

  Label resolve_oop, slow_path;

  // We use RDI, which also serves as argument register for slow call.
  // RAX always holds the src object ptr, except after the slow call,
  // then it holds the result. R8/RBX is used as temporary register.

  Register tmp1 = rdi;
  Register tmp2 = LP64_ONLY(r8) NOT_LP64(rbx);

  __ push(tmp1);
  __ push(tmp2);

  // Check for object being in the collection set.
  __ mov(tmp1, rax);
  __ shrptr(tmp1, ShenandoahHeapRegion::region_size_bytes_shift_jint());
  __ movptr(tmp2, (intptr_t) ShenandoahHeap::in_cset_fast_test_addr());
  __ movbool(tmp2, Address(tmp2, tmp1, Address::times_1));
  __ testbool(tmp2);
  __ jccb(Assembler::notZero, resolve_oop);
  __ pop(tmp2);
  __ pop(tmp1);
  __ ret(0);

  // Test if object is already resolved.
  __ bind(resolve_oop);
  __ movptr(tmp2, Address(rax, oopDesc::mark_offset_in_bytes()));
  // Test if both lowest bits are set. We trick it by negating the bits
  // then test for both bits clear.
  __ notptr(tmp2);
  __ testb(tmp2, markWord::marked_value);
  __ jccb(Assembler::notZero, slow_path);
  // Clear both lower bits. It's still inverted, so set them, and then invert back.
  __ orptr(tmp2, markWord::marked_value);
  __ notptr(tmp2);
  // At this point, tmp2 contains the decoded forwarding pointer.
  __ mov(rax, tmp2);

  __ pop(tmp2);
  __ pop(tmp1);
  __ ret(0);

  __ bind(slow_path);

  __ push(rcx);
  __ push(rdx);
  __ push(rdi);
  __ push(rsi);
#ifdef _LP64
  __ push(r8);
  __ push(r9);
  __ push(r10);
  __ push(r11);
  __ push(r12);
  __ push(r13);
  __ push(r14);
  __ push(r15);
#endif
  __ push(rbp);
  __ movptr(rbp, rsp);
  __ andptr(rsp, -StackAlignmentInBytes);
  __ push_FPU_state();
  __ call_VM_leaf(CAST_FROM_FN_PTR(address, ShenandoahRuntime::load_reference_barrier), rax);
  __ pop_FPU_state();
  __ movptr(rsp, rbp);
  __ pop(rbp);
#ifdef _LP64
  __ pop(r15);
  __ pop(r14);
  __ pop(r13);
  __ pop(r12);
  __ pop(r11);
  __ pop(r10);
  __ pop(r9);
  __ pop(r8);
#endif
  __ pop(rsi);
  __ pop(rdi);
  __ pop(rdx);
  __ pop(rcx);

  __ pop(tmp2);
  __ pop(tmp1);
  __ ret(0);

  return start;
}

#undef __

void ShenandoahBarrierSetAssembler::barrier_stubs_init() {
  if (ShenandoahLoadRefBarrier) {
    int stub_code_size = 4096;
    ResourceMark rm;
    BufferBlob* bb = BufferBlob::create("shenandoah_barrier_stubs", stub_code_size);
    CodeBuffer buf(bb);
    StubCodeGenerator cgen(&buf);
    _shenandoah_lrb = generate_shenandoah_lrb(&cgen);
  }
}
