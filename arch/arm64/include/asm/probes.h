/*
 * arch/arm64/include/asm/probes.h
 *
 * Copyright (C) 2013 Linaro Limited
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */
#ifndef _ARM_PROBES_H
#define _ARM_PROBES_H

enum probes_insn {
	INSN_REJECTED,
	INSN_GOOD_NO_SLOT,
	INSN_GOOD,
};

typedef u32 probes_opcode_t;
struct arch_probes_insn;

typedef void (probes_insn_handler_t) (u32 opcode,
			   struct arch_probes_insn *api,
			   struct pt_regs *);

typedef unsigned long (probes_check_cc)(unsigned long);

/* architecture specific copy of original instruction */
struct arch_probes_insn {
	probes_opcode_t *insn;
	pstate_check_t *pstate_cc;
	probes_insn_handler_t *insn_handler;
	/* restore address after step xol */
	unsigned long restore;
};
#ifdef CONFIG_KPROBES
typedef u32 kprobe_opcode_t;
struct arch_specific_insn {
	struct arch_probes_insn api;
};
#endif

#endif
