/*
 * Utility functions for x86 operand and address decoding
 *
 * Copyright (C) Intel Corporation 2016
 */
#include <linux/kernel.h>
#include <linux/string.h>
#include <asm/desc_defs.h>
#include <asm/desc.h>
#include <asm/inat.h>
#include <asm/insn.h>
#include <asm/insn-eval.h>
#include <asm/ldt.h>
#include <linux/mmu_context.h>
#include <asm/vm86.h>

enum reg_type {
	REG_TYPE_RM = 0,
	REG_TYPE_INDEX,
	REG_TYPE_BASE,
};

/**
 * get_segment_selector() - obtain segment selector
 * @regs:	Set of registers containing the segment selector
 * @insn:	Instruction structure with selector override prefixes
 * @regoff:	Operand offset, in pt_regs, of which the selector is needed
 *
 * The segment selector to which an effective address refers depends on
 * a) segment selector overrides instruction prefixes or b) the operand
 * register indicated in the ModRM or SiB byte.
 *
 * For case a), the function inspects any prefixes in the insn instruction;
 * insn can be null to indicate that selector override prefixes shall be
 * ignored. This is useful when the use of prefixes is forbidden (e.g.,
 * obtaining the code selector). For case b), the operand register shall be
 * represented as the offset from the base address of pt_regs. Also, regoff
 * can be -EINVAL for cases in which registers are not used as operands (e.g.,
 * when the mod and r/m parts of the ModRM byte are 0 and 5, respectively).
 *
 * The returned segment selector is obtained from the regs structure. Both
 * protected and virtual-8086 modes are supported. In virtual-8086 mode,
 * data segments are obtained from the kernel_vm86_regs structure.
 * For CONFIG_X86_64, the returned segment selector is null if such selector
 * refers to es, fs or gs.
 *
 * Return: Value of the segment selector
 */
static unsigned short get_segment_selector(struct pt_regs *regs,
					   struct insn *insn, int regoff)
{
	int i;

	struct kernel_vm86_regs *vm86regs = (struct kernel_vm86_regs *)regs;

	if (!insn)
		goto default_seg;

	insn_get_prefixes(insn);

	if (v8086_mode(regs)) {
		/*
		 * Check first if we have selector overrides. Having more than
		 * one selector override leads to undefined behavior. We
		 * only use the first one and return
		 */
		for (i = 0; i < insn->prefixes.nbytes; i++) {
			switch (insn->prefixes.bytes[i]) {
			/*
			 * Code and stack segment selector register are saved in
			 * all processor modes. Thus, it makes sense to take
			 * them from pt_regs.
			 */
			case 0x2e:
				return (unsigned short)regs->cs;
			case 0x36:
				return (unsigned short)regs->ss;
			/*
			 * The rest of the segment selector registers are only
			 * saved in virtual-8086 mode. Thus, we must obtain them
			 * from the vm86 register structure.
			 */
			case 0x3e:
				return vm86regs->ds;
			case 0x26:
				return vm86regs->es;
			case 0x64:
				return vm86regs->fs;
			case 0x65:
				return vm86regs->gs;
			/*
			 * No default action needed. We simply did not find any
			 * relevant prefixes.
			 */
			}
		}
	} else {/* protected mode */
		/*
		 * Check first if we have selector overrides. Having more than
		 * one selector override leads to undefined behavior. We
		 * only use the first one and return.
		 */
		for (i = 0; i < insn->prefixes.nbytes; i++) {
			switch (insn->prefixes.bytes[i]) {
			/*
			 * Code and stack segment selector register are saved in
			 * all processor modes. Thus, it makes sense to take
			 * them from pt_regs.
			 */
			case 0x2e:
				return (unsigned short)regs->cs;
			case 0x36:
				return (unsigned short)regs->ss;
#ifdef CONFIG_X86_32
			case 0x3e:
				return (unsigned short)regs->ds;
			case 0x26:
				return (unsigned short)regs->es;
			case 0x64:
				return (unsigned short)regs->fs;
			case 0x65:
				return (unsigned short)regs->gs;
#else
			/* do not return any segment selector in x86_64 */
			case 0x3e:
			case 0x26:
			case 0x64:
			case 0x65:
				return 0;
#endif
			/*
			 * No default action needed. We simply did not find any
			 * relevant prefixes.
			 */
			}
		}
	}

default_seg:
	/*
	 * If no overrides, use default selectors as described in the
	 * Intel documentation: SS for ESP or EBP. DS for all data references,
	 * except when relative to stack or string destination.
	 * Also, AX, CX and DX are not valid register operands in 16-bit
	 * address encodings.
	 * Callers must interpret the result correctly according to the type
	 * of instructions (e.g., use ES for string instructions).
	 * Also, some values of modrm and sib might seem to indicate the use
	 * of EBP and ESP (e.g., modrm_mod = 0, modrm_rm = 5) but actually
	 * they refer to cases in which only a displacement used. These cases
	 * should be indentified by the caller and not with this function.
	 */
	switch (regoff) {
	case offsetof(struct pt_regs, ax):
		/* fall through */
	case offsetof(struct pt_regs, cx):
		/* fall through */
	case offsetof(struct pt_regs, dx):
		if (insn && insn->addr_bytes == 2)
			return 0;
	case -EDOM: /* no register involved in address computation */
	case offsetof(struct pt_regs, bx):
		/* fall through */
	case offsetof(struct pt_regs, di):
		/* fall through */
	case offsetof(struct pt_regs, si):
		if (v8086_mode(regs))
			return vm86regs->ds;
#ifdef CONFIG_X86_32
		return (unsigned short)regs->ds;
#else
		return 0;
#endif
	case offsetof(struct pt_regs, bp):
		/* fall through */
	case offsetof(struct pt_regs, sp):
		return (unsigned short)regs->ss;
	case offsetof(struct pt_regs, ip):
		return (unsigned short)regs->cs;
	default:
		return 0;
	}
}

static int get_reg_offset(struct insn *insn, struct pt_regs *regs,
			  enum reg_type type)
{
	int regno = 0;

	static const int regoff[] = {
		offsetof(struct pt_regs, ax),
		offsetof(struct pt_regs, cx),
		offsetof(struct pt_regs, dx),
		offsetof(struct pt_regs, bx),
		offsetof(struct pt_regs, sp),
		offsetof(struct pt_regs, bp),
		offsetof(struct pt_regs, si),
		offsetof(struct pt_regs, di),
#ifdef CONFIG_X86_64
		offsetof(struct pt_regs, r8),
		offsetof(struct pt_regs, r9),
		offsetof(struct pt_regs, r10),
		offsetof(struct pt_regs, r11),
		offsetof(struct pt_regs, r12),
		offsetof(struct pt_regs, r13),
		offsetof(struct pt_regs, r14),
		offsetof(struct pt_regs, r15),
#endif
	};
	int nr_registers = ARRAY_SIZE(regoff);
	/*
	 * Don't possibly decode a 32-bit instructions as
	 * reading a 64-bit-only register.
	 */
	if (IS_ENABLED(CONFIG_X86_64) && !insn->x86_64)
		nr_registers -= 8;

	switch (type) {
	case REG_TYPE_RM:
		regno = X86_MODRM_RM(insn->modrm.value);
		if (X86_REX_B(insn->rex_prefix.value))
			regno += 8;
		break;

	case REG_TYPE_INDEX:
		regno = X86_SIB_INDEX(insn->sib.value);
		if (X86_REX_X(insn->rex_prefix.value))
			regno += 8;
		/*
		 * If mod !=3, register R/ESP (regno=4) is not used as index in
		 * the address computation. Check is done after looking at REX.X
		 * This is because R12 (regno=12) can be used as an index.
		 */
		if (regno == 4 && X86_MODRM_MOD(insn->modrm.value) != 3)
			return -EDOM;
		break;

	case REG_TYPE_BASE:
		regno = X86_SIB_BASE(insn->sib.value);
		/*
		 * If mod is 0 and register R/EBP (regno=5) is indicated in the
		 * base part of the SIB byte, the value of such register should
		 * not be used in the address computation. Also, a 32-bit
		 * displacement is expected in this case; the instruction
		 * decoder takes care of it. This is true for both R13 and
		 * R/EBP as REX.B will not be decoded.
		 */
		if (regno == 5 && X86_MODRM_MOD(insn->modrm.value) == 0)
			return -EDOM;

		if (X86_REX_B(insn->rex_prefix.value))
			regno += 8;
		break;

	default:
		pr_err("invalid register type");
		BUG();
		break;
	}

	if (regno >= nr_registers) {
		WARN_ONCE(1, "decoded an instruction with an invalid register");
		return -EINVAL;
	}
	return regoff[regno];
}

/**
 * get_desc() - Obtain address of segment descriptor
 * @seg:	Segment selector
 * @desc:	Pointer to the selected segment descriptor
 *
 * Given a segment selector, obtain a memory pointer to the segment
 * descriptor. Both global and local descriptor tables are supported.
 * desc will contain the address of the descriptor.
 *
 * Return: 0 if success, -EINVAL if failure
 */
static int get_desc(unsigned short seg, struct desc_struct **desc)
{
	struct desc_ptr gdt_desc = {0, 0};
	unsigned long desc_base;

	if (!desc)
		return -EINVAL;

	desc_base = seg & ~(SEGMENT_RPL_MASK | SEGMENT_TI_MASK);

#ifdef CONFIG_MODIFY_LDT_SYSCALL
	if ((seg & SEGMENT_TI_MASK) == SEGMENT_LDT) {
		seg >>= 3;

		mutex_lock(&current->active_mm->context.lock);
		if (unlikely(!current->active_mm->context.ldt ||
			     seg >= current->active_mm->context.ldt->size)) {
			*desc = NULL;
			mutex_unlock(&current->active_mm->context.lock);
			return -EINVAL;
		}

		*desc = &current->active_mm->context.ldt->entries[seg];
		mutex_unlock(&current->active_mm->context.lock);
		return 0;
	}
#endif
	native_store_gdt(&gdt_desc);

	/*
	 * Bits [15:3] of the segment selector contain the index. Such
	 * index needs to be multiplied by 8. However, as the index
	 * least significant bit is already in bit 3, we don't have
	 * to perform the multiplication.
	 */
	desc_base = seg & ~(SEGMENT_RPL_MASK | SEGMENT_TI_MASK);

	if (desc_base > gdt_desc.size) {
		*desc = NULL;
		return -EINVAL;
	}

	*desc = (struct desc_struct *)(gdt_desc.address + desc_base);
	return 0;
}

/**
 * insn_get_reg_offset_modrm_rm - Obtain register in r/m part of ModRM byte
 * @insn:	Instruction structure containing the ModRM byte
 * @regs:	Set of registers indicated by the ModRM byte
 *
 * Obtain the register indicated by the r/m part of the ModRM byte. The
 * register is obtained as an offset from the base of pt_regs. In specific
 * cases, the returned value can be -EDOM to indicate that the particular value
 * of ModRM does not refer to a register.
 *
 * Return: Register indicated by r/m, as an offset within struct pt_regs
 */
int insn_get_reg_offset_modrm_rm(struct insn *insn, struct pt_regs *regs)
{
	return get_reg_offset(insn, regs, REG_TYPE_RM);
}

/**
 * insn_get_reg_offset_sib_base - Obtain register in base part of SiB byte
 * @insn:	Instruction structure containing the SiB byte
 * @regs:	Set of registers indicated by the SiB byte
 *
 * Obtain the register indicated by the base part of the SiB byte. The
 * register is obtained as an offset from the base of pt_regs. In specific
 * cases, the returned value can be -EDOM to indicate that the particular value
 * of SiB does not refer to a register.
 *
 * Return: Register indicated by SiB's base, as an offset within struct pt_regs
 */
int insn_get_reg_offset_sib_base(struct insn *insn, struct pt_regs *regs)
{
	return get_reg_offset(insn, regs, REG_TYPE_BASE);
}

/**
 * insn_get_reg_offset_sib_index - Obtain register in index part of SiB byte
 * @insn:	Instruction structure containing the SiB byte
 * @regs:	Set of registers indicated by the SiB byte
 *
 * Obtain the register indicated by the index part of the SiB byte. The
 * register is obtained as an offset from the index of pt_regs. In specific
 * cases, the returned value can be -EDOM to indicate that the particular value
 * of SiB does not refer to a register.
 *
 * Return: Register indicated by SiB's base, as an offset within struct pt_regs
 */
int insn_get_reg_offset_sib_index(struct insn *insn, struct pt_regs *regs)
{
	return get_reg_offset(insn, regs, REG_TYPE_INDEX);
}

/*
 * return the address being referenced be instruction
 * for rm=3 returning the content of the rm reg
 * for rm!=3 calculates the address using SIB and Disp
 */
static void __user *insn_get_addr_ref(struct insn *insn, struct pt_regs *regs)
{
	unsigned long addr, base, indx;
	int addr_offset, base_offset, indx_offset;
	insn_byte_t sib;

	insn_get_modrm(insn);
	insn_get_sib(insn);
	sib = insn->sib.value;

	if (X86_MODRM_MOD(insn->modrm.value) == 3) {
		addr_offset = get_reg_offset(insn, regs, REG_TYPE_RM);
		if (addr_offset < 0)
			goto out_err;
		addr = regs_get_register(regs, addr_offset);
	} else {
		if (insn->sib.nbytes) {
			/*
			 * Negative values in the base and index offset means
			 * an error when decoding the SIB byte. Except -EDOM,
			 * which means that the registers should not be used
			 * in the address computation.
			 */

			base_offset = get_reg_offset(insn, regs, REG_TYPE_BASE);
			if (base_offset < 0)
				if (base_offset == -EDOM)
					base = 0;
				else
					goto out_err;
			else
				base = regs_get_register(regs, base_offset);

			indx_offset = get_reg_offset(insn, regs, REG_TYPE_INDEX);
			if (indx_offset < 0)
				if (indx_offset == -EDOM)
					indx = 0;
				else
					goto out_err;
			else
				indx = regs_get_register(regs, indx_offset);

			addr = base + indx * (1 << X86_SIB_SCALE(sib));
		} else {
			addr_offset = get_reg_offset(insn, regs, REG_TYPE_RM);
			if (addr_offset < 0)
				goto out_err;
			addr = regs_get_register(regs, addr_offset);
		}
		addr += insn->displacement.value;
	}
	return (void __user *)addr;
out_err:
	return (void __user *)-1;
}
