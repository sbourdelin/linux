/*
 *  This file contains the power_save function for Power7 CPUs.
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation; either version
 *  2 of the License, or (at your option) any later version.
 */

#include <linux/threads.h>
#include <asm/processor.h>
#include <asm/page.h>
#include <asm/cputable.h>
#include <asm/thread_info.h>
#include <asm/ppc_asm.h>
#include <asm/asm-offsets.h>
#include <asm/ppc-opcode.h>
#include <asm/hw_irq.h>
#include <asm/kvm_book3s_asm.h>
#include <asm/opal.h>
#include <asm/cpuidle.h>
#include <asm/exception-64s.h>
#include <asm/book3s/64/mmu-hash.h>

#undef DEBUG

/*
 * Use unused space in the interrupt stack to save and restore
 * registers for winkle support.
 */
#define _SDR1	GPR3
#define _RPR	GPR4
#define _SPURR	GPR5
#define _PURR	GPR6
#define _TSCR	GPR7
#define _DSCR	GPR8
#define _AMOR	GPR9
#define _WORT	GPR10
#define _WORC	GPR11

/* Idle state entry routines */

#define	IDLE_STATE_ENTER_SEQ(IDLE_INST)				\
	/* Magic NAP/SLEEP/WINKLE mode enter sequence */	\
	std	r0,0(r1);					\
	ptesync;						\
	ld	r0,0(r1);					\
1:	cmp	cr0,r0,r0;					\
	bne	1b;						\
	IDLE_INST;						\
	b	.

	.text

/*
 * Used by threads when the lock bit of core_idle_state is set.
 * Threads will spin in HMT_LOW until the lock bit is cleared.
 * r14 - pointer to core_idle_state
 * r15 - used to load contents of core_idle_state
 */

core_idle_lock_held:
	HMT_LOW
3:	lwz	r15,0(r14)
	andi.   r15,r15,PNV_CORE_IDLE_LOCK_BIT
	bne	3b
	HMT_MEDIUM
	lwarx	r15,0,r14
	blr

	.globl	power7_enter_nap_mode
power7_enter_nap_mode:
#ifdef CONFIG_KVM_BOOK3S_HV_POSSIBLE
	/* Tell KVM we're napping */
	li	r4,KVM_HWTHREAD_IN_NAP
	stb	r4,HSTATE_HWTHREAD_STATE(r13)
#endif
	stb	r3,PACA_THREAD_IDLE_STATE(r13)
	cmpwi	cr3,r3,PNV_THREAD_SLEEP
	bge	cr3,2f
	IDLE_STATE_ENTER_SEQ(PPC_NAP)
	/* No return */
2:
	/* Sleep or winkle */
	lbz	r7,PACA_THREAD_MASK(r13)
	ld	r14,PACA_CORE_IDLE_STATE_PTR(r13)
lwarx_loop1:
	lwarx	r15,0,r14

	andi.   r9,r15,PNV_CORE_IDLE_LOCK_BIT
	bnel	core_idle_lock_held

	andc	r15,r15,r7			/* Clear thread bit */

	andi.	r15,r15,PNV_CORE_IDLE_THREAD_BITS

/*
 * If cr0 = 0, then current thread is the last thread of the core entering
 * sleep. Last thread needs to execute the hardware bug workaround code if
 * required by the platform.
 * Make the workaround call unconditionally here. The below branch call is
 * patched out when the idle states are discovered if the platform does not
 * require it.
 */
.global pnv_fastsleep_workaround_at_entry
pnv_fastsleep_workaround_at_entry:
	beq	fastsleep_workaround_at_entry

	stwcx.	r15,0,r14
	bne-	lwarx_loop1
	isync

common_enter: /* common code for all the threads entering sleep or winkle */
	bgt	cr3,enter_winkle
	IDLE_STATE_ENTER_SEQ(PPC_SLEEP)

fastsleep_workaround_at_entry:
	ori	r15,r15,PNV_CORE_IDLE_LOCK_BIT
	stwcx.	r15,0,r14
	bne-	lwarx_loop1
	isync

	/* Fast sleep workaround */
	li	r3,1
	li	r4,1
	li	r0,OPAL_CONFIG_CPU_IDLE_STATE
	bl	opal_call_realmode

	/* Clear Lock bit */
	li	r0,0
	lwsync
	stw	r0,0(r14)
	b	common_enter

enter_winkle:
	/*
	 * Note all register i.e per-core, per-subcore or per-thread is saved
	 * here since any thread in the core might wake up first
	 */
	mfspr	r3,SPRN_SDR1
	std	r3,_SDR1(r1)
	mfspr	r3,SPRN_RPR
	std	r3,_RPR(r1)
	mfspr	r3,SPRN_SPURR
	std	r3,_SPURR(r1)
	mfspr	r3,SPRN_PURR
	std	r3,_PURR(r1)
	mfspr	r3,SPRN_TSCR
	std	r3,_TSCR(r1)
	mfspr	r3,SPRN_DSCR
	std	r3,_DSCR(r1)
	mfspr	r3,SPRN_AMOR
	std	r3,_AMOR(r1)
	mfspr	r3,SPRN_WORT
	std	r3,_WORT(r1)
	mfspr	r3,SPRN_WORC
	std	r3,_WORC(r1)
	IDLE_STATE_ENTER_SEQ(PPC_WINKLE)

_GLOBAL(power7_idle)
	/* Now check if user or arch enabled NAP mode */
	LOAD_REG_ADDRBASE(r3,powersave_nap)
	lwz	r4,ADDROFF(powersave_nap)(r3)
	cmpwi	0,r4,0
	beqlr
	li	r3, 1
	/* fall through */

_GLOBAL(power7_nap)
	mr	r4,r3
	li	r3,PNV_THREAD_NAP
	LOAD_REG_ADDR(r5, power7_enter_nap_mode)
	b	power_powersave_common
	/* No return */

_GLOBAL(power7_sleep)
	li	r3,PNV_THREAD_SLEEP
	li	r4,1
	LOAD_REG_ADDR(r5, power7_enter_nap_mode)
	b	power_powersave_common
	/* No return */

_GLOBAL(power7_winkle)
	li	r3,PNV_THREAD_WINKLE
	li	r4,1
	LOAD_REG_ADDR(r5, power7_enter_nap_mode)
	b	power_powersave_common
	/* No return */

_GLOBAL(power7_wakeup_tb_loss)
	ld	r2,PACATOC(r13);
	ld	r1,PACAR1(r13)
	/*
	 * Before entering any idle state, the NVGPRs are saved in the stack
	 * and they are restored before switching to the process context. Hence
	 * until they are restored, they are free to be used.
	 *
	 * Save SRR1 and LR in NVGPRs as they might be clobbered in
	 * opal_call_realmode (called in CHECK_HMI_INTERRUPT). SRR1 is required
	 * to determine the wakeup reason if we branch to kvm_start_guest. LR
	 * is required to return back to reset vector after hypervisor state
	 * restore is complete.
	 */
	mflr	r17
	mfspr	r16,SPRN_SRR1
BEGIN_FTR_SECTION
	CHECK_HMI_INTERRUPT
END_FTR_SECTION_IFSET(CPU_FTR_HVMODE)

	lbz	r7,PACA_THREAD_MASK(r13)
	ld	r14,PACA_CORE_IDLE_STATE_PTR(r13)
lwarx_loop2:
	lwarx	r15,0,r14
	andi.	r9,r15,PNV_CORE_IDLE_LOCK_BIT
	/*
	 * Lock bit is set in one of the 2 cases-
	 * a. In the sleep/winkle enter path, the last thread is executing
	 * fastsleep workaround code.
	 * b. In the wake up path, another thread is executing fastsleep
	 * workaround undo code or resyncing timebase or restoring context
	 * In either case loop until the lock bit is cleared.
	 */
	bnel	core_idle_lock_held

	cmpwi	cr2,r15,0
	lbz	r4,PACA_SUBCORE_SIBLING_MASK(r13)
	and	r4,r4,r15
	cmpwi	cr1,r4,0	/* Check if first in subcore */

	/*
	 * At this stage
	 * cr1 - 0b0100 if first thread to wakeup in subcore
	 * cr2 - 0b0100 if first thread to wakeup in core
	 * cr3-  0b0010 if waking up from sleep or winkle
	 * cr4 - 0b0100 if waking up from winkle
	 */

	or	r15,r15,r7		/* Set thread bit */

	beq	cr1,first_thread_in_subcore

	/* Not first thread in subcore to wake up */
	stwcx.	r15,0,r14
	bne-	lwarx_loop2
	isync
	b	common_exit

first_thread_in_subcore:
	/* First thread in subcore to wakeup */
	ori	r15,r15,PNV_CORE_IDLE_LOCK_BIT
	stwcx.	r15,0,r14
	bne-	lwarx_loop2
	isync

	/*
	 * If waking up from sleep, subcore state is not lost. Hence
	 * skip subcore state restore
	 */
	bne	cr4,subcore_state_restored

	/* Restore per-subcore state */
	ld      r4,_SDR1(r1)
	mtspr   SPRN_SDR1,r4
	ld      r4,_RPR(r1)
	mtspr   SPRN_RPR,r4
	ld	r4,_AMOR(r1)
	mtspr	SPRN_AMOR,r4

subcore_state_restored:
	/*
	 * Check if the thread is also the first thread in the core. If not,
	 * skip to clear_lock.
	 */
	bne	cr2,clear_lock

first_thread_in_core:

	/*
	 * First thread in the core waking up from fastsleep. It needs to
	 * call the fastsleep workaround code if the platform requires it.
	 * Call it unconditionally here. The below branch instruction will
	 * be patched out when the idle states are discovered if platform
	 * does not require workaround.
	 */
.global pnv_fastsleep_workaround_at_exit
pnv_fastsleep_workaround_at_exit:
	b	fastsleep_workaround_at_exit

timebase_resync:
	/* Do timebase resync if we are waking up from sleep. Use cr3 value
	 * set in exceptions-64s.S */
	ble	cr3,clear_lock
	/* Time base re-sync */
	li	r0,OPAL_RESYNC_TIMEBASE
	bl	opal_call_realmode;
	/* TODO: Check r3 for failure */

	/*
	 * If waking up from sleep, per core state is not lost, skip to
	 * clear_lock.
	 */
	bne	cr4,clear_lock

	/* Restore per core state */
	ld	r4,_TSCR(r1)
	mtspr	SPRN_TSCR,r4
	ld	r4,_WORC(r1)
	mtspr	SPRN_WORC,r4

clear_lock:
	andi.	r15,r15,PNV_CORE_IDLE_THREAD_BITS
	lwsync
	stw	r15,0(r14)

common_exit:
	/*
	 * Common to all threads.
	 *
	 * If waking up from sleep, hypervisor state is not lost. Hence
	 * skip hypervisor state restore.
	 */
	bne	cr4,hypervisor_state_restored

	/* Waking up from winkle */

	/* Restore per thread state */
	bl	__restore_cpu_power8

	/* Restore SLB  from PACA */
	ld	r8,PACA_SLBSHADOWPTR(r13)

	.rept	SLB_NUM_BOLTED
	li	r3, SLBSHADOW_SAVEAREA
	LDX_BE	r5, r8, r3
	addi	r3, r3, 8
	LDX_BE	r6, r8, r3
	andis.	r7,r5,SLB_ESID_V@h
	beq	1f
	slbmte	r6,r5
1:	addi	r8,r8,16
	.endr

	ld	r4,_SPURR(r1)
	mtspr	SPRN_SPURR,r4
	ld	r4,_PURR(r1)
	mtspr	SPRN_PURR,r4
	ld	r4,_DSCR(r1)
	mtspr	SPRN_DSCR,r4
	ld	r4,_WORT(r1)
	mtspr	SPRN_WORT,r4

hypervisor_state_restored:

	mtspr	SPRN_SRR1,r16
	mtlr	r17
	blr

fastsleep_workaround_at_exit:
	li	r3,1
	li	r4,0
	li	r0,OPAL_CONFIG_CPU_IDLE_STATE
	bl	opal_call_realmode
	b	timebase_resync
