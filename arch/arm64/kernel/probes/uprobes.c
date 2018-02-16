/*
 * Copyright (C) 2014-2016 Pratyush Anand <panand@redhat.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/highmem.h>
#include <linux/ptrace.h>
#include <linux/uprobes.h>
#include <asm/cacheflush.h>

#include "decode-insn.h"
#include "decode.h"
#include "decode-arm.h"
#include <../../../arm/include/asm/opcodes.h>

#define UPROBE_INV_FAULT_CODE	UINT_MAX

uprobe_opcode_t get_swbp_insn(void)
{
	if (is_compat_task())
		return AARCH32_BREAK_ARM;
	else
		return UPROBE_SWBP_INSN;
}

bool is_swbp_insn(uprobe_opcode_t *insn)
{
	return ((__mem_to_opcode_arm(*insn) & 0x0fffffff) ==
				(AARCH32_BREAK_ARM & 0x0fffffff)) ||
				*insn == UPROBE_SWBP_INSN;
}

int set_swbp(struct arch_uprobe *auprobe, struct mm_struct *mm,
	     unsigned long vaddr)
{
	if (auprobe->arch == UPROBE_AARCH32)
		return uprobe_write_opcode(mm, vaddr,
				__opcode_to_mem_arm(auprobe->bp_insn));
	else
		return uprobe_write_opcode(mm, vaddr, UPROBE_SWBP_INSN);
}

int set_orig_insn(struct arch_uprobe *auprobe, struct mm_struct *mm,
		unsigned long vaddr)
{
	if (auprobe->arch == UPROBE_AARCH32)
		return uprobe_write_opcode(mm, vaddr, auprobe->orig_insn);
	else
		return uprobe_write_opcode(mm, vaddr,
				*(uprobe_opcode_t *)&auprobe->insn);
}

void arch_uprobe_copy_ixol(struct page *page, unsigned long vaddr,
		void *src, unsigned long len)
{
	void *xol_page_kaddr = kmap_atomic(page);
	void *dst = xol_page_kaddr + (vaddr & ~PAGE_MASK);

	/* Initialize the slot */
	memcpy(dst, src, len);

	/* flush caches (dcache/icache) */
	sync_icache_aliases(dst, len);

	kunmap_atomic(xol_page_kaddr);
}

unsigned long uprobe_get_swbp_addr(struct pt_regs *regs)
{
	return instruction_pointer(regs);
}

int arch_uprobe_analyze_insn(struct arch_uprobe *auprobe, struct mm_struct *mm,
		unsigned long addr)
{
	probes_opcode_t insn;
	enum probes_insn retval;
	unsigned int bpinsn;

	insn = *(probes_opcode_t *)(&auprobe->insn[0]);

	if (!IS_ALIGNED(addr, AARCH64_INSN_SIZE))
		return -EINVAL;

	/* check if AARCH32 */
	if (is_compat_task()) {

		/* Thumb is not supported yet */
		if (addr & 0x3)
			return -EINVAL;

		retval = arm_probes_decode_insn(insn, &auprobe->api, false,
					     uprobes_probes_actions, NULL);
		auprobe->arch = UPROBE_AARCH32;

		/*
		 * original instruction could have been modified
		 * when preparing for xol on AArch32
		 */
		auprobe->orig_insn = insn;

		bpinsn = AARCH32_BREAK_ARM & 0x0fffffff;
		if (insn >= 0xe0000000) /* Unconditional instruction */
			bpinsn |= 0xe0000000;
		else /* Copy condition from insn */
			bpinsn |= insn & 0xf0000000;

		auprobe->bp_insn = bpinsn;
	} else {
		insn = *(probes_opcode_t *)(&auprobe->insn[0]);
		retval = arm64_probes_decode_insn(insn, &auprobe->api);
		auprobe->arch = UPROBE_AARCH64;
	}

	switch (retval) {
	case INSN_REJECTED:
		return -EINVAL;

	case INSN_GOOD_NO_SLOT:
		auprobe->simulate = true;
		break;

	default:
		break;
	}

	return 0;
}

int arch_uprobe_pre_xol(struct arch_uprobe *auprobe, struct pt_regs *regs)
{
	struct uprobe_task *utask = current->utask;

	if (auprobe->prehandler)
		auprobe->prehandler(auprobe, &utask->autask, regs);

	/* Initialize with an invalid fault code to detect if ol insn trapped */
	current->thread.fault_code = UPROBE_INV_FAULT_CODE;

	/* Instruction points to execute ol */
	instruction_pointer_set(regs, utask->xol_vaddr);

	user_enable_single_step(current);

	return 0;
}

int arch_uprobe_post_xol(struct arch_uprobe *auprobe, struct pt_regs *regs)
{
	struct uprobe_task *utask = current->utask;

	WARN_ON_ONCE(current->thread.fault_code != UPROBE_INV_FAULT_CODE);

	/* Instruction points to execute next to breakpoint address */
	instruction_pointer_set(regs, utask->vaddr + 4);

	user_disable_single_step(current);

	if (auprobe->posthandler)
		auprobe->posthandler(auprobe, &utask->autask, regs);

	return 0;
}
bool arch_uprobe_xol_was_trapped(struct task_struct *t)
{
	/*
	 * Between arch_uprobe_pre_xol and arch_uprobe_post_xol, if an xol
	 * insn itself is trapped, then detect the case with the help of
	 * invalid fault code which is being set in arch_uprobe_pre_xol
	 */
	if (t->thread.fault_code != UPROBE_INV_FAULT_CODE)
		return true;

	return false;
}

bool arch_uprobe_ignore(struct arch_uprobe *auprobe, struct pt_regs *regs)
{
	probes_opcode_t insn = *(probes_opcode_t *)(&auprobe->insn[0]);
	pstate_check_t *check = (*aarch32_opcode_cond_checks[insn >> 28]);

	if (auprobe->arch == UPROBE_AARCH64)
		return false;

	if (!check(regs->pstate & 0xffffffff)) {
		instruction_pointer_set(regs, instruction_pointer(regs) + 4);
		return true;
	}
	return false;
}

bool arch_uprobe_skip_sstep(struct arch_uprobe *auprobe, struct pt_regs *regs)
{
	probes_opcode_t insn;

	if (!auprobe->simulate)
		return false;

	insn = *(probes_opcode_t *)(&auprobe->insn[0]);

	if (auprobe->api.insn_handler)
		auprobe->api.insn_handler(insn, &auprobe->api, regs);

	return true;
}

void arch_uprobe_abort_xol(struct arch_uprobe *auprobe, struct pt_regs *regs)
{
	struct uprobe_task *utask = current->utask;

	/*
	 * Task has received a fatal signal, so reset back to probbed
	 * address.
	 */
	instruction_pointer_set(regs, utask->vaddr);

	user_disable_single_step(current);
}

bool arch_uretprobe_is_alive(struct return_instance *ret, enum rp_check ctx,
		struct pt_regs *regs)
{
	/*
	 * If a simple branch instruction (B) was called for retprobed
	 * assembly label then return true even when regs->sp and ret->stack
	 * are same. It will ensure that cleanup and reporting of return
	 * instances corresponding to callee label is done when
	 * handle_trampoline for called function is executed.
	 */
	if (ctx == RP_CHECK_CHAIN_CALL)
		return regs->sp <= ret->stack;
	else
		return regs->sp < ret->stack;
}

unsigned long
arch_uretprobe_hijack_return_addr(unsigned long trampoline_vaddr,
				  struct pt_regs *regs)
{
	unsigned long orig_ret_vaddr;

	/* Replace the return addr with trampoline addr */
	if (is_compat_task()) {
		orig_ret_vaddr = link_register(regs);
		link_register_set(regs, trampoline_vaddr);
	} else {
		orig_ret_vaddr = procedure_link_pointer(regs);
		procedure_link_pointer_set(regs, trampoline_vaddr);
	}

	return orig_ret_vaddr;
}

int arch_uprobe_exception_notify(struct notifier_block *self,
				 unsigned long val, void *data)
{
	return NOTIFY_DONE;
}

static int uprobe_breakpoint_handler(struct pt_regs *regs,
		unsigned int esr)
{
	if (user_mode(regs) && uprobe_pre_sstep_notifier(regs))
		return DBG_HOOK_HANDLED;

	return DBG_HOOK_ERROR;
}

static int uprobe_single_step_handler(struct pt_regs *regs,
		unsigned int esr)
{
	struct uprobe_task *utask = current->utask;

	if (user_mode(regs)) {
		WARN_ON(utask &&
			(instruction_pointer(regs) != utask->xol_vaddr + 4));

		if (uprobe_post_sstep_notifier(regs))
			return DBG_HOOK_HANDLED;
	}

	return DBG_HOOK_ERROR;
}

/* uprobe breakpoint handler hook */
static struct break_hook uprobes_break_hook = {
	.esr_mask = BRK64_ESR_MASK,
	.esr_val = BRK64_ESR_UPROBES,
	.fn = uprobe_breakpoint_handler,
};

/* uprobe single step handler hook */
static struct step_hook uprobes_step_hook = {
	.fn = uprobe_single_step_handler,
};

static int __init arch_init_uprobes(void)
{
	register_break_hook(&uprobes_break_hook);
	register_step_hook(&uprobes_step_hook);

	return 0;
}

device_initcall(arch_init_uprobes);
