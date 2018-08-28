/*
 *  Copyright 2018, IBM Corporation.
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation; either version
 *  2 of the License, or (at your option) any later version.
 *
 *  This file contains general idle entry/exit functions to save
 *  and restore stack and NVGPRs which allows C code to call idle
 *  states that lose GPRs, and it will return transparently with
 *  SRR1 wakeup reason return value.
 *
 *  The platform / CPU caller must ensure SPRs and any other non-GPR
 *  state is saved and restored correctly, handle KVM, interrupts, etc.
 */

#include <asm/ppc_asm.h>
#include <asm/asm-offsets.h>
#include <asm/ppc-opcode.h>
#include <asm/cpuidle.h>

/*
 * Desired PSSCR in r3
 *
 * No state will be lost regardless of wakeup mechanism (interrupt or NIA).
 *
 * An EC=0 type wakeup will return with a value of 0. SRESET wakeup (which can
 * happen with xscom SRESET and possibly MCE) may clobber volatiles except LR,
 * and must blr, to return to caller with r3 set according to caller's expected
 * return code (for Book3S/64 that is SRR1).
 */
_GLOBAL(isa300_idle_stop_noloss)
	mtspr 	SPRN_PSSCR,r3
	PPC_STOP
	li	r3,0
	blr

/*
 * Desired PSSCR in r3
 *
 * GPRs may be lost, so they are saved here. Wakeup is by interrupt only.
 * The SRESET wakeup returns to this function's caller by calling
 * idle_return_gpr_loss with r3 set to desired return value.
 *
 * A wakeup without GPR loss may alteratively be handled as in
 * isa300_idle_stop_noloss and blr directly, as an optimisation.
 *
 * The caller is responsible for saving/restoring SPRs, MSR, timebase,
 * etc.
 */
_GLOBAL(isa300_idle_stop_mayloss)
	mtspr 	SPRN_PSSCR,r3
	std	r1,PACAR1(r13)
	mflr	r4
	mfcr	r5
	/* use stack red zone rather than a new frame */
	addi	r6,r1,-INT_FRAME_SIZE
	SAVE_GPR(2, r6)
	SAVE_NVGPRS(r6)
	std	r4,_LINK(r6)
	std	r5,_CCR(r6)
	PPC_STOP
	b	.	/* catch bugs */

/*
 * Desired return value in r3
 *
 * The idle wakeup SRESET interrupt can call this after calling
 * to return to the idle sleep function caller with r3 as the return code.
 *
 * This must not be used if idle was entered via a _noloss function (use
 * a simple blr instead).
 */
_GLOBAL(idle_return_gpr_loss)
	ld	r1,PACAR1(r13)
	addi	r6,r1,-INT_FRAME_SIZE
	ld	r4,_LINK(r6)
	ld	r5,_CCR(r6)
	REST_NVGPRS(r6)
	REST_GPR(2, r6)
	mtlr	r4
	mtcr	r5
	blr

/*
 * This is the sequence required to execute idle instructions, as
 * specified in ISA v2.07 (and earlier). MSR[IR] and MSR[DR] must be 0.
 */
#define IDLE_STATE_ENTER_SEQ_NORET(IDLE_INST)			\
	/* Magic NAP/SLEEP/WINKLE mode enter sequence */	\
	std	r0,0(r1);					\
	ptesync;						\
	ld	r0,0(r1);					\
236:	cmpd	cr0,r0,r0;					\
	bne	236b;						\
	IDLE_INST;						\
	b	.	/* catch bugs */

/*
 * Desired instruction type in r3
 *
 * GPRs may be lost, so they are saved here. Wakeup is by interrupt only.
 * The SRESET wakeup returns to this function's caller by calling
 * idle_return_gpr_loss with r3 set to desired return value.
 *
 * A wakeup without GPR loss may alteratively be handled as in
 * isa300_idle_stop_noloss and blr directly, as an optimisation.
 *
 * The caller is responsible for saving/restoring SPRs, MSR, timebase,
 * etc.
 *
 * This must be called in real-mode.
 */
_GLOBAL(isa206_idle_insn_mayloss)
	std	r1,PACAR1(r13)
	mflr	r4
	mfcr	r5
	/* use stack red zone rather than a new frame */
	addi	r6,r1,-INT_FRAME_SIZE
	SAVE_GPR(2, r6)
	SAVE_NVGPRS(r6)
	std	r4,_LINK(r6)
	std	r5,_CCR(r6)
	cmpwi	r3,PNV_THREAD_NAP
	bne	1f
	IDLE_STATE_ENTER_SEQ_NORET(PPC_NAP)
1:	cmpwi	r3,PNV_THREAD_SLEEP
	bne	2f
	IDLE_STATE_ENTER_SEQ_NORET(PPC_SLEEP)
2:	IDLE_STATE_ENTER_SEQ_NORET(PPC_WINKLE)

#ifdef CONFIG_KVM_BOOK3S_HV_POSSIBLE
/*
 * This is used to branch into kvm_start_guest with non-volatiles saved.
 * KVM should branch to idle_return_gpr_loss to return back out to the
 * caller, with r3 as return code.
 */
_GLOBAL(idle_kvm_start_guest)
	std	r1,PACAR1(r13)
	mflr	r4
	mfcr	r5
	/* use stack red zone rather than a new frame */
	addi	r6,r1,-INT_FRAME_SIZE
	SAVE_GPR(2, r6)
	SAVE_NVGPRS(r6)
	std	r4,_LINK(r6)
	std	r5,_CCR(r6)
	b	kvm_start_guest
#endif
