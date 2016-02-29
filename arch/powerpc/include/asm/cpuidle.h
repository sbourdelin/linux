#ifndef _ASM_POWERPC_CPUIDLE_H
#define _ASM_POWERPC_CPUIDLE_H

#ifdef CONFIG_PPC_POWERNV
/* Used in powernv idle state management */
#define PNV_THREAD_RUNNING              0
#define PNV_THREAD_NAP                  1
#define PNV_THREAD_SLEEP                2
#define PNV_THREAD_WINKLE               3
#define PNV_CORE_IDLE_LOCK_BIT          0x100
#define PNV_CORE_IDLE_THREAD_BITS       0x0FF

#ifndef __ASSEMBLY__
extern u32 pnv_fastsleep_workaround_at_entry[];
extern u32 pnv_fastsleep_workaround_at_exit[];
#endif

/*
 * IDLE_STATE_PREP - Will be called in preparation for entering
 * any hardware idle state. Since idle states can result in a
 * state loss, we create a regs frame on the stack, fill it up
 * with the state we care about and stick a pointer to it in
 * PACAR1. Use interrupt stack frame for this purpose.
 * r3 - contains idle state to be entered
 * r13 - PACA pointer
 */
#define IDLE_STATE_PREP						\
	mflr	r0;						\
	std	r0,16(r1);					\
	stdu	r1,-INT_FRAME_SIZE(r1);				\
	std	r0,_LINK(r1);					\
	std	r0,_NIP(r1);					\
								\
	/* Hard disable interrupts */				\
	mfmsr	r9;						\
	rldicl	r9,r9,48,1;					\
	rotldi	r9,r9,16;					\
	mtmsrd	r9,1;		/* hard-disable interrupts */	\
								\
	/* Check if something happened while soft-disabled */	\
	lbz	r0,PACAIRQHAPPENED(r13);			\
	andi.	r0,r0,~PACA_IRQ_HARD_DIS@l;			\
	beq	1f;						\
	cmpwi	cr0,r4,0;					\
	beq	1f;						\
	addi	r1,r1,INT_FRAME_SIZE;				\
	ld	r0,16(r1);					\
	li	r3,0;			/* Return 0 (no nap) */	\
	mtlr	r0;						\
	blr;							\
1:	/* We mark irqs hard disabled as this is the state	\
	 * we'll  be in when returning and we need to tell	\
	 * arch_local_irq_restore() about it */			\
	li	r0,PACA_IRQ_HARD_DIS;				\
	stb	r0,PACAIRQHAPPENED(r13);			\
								\
	/* We haven't lost state ... yet */			\
	li	r0,0;						\
	stb	r0,PACA_NAPSTATELOST(r13);			\
								\
	/* Continue saving state */				\
	SAVE_GPR(2, r1);					\
	SAVE_NVGPRS(r1);					\
	mfcr	r4;						\
	std	r4,_CCR(r1);					\
	std	r9,_MSR(r1);					\
	std	r1,PACAR1(r13);					\

/*
 * We use interrupt stack to save and restore few registers like
 * PC, CR, LR and NVGPRs while enter/exiting idle states. Since
 * volatile registers don't need to be saved/restored use that space
 * instead to save/restore sprs which lose state during winkle.
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


#endif

#endif
