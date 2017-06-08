/*
 * test_kprobes: architectural helpers for validating pt_regs
 *		 received on a kprobe.
 *
 * Copyright 2017 Naveen N. Rao <naveen.n.rao@linux.vnet.ibm.com>
 *		  IBM Corporation
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; version 2
 * of the License.
 */

#define pr_fmt(fmt) "Kprobe smoke test (regs): " fmt

#include <asm/ptrace.h>
#include <linux/kernel.h>
#include <linux/kprobes.h>

static struct pt_regs *r;

void arch_kprobe_regs_set_ptregs(struct pt_regs *regs)
{
	r = regs;
}

static int validate_regs(struct kprobe *p, struct pt_regs *regs,
					int kp_on_ftrace, int post_handler)
{
	int i, ret = 1;

	if (!r) {
		pr_err("pt_regs not setup!\n");
		return 0;
	}

	if (regs->gpr[1] + STACK_FRAME_OVERHEAD != (unsigned long)r) {
		/* We'll continue since this may just indicate an incorrect r1 */
		pr_err("pt_regs pointer/r1 doesn't point where we expect!\n");
		ret = 0;
	}

	for (i = 0; i < 32; i++) {
		/* KPROBES_ON_FTRACE may have stomped r0 in the prologue */
		if (r->gpr[i] != regs->gpr[i] && (!kp_on_ftrace || i != 0)) {
			pr_err("gpr[%d] expected: 0x%lx, received: 0x%lx\n",
						i, r->gpr[i], regs->gpr[i]);
			ret = 0;
		}
	}

	if (r->ctr != regs->ctr) {
		pr_err("ctr expected: 0x%lx, received: 0x%lx\n",
					r->ctr, regs->ctr);
		ret = 0;
	}

	if (r->link != regs->link && !kp_on_ftrace) {
		pr_err("link expected: 0x%lx, received: 0x%lx\n",
					r->link, regs->link);
		ret = 0;
	}

	/* KPROBES_ON_FTRACE *must* have clobbered link */
	if (r->link == regs->link && kp_on_ftrace) {
		pr_err("link register not clobbered for KPROBES_ON_FTRACE!\n");
		ret = 0;
	}

	if (r->xer != regs->xer) {
		pr_err("xer expected: 0x%lx, received: 0x%lx\n",
					r->xer, regs->xer);
		ret = 0;
	}

	if (r->ccr != regs->ccr) {
		pr_err("ccr expected: 0x%lx, received: 0x%lx\n",
					r->ccr, regs->ccr);
		ret = 0;
	}

	if (!post_handler && regs->nip != (unsigned long)p->addr) {
		pr_err("nip expected: 0x%lx, received: 0x%lx\n",
					(unsigned long)p->addr, regs->nip);
		ret = 0;
	}

	if (post_handler &&
		regs->nip != (unsigned long)p->addr + sizeof(kprobe_opcode_t)) {
		pr_err("post_handler: nip expected: 0x%lx, received: 0x%lx\n",
				(unsigned long)p->addr + sizeof(kprobe_opcode_t),
				regs->nip);
		ret = 0;
	}

	return ret;
}

int arch_kprobe_regs_pre_handler(struct kprobe *p, struct pt_regs *regs)
{
	return validate_regs(p, regs, 0, 0);
}

int arch_kprobe_regs_post_handler(struct kprobe *p, struct pt_regs *regs,
							unsigned long flags)
{
	return validate_regs(p, regs, 0, 1);
}

#ifdef CONFIG_KPROBES_ON_FTRACE
int arch_kp_on_ftrace_pre_handler(struct kprobe *p, struct pt_regs *regs)
{
	return validate_regs(p, regs, 1, 0);
}
#endif
