/*
 * test_kprobe_regs: architectural helpers for validating pt_regs
 *		     received on a kprobe.
 *
 * Copyright 2017 Naveen N. Rao <naveen.n.rao@linux.vnet.ibm.com>
 *		  IBM Corporation
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; version 2
 * of the License.
 */

#include <asm/ppc_asm.h>
#include <asm/asm-offsets.h>
#include <asm/ptrace.h>

_GLOBAL(arch_kprobe_regs_function)
	mflr	r0
	std	r0, LRSAVE(r1)
	stdu	r1, -SWITCH_FRAME_SIZE(r1)

	/* Tell pre handler about our pt_regs location */
	addi	r3, r1, STACK_FRAME_OVERHEAD
	bl	arch_kprobe_regs_set_ptregs

	/* Load back our true LR */
	ld	r0, (SWITCH_FRAME_SIZE + LRSAVE)(r1)
	mtlr	r0

	/* Save all SPRs that we care about */
	mfctr	r0
	std	r0, _CTR(r1)
	mflr	r0
	std	r0, _LINK(r1)
	mfspr	r0, SPRN_XER
	std	r0, _XER(r1)
	mfcr	r0
	std	r0, _CCR(r1)

	/* Now, save all GPRs */
	SAVE_2GPRS(0, r1)
	SAVE_10GPRS(2, r1)
	SAVE_10GPRS(12, r1)
	SAVE_10GPRS(22, r1)

	/* We're now ready to be probed */
.global arch_kprobe_regs_probepoint
arch_kprobe_regs_probepoint:
	nop

#ifdef CONFIG_KPROBES_ON_FTRACE
	/* Let's also test KPROBES_ON_FTRACE */
	bl	kprobe_regs_kp_on_ftrace_target
	nop
#endif

	/* All done */
	addi	r1, r1, SWITCH_FRAME_SIZE
	ld	r0, LRSAVE(r1)
	mtlr	r0
	blr
