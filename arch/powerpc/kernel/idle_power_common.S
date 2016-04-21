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

	.text

/*
 * Pass requested state in r3:
 *	r3 - PNV_THREAD_NAP/SLEEP/WINKLE
 *
 * To check IRQ_HAPPENED in r4
 * 	0 - don't check
 * 	1 - check
 *
 * Address to 'rfid' to in r5
 */
_GLOBAL(power_powersave_common)
	/* Use r3 to pass state nap/sleep/winkle */
	/* NAP is a state loss, we create a regs frame on the
	 * stack, fill it up with the state we care about and
	 * stick a pointer to it in PACAR1. We really only
	 * need to save PC, some CR bits and the NV GPRs,
	 * but for now an interrupt frame will do.
	 */
	mflr	r0
	std	r0,16(r1)
	stdu	r1,-INT_FRAME_SIZE(r1)
	std	r0,_LINK(r1)
	std	r0,_NIP(r1)

	/* Hard disable interrupts */
	mfmsr	r9
	rldicl	r9,r9,48,1
	rotldi	r9,r9,16
	mtmsrd	r9,1			/* hard-disable interrupts */

	/* Check if something happened while soft-disabled */
	lbz	r0,PACAIRQHAPPENED(r13)
	andi.	r0,r0,~PACA_IRQ_HARD_DIS@l
	beq	1f
	cmpwi	cr0,r4,0
	beq	1f
	addi	r1,r1,INT_FRAME_SIZE
	ld	r0,16(r1)
	li	r3,0			/* Return 0 (no nap) */
	mtlr	r0
	blr

1:	/* We mark irqs hard disabled as this is the state we'll
	 * be in when returning and we need to tell arch_local_irq_restore()
	 * about it
	 */
	li	r0,PACA_IRQ_HARD_DIS
	stb	r0,PACAIRQHAPPENED(r13)

	/* We haven't lost state ... yet */
	li	r0,0
	stb	r0,PACA_NAPSTATELOST(r13)

	/* Continue saving state */
	SAVE_GPR(2, r1)
	SAVE_NVGPRS(r1)
	mfcr	r4
	std	r4,_CCR(r1)
	std	r9,_MSR(r1)
	std	r1,PACAR1(r13)

	/*
	 * Go to real mode to do the nap, as required by the architecture.
	 * Also, we need to be in real mode before setting hwthread_state,
	 * because as soon as we do that, another thread can switch
	 * the MMU context to the guest.
	 */
	LOAD_REG_IMMEDIATE(r7, MSR_IDLE)
	li	r6, MSR_RI
	andc	r6, r9, r6
	mtmsrd	r6, 1		/* clear RI before setting SRR0/1 */
	mtspr	SPRN_SRR0, r5
	mtspr	SPRN_SRR1, r7
	rfid
	/* No return */

/*
 * Called from reset vector. Check whether we have woken up with
 * hypervisor state loss. If yes, restore hypervisor state and return
 * back to reset vector.
 */
_GLOBAL(power7_restore_hyp_resource)
	GET_PACA(r13)
BEGIN_FTR_SECTION_NESTED(888)
	/*
	 * POWER ISA 3. Use PSSCR to determine if we
	 * are waking up from deep idle state
	 */
	LOAD_REG_ADDRBASE(r5,pnv_first_deep_stop_state)
	ld	r4,ADDROFF(pnv_first_deep_stop_state)(r5)

	mfspr	r5,SPRN_PSSCR
	/*
	 * 0-4 bits correspond to Power-Saving Level Status
	 * which indicates the idle state we are waking up from
	 */
	rldicl  r5,r5,4,60
	cmpd	r5,r4
	bge	power_stop_wakeup_hyp_loss
	/*
	 * Waking up without hypervisor state loss. Return to
	 * reset vector
	 */
	blr

END_FTR_SECTION_NESTED(CPU_FTR_STOP_INST,CPU_FTR_STOP_INST,888)
	/*
	 * POWER ISA 2.07 or less.
	 * Check if last bit of HSPGR0 is set. This indicates whether we are
	 * waking up from winkle.
	 */
	clrldi	r5,r13,63
	clrrdi	r13,r13,1
	cmpwi	cr4,r5,1
	mtspr	SPRN_HSPRG0,r13

	lbz	r0,PACA_THREAD_IDLE_STATE(r13)
	cmpwi   cr2,r0,PNV_THREAD_NAP
	bgt     cr2,power7_wakeup_tb_loss	/* Either sleep or Winkle */

	/*
	 * We fall through here if PACA_THREAD_IDLE_STATE shows we are waking
	 * up from nap. At this stage CR3 shouldn't contains 'gt' since that
	 * indicates we are waking with hypervisor state loss from nap.
	 */
	bgt	cr3,.

	blr

/*
 * R3 here contains the value that will be returned to the caller
 * of power7_nap.
 */
_GLOBAL(power7_wakeup_loss)
	ld	r1,PACAR1(r13)
BEGIN_FTR_SECTION
	CHECK_HMI_INTERRUPT
END_FTR_SECTION_IFSET(CPU_FTR_HVMODE)
	REST_NVGPRS(r1)
	REST_GPR(2, r1)
	ld	r6,_CCR(r1)
	ld	r4,_MSR(r1)
	ld	r5,_NIP(r1)
	addi	r1,r1,INT_FRAME_SIZE
	mtcr	r6
	mtspr	SPRN_SRR1,r4
	mtspr	SPRN_SRR0,r5
	rfid

/*
 * R3 here contains the value that will be returned to the caller
 * of power7_nap.
 */
_GLOBAL(power7_wakeup_noloss)
	lbz	r0,PACA_NAPSTATELOST(r13)
	cmpwi	r0,0
	bne	power7_wakeup_loss
BEGIN_FTR_SECTION
	CHECK_HMI_INTERRUPT
END_FTR_SECTION_IFSET(CPU_FTR_HVMODE)
	ld	r1,PACAR1(r13)
	ld	r6,_CCR(r1)
	ld	r4,_MSR(r1)
	ld	r5,_NIP(r1)
	addi	r1,r1,INT_FRAME_SIZE
	mtcr	r6
	mtspr	SPRN_SRR1,r4
	mtspr	SPRN_SRR0,r5
	rfid
