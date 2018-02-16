/*
 * Copyright (C) 2014-2016 Pratyush Anand <panand@redhat.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#ifndef _ASM_UPROBES_H
#define _ASM_UPROBES_H

#include <asm/debug-monitors.h>
#include <asm/insn.h>
#include <asm/probes.h>

#define MAX_UINSN_BYTES		AARCH64_INSN_SIZE

#define UPROBE_SWBP_INSN	BRK64_OPCODE_UPROBES
#define UPROBE_SWBP_INSN_SIZE	AARCH64_INSN_SIZE
#define UPROBE_XOL_SLOT_BYTES	MAX_UINSN_BYTES

typedef u32 uprobe_opcode_t;

struct arch_uprobe_task {
	u64 backup;
};

enum uprobe_arch {
	UPROBE_AARCH64,
	UPROBE_AARCH32
};

struct arch_uprobe {
	union {
		u8 insn[MAX_UINSN_BYTES];
		u8 ixol[MAX_UINSN_BYTES];
	};

	probes_opcode_t orig_insn;
	probes_opcode_t bp_insn;

	struct arch_probes_insn api;
	bool simulate;
	u64 pcreg;
	enum uprobe_arch arch;

	void (*prehandler)(struct arch_uprobe *auprobe,
			   struct arch_uprobe_task *autask,
			   struct pt_regs *regs);
	void (*posthandler)(struct arch_uprobe *auprobe,
		    struct arch_uprobe_task *autask,
		    struct pt_regs *regs);
};

#endif
