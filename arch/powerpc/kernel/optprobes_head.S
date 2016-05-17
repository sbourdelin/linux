/*
 * Code to prepare detour buffer for optprobes in kernel.
 *
 * Copyright 2016, Anju T, IBM Corp.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */

#include <asm/ppc_asm.h>
#include <asm/ptrace.h>
#include <asm/asm-offsets.h>

.global optprobe_template_entry
optprobe_template_entry:
	stdu	r1,-INT_FRAME_SIZE(r1)
	SAVE_GPR(0,r1)
	/* Save the previous SP into stack */
	addi	r0,r1,INT_FRAME_SIZE
	std	0,GPR1(r1)
	SAVE_2GPRS(2,r1)
	SAVE_8GPRS(4,r1)
	SAVE_10GPRS(12,r1)
	SAVE_10GPRS(22,r1)
	/* Save SPRS */
	mfcfar	r5
	std	r5,_NIP(r1)
	mfmsr	r5
	std	r5,_MSR(r1)
	mfctr	r5
	std	r5,_CTR(r1)
	mflr	r5
	std	r5,_LINK(r1)
	mfspr	r5,SPRN_XER
	std	r5,_XER(r1)
	li	r5,0
	std	r5,_TRAP(r1)
	mfdar	r5
	std	r5,_DAR(r1)
	mfdsisr	r5
	std	r5,_DSISR(r1)
	/* Pass parameters for optimized_callback */
.global optprobe_template_op_address1
optprobe_template_op_address1:
	nop
	nop
	nop
	nop
	nop
	addi	r4,r1,STACK_FRAME_OVERHEAD
	/* Branch to the prehandler */
.global optprobe_template_call_handler
optprobe_template_call_handler:
	nop
	/* Pass parameters for instruction emulation */
	addi	r3,r1,STACK_FRAME_OVERHEAD
.global optprobe_template_insn
optprobe_template_insn:
	nop
	nop
	/* Branch to instruction emulation  */
.global optprobe_template_call_emulate
optprobe_template_call_emulate:
	nop
.global optprobe_template_op_address2
optprobe_template_op_address2:
	nop
	nop
	nop
	nop
	nop
	addi	r4,r1,STACK_FRAME_OVERHEAD
	/* Branch to create_return_branch() function */
.global optprobe_template_ret_branch
optprobe_template_ret_branch:
	nop
	/* Restore the registers */
	ld	r5,_MSR(r1)
	mtmsr	r5
	ld	r5,_CTR(r1)
	mtctr	r5
	ld	r5,_LINK(r1)
	mtlr	r5
	ld	r5,_XER(r1)
	mtxer	r5
	ld	r5,_DAR(r1)
	mtdar	r5
	ld	r5,_DSISR(r1)
	mtdsisr	r5
	REST_GPR(0,r1)
	REST_2GPRS(2,r1)
	REST_8GPRS(4,r1)
	REST_10GPRS(12,r1)
	REST_10GPRS(22,r1)
	/* Restore the previous SP */
	addi	r1,r1,INT_FRAME_SIZE
	/* Jump back to the normal workflow from trampoline */
.global optprobe_template_ret
optprobe_template_ret:
	nop
.global optprobe_template_end
optprobe_template_end:
