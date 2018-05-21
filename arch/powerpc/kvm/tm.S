/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 2, as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * Derived from book3s_hv_rmhandlers.S,  which are:
 *
 * Copyright 2011 Paul Mackerras, IBM Corp. <paulus@au1.ibm.com>
 *
 */

#include <asm/reg.h>
#include <asm/ppc_asm.h>
#include <asm/asm-offsets.h>
#include <asm/export.h>
#include <asm/tm.h>
#include <asm/cputable.h>

#ifdef CONFIG_PPC_TRANSACTIONAL_MEM
#define VCPU_GPRS_TM(reg) (((reg) * ULONG_SIZE) + VCPU_GPR_TM)

/*
 * Save transactional state and TM-related registers.
 * Called with r9 pointing to the vcpu struct.
 * This can modify all checkpointed registers, but
 * restores r1, r2 and r9 (vcpu pointer) before exit.
 */
_GLOBAL(kvmppc_save_tm)
	mflr	r0
	std	r0, PPC_LR_STKOFF(r1)
	stdu	r1, -PPC_MIN_STKFRM(r1)

	/* Turn on TM. */
	mfmsr	r8
	li	r0, 1
	rldimi	r8, r0, MSR_TM_LG, 63-MSR_TM_LG
	mtmsrd	r8

#ifdef CONFIG_KVM_BOOK3S_HV_POSSIBLE
	ld	r5, VCPU_MSR(r9)
	rldicl. r5, r5, 64 - MSR_TS_S_LG, 62
	beq	1f	/* TM not active in guest. */
#endif

	std	r1, HSTATE_HOST_R1(r13)
	li	r3, TM_CAUSE_KVM_RESCHED

#ifdef CONFIG_KVM_BOOK3S_HV_POSSIBLE
BEGIN_FTR_SECTION
	lbz	r0, HSTATE_FAKE_SUSPEND(r13) /* Were we fake suspended? */
	cmpwi	r0, 0
	beq	3f
	rldicl. r8, r8, 64 - MSR_TS_S_LG, 62 /* Did we actually hrfid? */
	beq	4f
BEGIN_FTR_SECTION_NESTED(96)
	bl	pnv_power9_force_smt4_catch
END_FTR_SECTION_NESTED(CPU_FTR_P9_TM_XER_SO_BUG, CPU_FTR_P9_TM_XER_SO_BUG, 96)
	nop
	b	6f
3:
	/* Emulation of the treclaim instruction needs TEXASR before treclaim */
	mfspr	r6, SPRN_TEXASR
	std	r6, VCPU_ORIG_TEXASR(r9)
6:
END_FTR_SECTION_IFSET(CPU_FTR_P9_TM_HV_ASSIST)
#endif

	/* Clear the MSR RI since r1, r13 are all going to be foobar. */
	li	r5, 0
	mtmsrd	r5, 1

	/* All GPRs are volatile at this point. */
	TRECLAIM(R3)

	/* Temporarily store r13 and r9 so we have some regs to play with */
	SET_SCRATCH0(r13)
	GET_PACA(r13)
	std	r9, PACATMSCRATCH(r13)

#ifdef CONFIG_KVM_BOOK3S_HV_POSSIBLE
	/* If doing TM emulation on POWER9 DD2.2, check for fake suspend mode */
BEGIN_FTR_SECTION
	lbz	r9, HSTATE_FAKE_SUSPEND(r13)
	cmpwi	r9, 0
	beq	2f
	/*
	 * We were in fake suspend, so we are not going to save the
	 * register state as the guest checkpointed state (since
	 * we already have it), therefore we can now use any volatile GPR.
	 */
	/* Reload stack pointer and TOC. */
	ld	r1, HSTATE_HOST_R1(r13)
	ld	r2, PACATOC(r13)
	/* Set MSR RI now we have r1 and r13 back. */
	li	r5, MSR_RI
	mtmsrd	r5, 1
	HMT_MEDIUM
	ld	r6, HSTATE_DSCR(r13)
	mtspr	SPRN_DSCR, r6
BEGIN_FTR_SECTION_NESTED(96)
	bl	pnv_power9_force_smt4_release
END_FTR_SECTION_NESTED(CPU_FTR_P9_TM_XER_SO_BUG, CPU_FTR_P9_TM_XER_SO_BUG, 96)
	nop

4:
	mfspr	r3, SPRN_PSSCR
	/* PSSCR_FAKE_SUSPEND is a write-only bit, but clear it anyway */
	li	r0, PSSCR_FAKE_SUSPEND
	andc	r3, r3, r0
	mtspr	SPRN_PSSCR, r3
	ld	r9, HSTATE_KVM_VCPU(r13)
	/* Don't save TEXASR, use value from last exit in real suspend state */
	b	11f
2:
END_FTR_SECTION_IFSET(CPU_FTR_P9_TM_HV_ASSIST)

	ld	r9, HSTATE_KVM_VCPU(r13)
#endif


	/* Get a few more GPRs free. */
	std	r29, VCPU_GPRS_TM(29)(r9)
	std	r30, VCPU_GPRS_TM(30)(r9)
	std	r31, VCPU_GPRS_TM(31)(r9)

	/* Save away PPR and DSCR soon so don't run with user values. */
	mfspr	r31, SPRN_PPR
	HMT_MEDIUM
	mfspr	r30, SPRN_DSCR
#ifdef CONFIG_KVM_BOOK3S_HV_POSSIBLE
	ld	r29, HSTATE_DSCR(r13)
	mtspr	SPRN_DSCR, r29
#endif

	/* Save all but r9, r13 & r29-r31 */
	reg = 0
	.rept	29
	.if (reg != 9) && (reg != 13)
	std	reg, VCPU_GPRS_TM(reg)(r9)
	.endif
	reg = reg + 1
	.endr
	/* ... now save r13 */
	GET_SCRATCH0(r4)
	std	r4, VCPU_GPRS_TM(13)(r9)
	/* ... and save r9 */
	ld	r4, PACATMSCRATCH(r13)
	std	r4, VCPU_GPRS_TM(9)(r9)

	/* Reload stack pointer and TOC. */
	ld	r1, HSTATE_HOST_R1(r13)
	ld	r2, PACATOC(r13)

	/* Set MSR RI now we have r1 and r13 back. */
	li	r5, MSR_RI
	mtmsrd	r5, 1

	/* Save away checkpinted SPRs. */
	std	r31, VCPU_PPR_TM(r9)
	std	r30, VCPU_DSCR_TM(r9)
	mflr	r5
	mfcr	r6
	mfctr	r7
	mfspr	r8, SPRN_AMR
	mfspr	r10, SPRN_TAR
	mfxer	r11
	std	r5, VCPU_LR_TM(r9)
	stw	r6, VCPU_CR_TM(r9)
	std	r7, VCPU_CTR_TM(r9)
	std	r8, VCPU_AMR_TM(r9)
	std	r10, VCPU_TAR_TM(r9)
	std	r11, VCPU_XER_TM(r9)

	/* Restore r12 as trap number. */
	lwz	r12, VCPU_TRAP(r9)

	/* Save FP/VSX. */
	addi	r3, r9, VCPU_FPRS_TM
	bl	store_fp_state
	addi	r3, r9, VCPU_VRS_TM
	bl	store_vr_state
	mfspr	r6, SPRN_VRSAVE
	stw	r6, VCPU_VRSAVE_TM(r9)
1:
	/*
	 * We need to save these SPRs after the treclaim so that the software
	 * error code is recorded correctly in the TEXASR.  Also the user may
	 * change these outside of a transaction, so they must always be
	 * context switched.
	 */
	mfspr	r7, SPRN_TEXASR
	std	r7, VCPU_TEXASR(r9)
11:
	mfspr	r5, SPRN_TFHAR
	mfspr	r6, SPRN_TFIAR
	std	r5, VCPU_TFHAR(r9)
	std	r6, VCPU_TFIAR(r9)

	addi	r1, r1, PPC_MIN_STKFRM
	ld	r0, PPC_LR_STKOFF(r1)
	mtlr	r0
	blr

/*
 * Restore transactional state and TM-related registers.
 * Called with r4 pointing to the vcpu struct.
 * This potentially modifies all checkpointed registers.
 * It restores r1, r2, r4 from the PACA.
 */
_GLOBAL(kvmppc_restore_tm)
	mflr	r0
	std	r0, PPC_LR_STKOFF(r1)

	/* Turn on TM/FP/VSX/VMX so we can restore them. */
	mfmsr	r5
	li	r6, MSR_TM >> 32
	sldi	r6, r6, 32
	or	r5, r5, r6
	ori	r5, r5, MSR_FP
	oris	r5, r5, (MSR_VEC | MSR_VSX)@h
	mtmsrd	r5

	/*
	 * The user may change these outside of a transaction, so they must
	 * always be context switched.
	 */
	ld	r5, VCPU_TFHAR(r4)
	ld	r6, VCPU_TFIAR(r4)
	ld	r7, VCPU_TEXASR(r4)
	mtspr	SPRN_TFHAR, r5
	mtspr	SPRN_TFIAR, r6
	mtspr	SPRN_TEXASR, r7

#ifdef CONFIG_KVM_BOOK3S_HV_POSSIBLE
	li	r0, 0
	stb	r0, HSTATE_FAKE_SUSPEND(r13)
#endif
#ifdef CONFIG_KVM_BOOK3S_HV_POSSIBLE
	ld	r5, VCPU_MSR(r4)
	rldicl. r5, r5, 64 - MSR_TS_S_LG, 62
	beqlr		/* TM not active in guest */
#endif
	std	r1, HSTATE_HOST_R1(r13)

	/* Make sure the failure summary is set, otherwise we'll program check
	 * when we trechkpt.  It's possible that this might have been not set
	 * on a kvmppc_set_one_reg() call but we shouldn't let this crash the
	 * host.
	 */
	oris	r7, r7, (TEXASR_FS)@h
	mtspr	SPRN_TEXASR, r7

#ifdef CONFIG_KVM_BOOK3S_HV_POSSIBLE
	/*
	 * If we are doing TM emulation for the guest on a POWER9 DD2,
	 * then we don't actually do a trechkpt -- we either set up
	 * fake-suspend mode, or emulate a TM rollback.
	 */
BEGIN_FTR_SECTION
	b	.Ldo_tm_fake_load
END_FTR_SECTION_IFSET(CPU_FTR_P9_TM_HV_ASSIST)
#endif

	/*
	 * We need to load up the checkpointed state for the guest.
	 * We need to do this early as it will blow away any GPRs, VSRs and
	 * some SPRs.
	 */

	mr	r31, r4
	addi	r3, r31, VCPU_FPRS_TM
	bl	load_fp_state
	addi	r3, r31, VCPU_VRS_TM
	bl	load_vr_state
	mr	r4, r31
	lwz	r7, VCPU_VRSAVE_TM(r4)
	mtspr	SPRN_VRSAVE, r7

	ld	r5, VCPU_LR_TM(r4)
	lwz	r6, VCPU_CR_TM(r4)
	ld	r7, VCPU_CTR_TM(r4)
	ld	r8, VCPU_AMR_TM(r4)
	ld	r9, VCPU_TAR_TM(r4)
	ld	r10, VCPU_XER_TM(r4)
	mtlr	r5
	mtcr	r6
	mtctr	r7
	mtspr	SPRN_AMR, r8
	mtspr	SPRN_TAR, r9
	mtxer	r10

	/*
	 * Load up PPR and DSCR values but don't put them in the actual SPRs
	 * till the last moment to avoid running with userspace PPR and DSCR for
	 * too long.
	 */
	ld	r29, VCPU_DSCR_TM(r4)
	ld	r30, VCPU_PPR_TM(r4)

	std	r2, PACATMSCRATCH(r13) /* Save TOC */

	/* Clear the MSR RI since r1, r13 are all going to be foobar. */
	li	r5, 0
	mtmsrd	r5, 1

	/* Load GPRs r0-r28 */
	reg = 0
	.rept	29
	ld	reg, VCPU_GPRS_TM(reg)(r31)
	reg = reg + 1
	.endr

	mtspr	SPRN_DSCR, r29
	mtspr	SPRN_PPR, r30

	/* Load final GPRs */
	ld	29, VCPU_GPRS_TM(29)(r31)
	ld	30, VCPU_GPRS_TM(30)(r31)
	ld	31, VCPU_GPRS_TM(31)(r31)

	/* TM checkpointed state is now setup.  All GPRs are now volatile. */
	TRECHKPT

	/* Now let's get back the state we need. */
	HMT_MEDIUM
	GET_PACA(r13)
#ifdef CONFIG_KVM_BOOK3S_HV_POSSIBLE
	ld	r29, HSTATE_DSCR(r13)
	mtspr	SPRN_DSCR, r29
	ld	r4, HSTATE_KVM_VCPU(r13)
#endif
	ld	r1, HSTATE_HOST_R1(r13)
	ld	r2, PACATMSCRATCH(r13)

	/* Set the MSR RI since we have our registers back. */
	li	r5, MSR_RI
	mtmsrd	r5, 1
9:
	ld	r0, PPC_LR_STKOFF(r1)
	mtlr	r0
	blr

#ifdef CONFIG_KVM_BOOK3S_HV_POSSIBLE
.Ldo_tm_fake_load:
	cmpwi	r5, 1		/* check for suspended state */
	bgt	10f
	stb	r5, HSTATE_FAKE_SUSPEND(r13)
	b	9b		/* and return */
10:	stdu	r1, -PPC_MIN_STKFRM(r1)
	/* guest is in transactional state, so simulate rollback */
	mr	r3, r4
	bl	kvmhv_emulate_tm_rollback
	nop
	ld      r4, HSTATE_KVM_VCPU(r13) /* our vcpu pointer has been trashed */
	addi	r1, r1, PPC_MIN_STKFRM
	b	9b
#endif
#endif
