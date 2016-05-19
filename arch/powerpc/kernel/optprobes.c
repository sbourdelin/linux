/*
 * Code for Kernel probes Jump optimization.
 *
 * Copyright 2016, Anju T, IBM Corp.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */

#include <linux/kprobes.h>
#include <linux/jump_label.h>
#include <linux/types.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <asm/kprobes.h>
#include <asm/ptrace.h>
#include <asm/cacheflush.h>
#include <asm/code-patching.h>
#include <asm/sstep.h>

#define SLOT_SIZE 65536
#define TMPL_CALL_HDLR_IDX	\
	(optprobe_template_call_handler - optprobe_template_entry)
#define TMPL_EMULATE_IDX	\
	(optprobe_template_call_emulate - optprobe_template_entry)
#define TMPL_RET_BRANCH_IDX	\
	(optprobe_template_ret_branch - optprobe_template_entry)
#define TMPL_RET_IDX	\
	(optprobe_template_ret - optprobe_template_entry)
#define TMPL_OP1_IDX	\
	(optprobe_template_op_address1 - optprobe_template_entry)
#define TMPL_OP2_IDX	\
	(optprobe_template_op_address2 - optprobe_template_entry)
#define TMPL_INSN_IDX	\
	(optprobe_template_insn - optprobe_template_entry)
#define TMPL_END_IDX	\
	(optprobe_template_end - optprobe_template_entry)

struct kprobe_ppc_insn_page {
	struct list_head list;
	kprobe_opcode_t *insns;	/* Page of instruction slots */
	struct kprobe_insn_cache *cache;
	int nused;
	int ngarbage;
	char slot_used[];
};

#define PPC_KPROBE_INSN_PAGE_SIZE(slots)	\
	(offsetof(struct kprobe_ppc_insn_page, slot_used) +	\
	(sizeof(char) * (slots)))

enum ppc_kprobe_slot_state {
	SLOT_CLEAN = 0,
	SLOT_DIRTY = 1,
	SLOT_USED = 2,
};

static struct kprobe_insn_cache kprobe_ppc_optinsn_slots = {
	.mutex = __MUTEX_INITIALIZER(kprobe_ppc_optinsn_slots.mutex),
	.pages = LIST_HEAD_INIT(kprobe_ppc_optinsn_slots.pages),
	/* .insn_size is initialized later */
	.nr_garbage = 0,
};

static int ppc_slots_per_page(struct kprobe_insn_cache *c)
{
	/*
	 * Here the #slots per page differs from x86 as we have
	 * only 64KB reserved.
	 */
	return SLOT_SIZE / (c->insn_size * sizeof(kprobe_opcode_t));
}

/* Return 1 if all garbages are collected, otherwise 0. */
static int collect_one_slot(struct kprobe_ppc_insn_page *kip, int idx)
{
	kip->slot_used[idx] = SLOT_CLEAN;
	kip->nused--;
	return 0;
}

static int collect_garbage_slots(struct kprobe_insn_cache *c)
{
	struct kprobe_ppc_insn_page *kip, *next;

	/* Ensure no-one is interrupted on the garbages */
	synchronize_sched();

	list_for_each_entry_safe(kip, next, &c->pages, list) {
		int i;

		if (kip->ngarbage == 0)
			continue;
		kip->ngarbage = 0;	/* we will collect all garbages */
		for (i = 0; i < ppc_slots_per_page(c); i++) {
			if (kip->slot_used[i] == SLOT_DIRTY &&
			    collect_one_slot(kip, i))
				break;
		}
	}
	c->nr_garbage = 0;
	return 0;
}

kprobe_opcode_t  *__ppc_get_optinsn_slot(struct kprobe_insn_cache *c)
{
	struct kprobe_ppc_insn_page *kip;
	kprobe_opcode_t *slot = NULL;

	mutex_lock(&c->mutex);
	list_for_each_entry(kip, &c->pages, list) {
		if (kip->nused < ppc_slots_per_page(c)) {
			int i;

			for (i = 0; i < ppc_slots_per_page(c); i++) {
				if (kip->slot_used[i] == SLOT_CLEAN) {
					kip->slot_used[i] = SLOT_USED;
					kip->nused++;
					slot = kip->insns + (i * c->insn_size);
					goto out;
				}
			}
			/* kip->nused reached max value. */
			kip->nused = ppc_slots_per_page(c);
			WARN_ON(1);
		}
		if (!list_empty(&c->pages)) {
			pr_info("No more slots to allocate\n");
			return NULL;
		}
	}
	kip = kmalloc(PPC_KPROBE_INSN_PAGE_SIZE(ppc_slots_per_page(c)),
		      GFP_KERNEL);
	if (!kip)
		goto out;
	/*
	 * Allocate from the reserved area so as to
	 * ensure the range will be within +/-32MB
	 */
	kip->insns = &optinsn_slot;
	if (!kip->insns) {
		kfree(kip);
		goto out;
	}
	INIT_LIST_HEAD(&kip->list);
	memset(kip->slot_used, SLOT_CLEAN, ppc_slots_per_page(c));
	kip->slot_used[0] = SLOT_USED;
	kip->nused = 1;
	kip->ngarbage = 0;
	kip->cache = c;
	list_add(&kip->list, &c->pages);
	slot = kip->insns;
out:
	mutex_unlock(&c->mutex);
	return slot;
}

kprobe_opcode_t *ppc_get_optinsn_slot(struct optimized_kprobe *op)
{
	/*
	 * The insn slot is allocated from the reserved
	 * area(ie &optinsn_slot).We are not optimizing probes
	 * at module_addr now.
	 */
	kprobe_opcode_t *slot = NULL;

	if (is_kernel_addr(op->kp.addr))
		slot = __ppc_get_optinsn_slot(&kprobe_ppc_optinsn_slots);
	else
		pr_info("Kprobe can not be optimized\n");
	return slot;
}

void __ppc_free_optinsn_slot(struct kprobe_insn_cache *c,
			     kprobe_opcode_t *slot, int dirty)
{
	struct kprobe_ppc_insn_page *kip;

	mutex_lock(&c->mutex);

	list_for_each_entry(kip, &c->pages, list) {
		long idx = ((long)slot - (long)kip->insns) /
				(c->insn_size * sizeof(kprobe_opcode_t));
		if (idx >= 0 && idx < ppc_slots_per_page(c)) {
			WARN_ON(kip->slot_used[idx] != SLOT_USED);
			if (dirty) {
				kip->slot_used[idx] = SLOT_DIRTY;
				kip->ngarbage++;
				if (++c->nr_garbage > ppc_slots_per_page(c))
					collect_garbage_slots(c);
			} else
				collect_one_slot(kip, idx);
			goto out;
		}
	}
	/* Could not free this slot. */
	WARN_ON(1);
out:
	mutex_unlock(&c->mutex);
}

static void ppc_free_optinsn_slot(struct optimized_kprobe *op)
{
	if (!op->optinsn.insn)
		return;
	if (is_kernel_addr((unsigned long)op->kp.addr))
		__ppc_free_optinsn_slot(&kprobe_ppc_optinsn_slots,
					op->optinsn.insn, 0);
}

static void
__arch_remove_optimized_kprobe(struct optimized_kprobe *op, int dirty)
{
	if (op->optinsn.insn) {
		ppc_free_optinsn_slot(op);
		op->optinsn.insn = NULL;
	}
}

static int can_optimize(struct kprobe *p)
{
	struct pt_regs *regs;
	unsigned int instr;
	int r;

	/*
	 * Not optimizing the kprobe placed by
	 * kretprobe during boot time
	 */
	if ((kprobe_opcode_t)p->addr == (kprobe_opcode_t)&kretprobe_trampoline)
		return 0;

	regs = current_pt_regs();
	instr = *(p->ainsn.insn);

	/* Ensure the instruction can be emulated*/
	r = emulate_step(regs, instr);
	if (r != 1)
		return 0;

	return 1;
}

static void
create_return_branch(struct optimized_kprobe *op, struct pt_regs *regs)
{
	/*
	 * Create a branch back to the return address
	 * after the probed instruction is emulated
	 */

	kprobe_opcode_t branch, *buff;
	unsigned long ret;

	ret = regs->nip;
	buff = op->optinsn.insn;

	branch = create_branch((unsigned int *)buff + TMPL_RET_IDX,
			       (unsigned long)ret, 0);
	buff[TMPL_RET_IDX] = branch;
}

static void
optimized_callback(struct optimized_kprobe *op, struct pt_regs *regs)
{
	struct kprobe_ctlblk *kcb = get_kprobe_ctlblk();
	unsigned long flags;

	local_irq_save(flags);

	if (kprobe_running())
		kprobes_inc_nmissed_count(&op->kp);
	else {
		__this_cpu_write(current_kprobe, &op->kp);
		kcb->kprobe_status = KPROBE_HIT_ACTIVE;
		opt_pre_handler(&op->kp, regs);
		__this_cpu_write(current_kprobe, NULL);
	}
	local_irq_restore(flags);
}
NOKPROBE_SYMBOL(optimized_callback);

void arch_remove_optimized_kprobe(struct optimized_kprobe *op)
{
	 __arch_remove_optimized_kprobe(op, 1);
}

void  create_insn(unsigned int insn, kprobe_opcode_t *addr)
{
	u32 instr, instr2;

	/*
	 * emulate_step() requires insn to be emulated as
	 * second parameter. Hence r4 should be loaded
	 * with 'insn'.
	 * synthesize addis r4,0,(insn)@h
	 */
	instr = 0x3c000000 | 0x800000 | ((insn >> 16) & 0xffff);
	*addr++ = instr;

	/* ori r4,r4,(insn)@l */
	instr2 = 0x60000000 | 0x40000 | 0x800000;
	instr2 = instr2 | (insn & 0xffff);
	*addr = instr2;
}

void create_load_address_insn(struct optimized_kprobe *op,
			      kprobe_opcode_t *addr, kprobe_opcode_t *addr2)
{
	u32 instr1, instr2, instr3, instr4, instr5;
	unsigned long val;

	val = op;

	/*
	 * Optimized_kprobe structure is required as a parameter
	 * for invoking optimized_callback() and create_return_branch()
	 * from detour buffer. Hence need to have a 64bit immediate
	 * load into r3.
	 *
	 * lis r3,(op)@highest
	 */
	instr1 = 0x3c000000 | 0x600000 | ((val >> 48) & 0xffff);
	*addr++ = instr1;
	*addr2++ = instr1;

	/* ori r3,r3,(op)@higher */
	instr2 = 0x60000000 | 0x30000 | 0x600000 | ((val >> 32) & 0xffff);
	*addr++ = instr2;
	*addr2++ = instr2;

	/* rldicr r3,r3,32,31 */
	instr3 = 0x78000004 | 0x30000 | 0x600000 | ((32 & 0x1f) << 11);
	instr3 = instr3 | ((31 & 0x1f) << 6) | ((32 & 0x20) >> 4);
	*addr++ = instr3;
	*addr2++ = instr3;

	/* oris r3,r3,(op)@h */
	instr4 = 0x64000000 |  0x30000 | 0x600000 | ((val >> 16) & 0xffff);
	*addr++ = instr4;
	*addr2++ = instr4;

	/* ori r3,r3,(op)@l */
	instr5 =  0x60000000 | 0x30000 | 0x600000 | (val & 0xffff);
	*addr = instr5;
	*addr2 = instr5;
}

int arch_prepare_optimized_kprobe(struct optimized_kprobe *op, struct kprobe *p)
{
	kprobe_opcode_t *buff, branch, branch2, branch3;
	long rel_chk, ret_chk;

	kprobe_ppc_optinsn_slots.insn_size = MAX_OPTINSN_SIZE;
	op->optinsn.insn = NULL;

	if (!can_optimize(p))
		return -EILSEQ;

	/* Allocate instruction slot for detour buffer*/
	buff = ppc_get_optinsn_slot(op);
	if (!buff)
		return -ENOMEM;

	/*
	 * OPTPROBE use a 'b' instruction to branch to optinsn.insn.
	 *
	 * The target address has to be relatively nearby, to permit use
	 * of branch instruction in powerpc because the address is specified
	 * in an immediate field in the instruction opcode itself, ie 24 bits
	 * in the opcode specify the address. Therefore the address gap should
	 * be 32MB on either side of the current instruction.
	 */
	rel_chk = (long)buff -
			(unsigned long)p->addr;
	if (rel_chk < -0x2000000 || rel_chk > 0x1fffffc || rel_chk & 0x3) {
		ppc_free_optinsn_slot(op);
		return -ERANGE;
	}
	/*
	 * For the time being assume that the return address is NIP+4.
	 * TODO : check the return address is also within 32MB range for
	 * cases where NIP is not NIP+4.ie the NIP is decided after emulation.
	 */
	ret_chk = (long)(buff + TMPL_RET_IDX) - (unsigned long)(p->addr) + 4;
	if (ret_chk < -0x2000000 || ret_chk > 0x1fffffc || ret_chk & 0x3) {
		ppc_free_optinsn_slot(op);
		return -ERANGE;
	}

	/* Do Copy arch specific instance from template*/
	memcpy(buff, optprobe_template_entry,
	       TMPL_END_IDX * sizeof(kprobe_opcode_t));
	create_load_address_insn(op, buff + TMPL_OP1_IDX, buff + TMPL_OP2_IDX);

	/* Create a branch to the optimized_callback function */
	branch = create_branch((unsigned int *)buff + TMPL_CALL_HDLR_IDX,
			       (unsigned long)optimized_callback + 8,
				BRANCH_SET_LINK);

	/* Place the branch instr into the trampoline */
	buff[TMPL_CALL_HDLR_IDX] = branch;
	create_insn(*(p->ainsn.insn), buff + TMPL_INSN_IDX);

	/*Create a branch instruction into the emulate_step*/
	branch3 = create_branch((unsigned int *)buff + TMPL_EMULATE_IDX,
				(unsigned long)emulate_step + 8,
				BRANCH_SET_LINK);
	buff[TMPL_EMULATE_IDX] = branch3;

	/* Create a branch for jumping back*/
	branch2 = create_branch((unsigned int *)buff + TMPL_RET_BRANCH_IDX,
				(unsigned long)create_return_branch + 8,
				BRANCH_SET_LINK);
	buff[TMPL_RET_BRANCH_IDX] = branch2;

	op->optinsn.insn = buff;
	return 0;
}

int arch_prepared_optinsn(struct arch_optimized_insn *optinsn)
{
	return optinsn->insn;
}

/*
 * Here,kprobe opt always replace one instruction (4 bytes
 * aligned and 4 bytes long). It is impossible to encounter another
 * kprobe in the address range. So always return 0.
 */
int arch_check_optimized_kprobe(struct optimized_kprobe *op)
{
	return 0;
}

void arch_optimize_kprobes(struct list_head *oplist)
{
	struct optimized_kprobe *op;
	struct optimized_kprobe *tmp;

	unsigned int branch;

	list_for_each_entry_safe(op, tmp, oplist, list) {
	/*
	* Backup instructions which will be replaced
	* by jump address
	*/
		memcpy(op->optinsn.copied_insn, op->kp.addr,
		       RELATIVEJUMP_SIZE);
		branch = create_branch((unsigned int *)op->kp.addr,
					(unsigned long)op->optinsn.insn, 0);
		*op->kp.addr = branch;
		list_del_init(&op->list);
	}
}

void arch_unoptimize_kprobe(struct optimized_kprobe *op)
{
	arch_arm_kprobe(&op->kp);
}

void arch_unoptimize_kprobes(struct list_head *oplist,
			     struct list_head *done_list)
{
	struct optimized_kprobe *op;
	struct optimized_kprobe *tmp;

	list_for_each_entry_safe(op, tmp, oplist, list) {
		arch_unoptimize_kprobe(op);
		list_move(&op->list, done_list);
	}
}

int arch_within_optimized_kprobe(struct optimized_kprobe *op,
				 unsigned long addr)
{
	return 0;
}
