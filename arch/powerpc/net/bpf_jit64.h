/*
 * bpf_jit64.h: BPF JIT compiler for PPC64
 *
 * Copyright 2016 Naveen N. Rao <naveen.n.rao@linux.vnet.ibm.com>
 *		  IBM Corporation
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; version 2
 * of the License.
 */
#ifndef _BPF_JIT64_H
#define _BPF_JIT64_H

#include "bpf_jit.h"

/* Stack layout:
 *
 *		[	prev sp		] <-------------
 *		[   nv gpr save area	] 6*8		|
 * fp (r31) -->	[   ebpf stack space	] 512		|
 *		[  local/tmp var space	] 16		|
 *		[     frame header	] 32/112	|
 * sp (r1) --->	[    stack pointer	] --------------
 */

/* for bpf JIT code internal usage */
#define BPF_PPC_STACK_LOCALS	16
/* for gpr non volatile registers BPG_REG_6 to 10 */
#define BPF_PPC_STACK_SAVE	(6*8)
/* Ensure this is quadword aligned */
#define BPF_PPC_STACKFRAME	(STACK_FRAME_MIN_SIZE + BPF_PPC_STACK_LOCALS + \
				 MAX_BPF_STACK + BPF_PPC_STACK_SAVE)

/* Truncate to 32-bit */
#define PPC_CLEAR32()	   do {						      \
			   if (BPF_CLASS(code) == BPF_ALU)		      \
				PPC_RLWINM(dst_reg, dst_reg, 0, 0, 31);	      \
			   } while (0)

#define SEEN_FUNC	0x1000 /* might call external helpers */
#define SEEN_STACK	0x2000 /* uses BPF stack */

struct codegen_context {
	/*
	 * This is used to track register usage as well
	 * as calls to external helpers.
	 * - register usage is tracked with corresponding
	 *   bits (r3-r10 and r26-r31)
	 * - rest of the bits can be used to track other
	 *   things -- for now, we use bits 16 to 23
	 *   encoded in SEEN_* macros above
	 */
	unsigned int seen;
	unsigned int idx;
};

#endif
