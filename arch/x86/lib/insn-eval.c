/*
 * Utility functions for x86 operand and address decoding
 *
 * Copyright (C) Intel Corporation 2017
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

enum segment {
	SEG_CS = 0x23,
	SEG_SS = 0x36,
	SEG_DS = 0x3e,
	SEG_ES = 0x26,
	SEG_FS = 0x64,
	SEG_GS = 0x65
};

/**
 * resolve_seg_selector() - obtain segment selector
 * @regs:	Set of registers containing the segment selector
 * @insn:	Instruction structure with selector override prefixes
 * @regoff:	Operand offset, in pt_regs, of which the selector is needed
 * @default:	Resolve default segment selector (i.e., ignore overrides)
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
 * This function returns the segment selector to utilize as per the conditions
 * described above. Please note that this functin does not return the value
 * of the segment selector. The value of the segment selector needs to be
 * obtained using get_segment_selector and passing the segment selector type
 * resolved by this function.
 *
 * Return: Segment selector to use, among CS, SS, DS, ES, FS or GS.
 */
static int resolve_seg_selector(struct insn *insn, int regoff, bool get_default)
{
	int i;

	if (!insn)
		return -EINVAL;

	if (get_default)
		goto default_seg;
	/*
	 * Check first if we have selector overrides. Having more than
	 * one selector override leads to undefined behavior. We
	 * only use the first one and return
	 */
	for (i = 0; i < insn->prefixes.nbytes; i++) {
		switch (insn->prefixes.bytes[i]) {
		case SEG_CS:
			return SEG_CS;
		case SEG_SS:
			return SEG_SS;
		case SEG_DS:
			return SEG_DS;
		case SEG_ES:
			return SEG_ES;
		case SEG_FS:
			return SEG_FS;
		case SEG_GS:
			return SEG_GS;
		default:
			return -EINVAL;
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
			return -EINVAL;
	case -EDOM: /* no register involved in address computation */
	case offsetof(struct pt_regs, bx):
		/* fall through */
	case offsetof(struct pt_regs, di):
		/* fall through */
	case offsetof(struct pt_regs, si):
		return SEG_DS;
	case offsetof(struct pt_regs, bp):
		/* fall through */
	case offsetof(struct pt_regs, sp):
		return SEG_SS;
	case offsetof(struct pt_regs, ip):
		return SEG_CS;
	default:
		return -EINVAL;
	}
}

/**
 * get_segment_selector() - obtain segment selector
 * @regs:	Set of registers containing the segment selector
 * @seg_type:	Type of segment selector to obtain
 * @regoff:	Operand offset, in pt_regs, of which the selector is needed
 *
 * Obtain the segment selector for any of CS, SS, DS, ES, FS, GS. In
 * CONFIG_X86_32, the segment is obtained from either pt_regs or
 * kernel_vm86_regs as applicable. In CONFIG_X86_64, CS and SS are obtained
 * from pt_regs. DS, ES, FS and GS are obtained by reading the ds and es, fs
 * and gs, respectively.
 *
 * Return: Value of the segment selector
 */
static unsigned short get_segment_selector(struct pt_regs *regs,
					   enum segment seg_type)
{
#ifdef CONFIG_X86_64
	unsigned short seg_sel;

	switch (seg_type) {
	case SEG_CS:
		return (unsigned short)(regs->cs & 0xffff);
	case SEG_SS:
		return (unsigned short)(regs->ss & 0xffff);
	case SEG_DS:
		savesegment(ds, seg_sel);
		return seg_sel;
	case SEG_ES:
		savesegment(es, seg_sel);
		return seg_sel;
	case SEG_FS:
		savesegment(fs, seg_sel);
		return seg_sel;
	case SEG_GS:
		savesegment(gs, seg_sel);
		return seg_sel;
	default:
		return -1;
	}
#else /* CONFIG_X86_32 */
	struct kernel_vm86_regs *vm86regs = (struct kernel_vm86_regs *)regs;

	if (v8086_mode(regs)) {
		switch (seg_type) {
		case SEG_CS:
			return (unsigned short)(regs->cs & 0xffff);
		case SEG_SS:
			return (unsigned short)(regs->ss & 0xffff);
		case SEG_DS:
			return vm86regs->ds;
		case SEG_ES:
			return vm86regs->es;
		case SEG_FS:
			return vm86regs->fs;
		case SEG_GS:
			return vm86regs->gs;
		default:
			return -1;
		}
	}

	switch (seg_type) {
	case SEG_CS:
		return (unsigned short)(regs->cs & 0xffff);
	case SEG_SS:
		return (unsigned short)(regs->ss & 0xffff);
	case SEG_DS:
		return (unsigned short)(regs->ds & 0xffff);
	case SEG_ES:
		return (unsigned short)(regs->es & 0xffff);
	case SEG_FS:
		return (unsigned short)(regs->fs & 0xffff);
	case SEG_GS:
		/*
		 * GS may or may not be in regs as per CONFIG_X86_32_LAZY_GS.
		 * The macro below takes care of both cases.
		 */
		return get_user_gs(regs);
	default:
		return -1;
	}
#endif /* CONFIG_X86_64 */
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
		/* if mod=0, register R/EBP is not used in the address
		 * computation. Instead, a 32-bit displacement is expected;
		 * the instruction decoder takes care of reading such
		 * displacement. This is true for both R/EBP and R13, as the
		 * REX.B bit is not decoded.
		 */
		if (regno == 5 && X86_MODRM_MOD(insn->modrm.value) == 0)
			return -EDOM;
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
 * get_reg_offset_16 - Obtain offset of register indicated by instruction
 * @insn:	Instruction structure containing ModRM and SiB bytes
 * @regs:	Set of registers referred by the instruction
 * @offs1:	Offset of the first operand register
 * @offs2:	Offset of the second opeand register, if applicable.
 *
 * Obtain the offset, in pt_regs, of the registers indicated by the ModRM byte
 * within insn. This function is to be used with 16-bit address encodings. The
 * offs1 and offs2 will be written with the offset of the two registers
 * indicated by the instruction. In cases where any of the registers is not
 * referenced by the instruction, the value will be set to -EDOM.
 *
 * Return: 0 on success, -EINVAL on failure.
 */
static int get_reg_offset_16(struct insn *insn, struct pt_regs *regs,
			     int *offs1, int *offs2)
{
	/* 16-bit addressing can use one or two registers */
	static const int regoff1[] = {
		offsetof(struct pt_regs, bx),
		offsetof(struct pt_regs, bx),
		offsetof(struct pt_regs, bp),
		offsetof(struct pt_regs, bp),
		offsetof(struct pt_regs, si),
		offsetof(struct pt_regs, di),
		offsetof(struct pt_regs, bp),
		offsetof(struct pt_regs, bx),
	};

	static const int regoff2[] = {
		offsetof(struct pt_regs, si),
		offsetof(struct pt_regs, di),
		offsetof(struct pt_regs, si),
		offsetof(struct pt_regs, di),
		-EDOM,
		-EDOM,
		-EDOM,
		-EDOM,
	};

	if (!offs1 || !offs2)
		return -EINVAL;

	/* operand is a register, use the generic function */
	if (X86_MODRM_MOD(insn->modrm.value) == 3) {
		*offs1 = insn_get_reg_offset_modrm_rm(insn, regs);
		*offs2 = -EDOM;
		return 0;
	}

	*offs1 = regoff1[X86_MODRM_RM(insn->modrm.value)];
	*offs2 = regoff2[X86_MODRM_RM(insn->modrm.value)];

	/*
	 * If no displacement is indicated in the mod part of the ModRM byte,
	 * (mod part is 0) and the r/m part of the same byte is 6, no register
	 * is used caculate the operand address. An r/m part of 6 means that
	 * the second register offset is already invalid.
	 */
	if ((X86_MODRM_MOD(insn->modrm.value) == 0) &&
	    (X86_MODRM_RM(insn->modrm.value) == 6))
		*offs1 = -EDOM;

	return 0;
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
 * insn_get_seg_base() - Obtain base address contained in descriptor
 * @regs:	Set of registers containing the segment selector
 * @insn:	Instruction structure with selector override prefixes
 * @regoff:	Operand offset, in pt_regs, of which the selector is needed
 * @use_default_seg: Use the default segment instead of prefix overrides
 *
 * Obtain the base address of the segment descriptor as indicated by either
 * any segment override prefixes contained in insn or the default segment
 * applicable to the register indicated by regoff. regoff is specified as the
 * offset in bytes from the base of pt_regs.
 *
 * Return: In protected mode, base address of the segment. It may be zero in
 * certain cases for 64-bit builds and/or 64-bit applications. In virtual-8086
 * mode, the segment selector shifed 4 positions to the right. -1L in case of
 * error.
 */
unsigned long insn_get_seg_base(struct pt_regs *regs, struct insn *insn,
				int regoff, bool use_default_seg)
{
	struct desc_struct *desc;
	unsigned short seg;
	enum segment seg_type;
	int ret;

	seg_type = resolve_seg_selector(insn, regoff, use_default_seg);

	seg = get_segment_selector(regs, seg_type);
	if (seg < 0)
		return -1L;

	if (v8086_mode(regs))
		/*
		 * Base is simply the segment selector shifted 4
		 * positions to the right.
		 */
		return (unsigned long)(seg << 4);

#ifdef CONFIG_X86_64
	if (user_64bit_mode(regs)) {
		/*
		 * Only FS or GS will have a base address, the rest of
		 * the segments' bases are forced to 0.
		 */
		unsigned long base;

		if (seg_type == SEG_FS)
			rdmsrl(MSR_FS_BASE, base);
		else if (seg_type == SEG_GS)
			/*
			 * swapgs was called at the kernel entry point. Thus,
			 * MSR_KERNEL_GS_BASE will have the user-space GS base.
			 */
			rdmsrl(MSR_KERNEL_GS_BASE, base);
		else
			base = 0;
		return base;
	}
#endif
	ret = get_desc(seg, &desc);
	if (ret)
		return -1L;

	return get_desc_base(desc);
}

/**
 * insn_get_seg_default_address_bytes - Obtain default address size of segment
 * @regs:	Set of registers containing the segment selector
 *
 * Obtain the default address size as indicated in the segment descriptor
 * selected in regs' code segment selector. In protected mode, the default
 * address is determined by inspecting the L and D bits of the segment
 * descriptor. In virtual-8086 mode, the default is always two bytes.
 *
 * Return: Default address size of segment
 */
unsigned char insn_get_seg_default_address_bytes(struct pt_regs *regs)
{
	struct desc_struct *desc;
	unsigned short seg;
	int ret;

	if (v8086_mode(regs))
		return 2;

	seg = (unsigned short)regs->cs;

	ret = get_desc(seg, &desc);
	if (ret)
		return 0;

	switch ((desc->l << 1) | desc->d) {
	case 0: /* Legacy mode. 16-bit addresses. CS.L=0, CS.D=0 */
		return 2;
	case 1: /* Legacy mode. 32-bit addresses. CS.L=0, CS.D=1 */
		return 4;
	case 2: /* IA-32e 64-bit mode. 64-bit addresses. CS.L=1, CS.D=0 */
		return 8;
	case 3: /* Invalid setting. CS.L=1, CS.D=1 */
		/* fall through */
	default:
		return 0;
	}
}

/**
 * insn_get_seg_default_operand_bytes - Obtain default operand size of segment
 * @regs:	Set of registers containing the segment selector
 *
 * Obtain the default operand size as indicated in the segment descriptor
 * selected in regs' code segment selector. In protected mode, the default
 * operand size is determined by inspecting the L and D bits of the segment
 * descriptor. In virtual-8086 mode, the default is always two bytes.
 *
 * Return: Default operand size of segment
 */
unsigned char insn_get_seg_default_operand_bytes(struct pt_regs *regs)
{
	struct desc_struct *desc;
	unsigned short seg;
	int ret;

	if (v8086_mode(regs))
		return 2;

	seg = (unsigned short)regs->cs;

	ret = get_desc(seg, &desc);
	if (ret)
		return 0;

	switch ((desc->l << 1) | desc->d) {
	case 0: /* Legacy mode. 16-bit or 8-bit operands CS.L=0, CS.D=0 */
		return 2;
	case 1: /* Legacy mode. 32- or 8 bit operands CS.L=0, CS.D=1 */
		/* fall through */
	case 2: /* IA-32e 64-bit mode. 32- or 8-bit opnds. CS.L=1, CS.D=0 */
		return 4;
	case 3: /* Invalid setting. CS.L=1, CS.D=1 */
		/* fall through */
	default:
		return 0;
	}
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

/**
 * insn_get_addr_ref_16 - Obtain the 16-bit address referred by instruction
 * @insn:	Instruction structure containing ModRM byte and displacement
 * @regs:	Set of registers referred by the instruction
 *
 * This function is to be used with 16-bit address encodings. Obtain the memory
 * address referred by the instruction's ModRM bytes and displacement. Also, the
 * segment used as base is determined by either any segment override prefixes in
 * insn or the default segment of the registers involved in the address
 * computation.
 * the ModRM byte
 *
 * Return: linear address referenced by instruction and registers
 */
static void __user *insn_get_addr_ref_16(struct insn *insn,
					 struct pt_regs *regs)
{
	unsigned long linear_addr, seg_base_addr;
	short eff_addr, addr1 = 0, addr2 = 0;
	int addr_offset1, addr_offset2;
	int ret;

	insn_get_modrm(insn);
	insn_get_displacement(insn);

	/*
	 * If operand is a register, the layout is the same as in
	 * 32-bit and 64-bit addressing.
	 */
	if (X86_MODRM_MOD(insn->modrm.value) == 3) {
		addr_offset1 = get_reg_offset(insn, regs, REG_TYPE_RM);
		if (addr_offset1 < 0)
			goto out_err;
		eff_addr = regs_get_register(regs, addr_offset1);
		seg_base_addr = insn_get_seg_base(regs, insn, addr_offset1,
						  false);
	} else {
		ret = get_reg_offset_16(insn, regs, &addr_offset1,
					&addr_offset2);
		if (ret < 0)
			goto out_err;
		/*
		 * Don't fail on invalid offset values. They might be invalid
		 * because they cannot be used for this particular value of
		 * the ModRM. Instead, use them in the computation only if
		 * they contain a valid value.
		 */
		if (addr_offset1 != -EDOM)
			addr1 = 0xffff & regs_get_register(regs, addr_offset1);
		if (addr_offset2 != -EDOM)
			addr2 = 0xffff & regs_get_register(regs, addr_offset2);
		eff_addr = addr1 + addr2;
		/*
		 * The first register is in the operand implies the SS or DS
		 * segment selectors, the second register in the operand can
		 * only imply DS. Thus, use the first register to obtain
		 * the segment selector.
		 */
		seg_base_addr = insn_get_seg_base(regs, insn, addr_offset1,
						  false);

		eff_addr += (insn->displacement.value & 0xffff);
	}
	linear_addr = (unsigned short)eff_addr + seg_base_addr;

	return (void __user *)linear_addr;
out_err:
	return (void __user *)-1;
}

static inline long __to_signed_long(unsigned long val, int long_bytes)
{
#ifdef CONFIG_X86_64
	return long_bytes == 4 ? (long)((int)((val) & 0xffffffff)) : (long)val;
#else
	return (long)val;
#endif
}

/**
 * insn_get_addr_ref_32_64 - Obtain a 32/64-bit address referred by instruction
 * @insn:	Instruction struct with ModRM and SiB bytes and displacement
 * @regs:	Set of registers referred by the instruction
 *
 * This function is to be used with 32-bit and 64-bit address encodings. Obtain
 * the memory address referred by the instruction's ModRM bytes and
 * displacement. Also, the segment used as base is determined by either any
 * segment override prefixes in insn or the default segment of the registers
 * involved in the linear address computation.
 *
 * Return: linear address referenced by instruction and registers
 */
static void __user *insn_get_addr_ref_32_64(struct insn *insn,
					    struct pt_regs *regs)
{
	unsigned long linear_addr, seg_base_addr;
	long eff_addr, base, indx, tmp;
	int addr_offset, base_offset, indx_offset, addr_bytes;
	insn_byte_t sib;

	insn_get_modrm(insn);
	insn_get_sib(insn);
	sib = insn->sib.value;
	addr_bytes = insn->addr_bytes;

	if (X86_MODRM_MOD(insn->modrm.value) == 3) {
		addr_offset = get_reg_offset(insn, regs, REG_TYPE_RM);
		if (addr_offset < 0)
			goto out_err;
		tmp = regs_get_register(regs, addr_offset);
		eff_addr = __to_signed_long(tmp, addr_bytes);
		seg_base_addr = insn_get_seg_base(regs, insn, addr_offset,
						  false);
	} else {
		if (insn->sib.nbytes) {
			/*
			 * Negative values in the base and index offset means
			 * an error when decoding the SIB byte. Except -EDOM,
			 * which means that the registers should not be used
			 * in the address computation.
			 */
			base_offset = get_reg_offset(insn, regs, REG_TYPE_BASE);
			if (unlikely(base_offset == -EDOM)) {
				base = 0;
			} else if (unlikely(base_offset < 0)) {
				goto out_err;
			} else {
				tmp = regs_get_register(regs, base_offset);
				base = __to_signed_long(tmp, addr_bytes);
			}

			indx_offset = get_reg_offset(insn, regs, REG_TYPE_INDEX);
			if (unlikely(indx_offset == -EDOM)) {
				indx = 0;
			} else if (unlikely(indx_offset < 0)) {
				goto out_err;
			} else {
				tmp = regs_get_register(regs, indx_offset);
				indx = __to_signed_long(tmp, addr_bytes);
			}

			eff_addr = base + indx * (1 << X86_SIB_SCALE(sib));
			seg_base_addr = insn_get_seg_base(regs, insn,
							  base_offset, false);
		} else {
			addr_offset = get_reg_offset(insn, regs, REG_TYPE_RM);
			/* -EDOM means that we must ignore the address_offset.
			 * The only case in which we see this value is when
			 * R/M points to R/EBP. In such a case, in 64-bit mode
			 * the effective address is relative to tho RIP.
			 */
			if (addr_offset == -EDOM) {
				eff_addr = 0;
#ifdef CONFIG_X86_64
				if (user_64bit_mode(regs))
					eff_addr = (long)regs->ip;
#endif
			} else if (addr_offset < 0) {
				goto out_err;
			} else {
				tmp = regs_get_register(regs, addr_offset);
				eff_addr = __to_signed_long(tmp, addr_bytes);
			}
			seg_base_addr = insn_get_seg_base(regs, insn,
							  addr_offset, false);
		}
		eff_addr += insn->displacement.value;
	}
	/* truncate to 4 bytes for 32-bit effective addresses */
	if (addr_bytes == 4)
		eff_addr &= 0xffffffff;

	linear_addr = (unsigned long)eff_addr + seg_base_addr;

	return (void __user *)linear_addr;
out_err:
	return (void __user *)-1;
}

/**
 * insn_get_addr_ref - Obtain the linear address referred by instruction
 * @insn:	Instruction structure containing ModRM byte and displacement
 * @regs:	Set of registers referred by the instruction
 *
 * Obtain the memory address referred by the instruction's ModRM bytes and
 * displacement. Also, the segment used as base is determined by either any
 * segment override prefixes in insn or the default segment of the registers
 * involved in the address computation.
 *
 * Return: linear address referenced by instruction and registers
 */
void __user *insn_get_addr_ref(struct insn *insn, struct pt_regs *regs)
{
	switch (insn->addr_bytes) {
	case 2:
		return insn_get_addr_ref_16(insn, regs);
	case 4:
		/* fall through */
	case 8:
		return insn_get_addr_ref_32_64(insn, regs);
	default:
		return (void __user *)-1;
	}
}
