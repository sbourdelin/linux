/*
 * Utility functions for x86 operand and address decoding
 *
 * Copyright (C) Intel Corporation 2017
 */
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/ratelimit.h>
#include <linux/mmu_context.h>
#include <asm/desc_defs.h>
#include <asm/desc.h>
#include <asm/inat.h>
#include <asm/insn.h>
#include <asm/insn-eval.h>
#include <asm/ldt.h>
#include <asm/vm86.h>

enum reg_type {
	REG_TYPE_RM = 0,
	REG_TYPE_INDEX,
	REG_TYPE_BASE,
};

enum string_instruction {
	INSB		= 0x6c,
	INSW_INSD	= 0x6d,
	OUTSB		= 0x6e,
	OUTSW_OUTSD	= 0x6f,
	MOVSB		= 0xa4,
	MOVSW_MOVSD	= 0xa5,
	CMPSB		= 0xa6,
	CMPSW_CMPSD	= 0xa7,
	STOSB		= 0xaa,
	STOSW_STOSD	= 0xab,
	LODSB		= 0xac,
	LODSW_LODSD	= 0xad,
	SCASB		= 0xae,
	SCASW_SCASD	= 0xaf,
};

enum segment_register {
	SEG_REG_INVAL = -1,
	SEG_REG_IGNORE = 0,
	SEG_REG_CS = 0x23,
	SEG_REG_SS = 0x36,
	SEG_REG_DS = 0x3e,
	SEG_REG_ES = 0x26,
	SEG_REG_FS = 0x64,
	SEG_REG_GS = 0x65,
};

/**
 * is_string_instruction - Determine if instruction is a string instruction
 * @insn:	Instruction structure containing the opcode
 *
 * Return: true if the instruction, determined by the opcode, is any of the
 * string instructions as defined in the Intel Software Development manual.
 * False otherwise.
 */
static bool is_string_instruction(struct insn *insn)
{
	insn_get_opcode(insn);

	/* all string instructions have a 1-byte opcode */
	if (insn->opcode.nbytes != 1)
		return false;

	switch (insn->opcode.bytes[0]) {
	case INSB:
		/* fall through */
	case INSW_INSD:
		/* fall through */
	case OUTSB:
		/* fall through */
	case OUTSW_OUTSD:
		/* fall through */
	case MOVSB:
		/* fall through */
	case MOVSW_MOVSD:
		/* fall through */
	case CMPSB:
		/* fall through */
	case CMPSW_CMPSD:
		/* fall through */
	case STOSB:
		/* fall through */
	case STOSW_STOSD:
		/* fall through */
	case LODSB:
		/* fall through */
	case LODSW_LODSD:
		/* fall through */
	case SCASB:
		/* fall through */
	case SCASW_SCASD:
		return true;
	default:
		return false;
	}
}

/**
 * resolve_seg_register() - obtain segment register
 * @insn:	Instruction structure with segment override prefixes
 * @regs:	Structure with register values as seen when entering kernel mode
 * @regoff:	Operand offset, in pt_regs, used to deterimine segment register
 *
 * The segment register to which an effective address refers depends on
 * a) whether segment override prefixes must be ignored: always use CS when
 * the register is (R|E)IP; always use ES when operand register is (E)DI with
 * string instructions as defined in the Intel documentation. b) If segment
 * overrides prefixes are used in the instruction instruction prefixes. C) Use
 * the default segment register associated with the operand register.
 *
 * The operand register, regoff, is represented as the offset from the base of
 * pt_regs. Also, regoff can be -EDOM for cases in which registers are not
 * used as operands (e.g., displacement-only memory addressing).
 *
 * This function returns the segment register as value from an enumeration
 * as per the conditions described above. Please note that this function
 * does not return the value in the segment register (i.e., the segment
 * selector). The segment selector needs to be obtained using
 * get_segment_selector() and passing the segment register resolved by
 * this function.
 *
 * Return: Enumerated segment register to use, among CS, SS, DS, ES, FS, GS,
 * ignore (in 64-bit mode as applicable), or -EINVAL in case of error.
 */
static enum segment_register resolve_seg_register(struct insn *insn,
						  struct pt_regs *regs,
						  int regoff)
{
	int i;
	int sel_overrides = 0;
	int seg_register = SEG_REG_IGNORE;

	if (!insn)
		return SEG_REG_INVAL;

	/* First handle cases when segment override prefixes must be ignored */
	if (regoff == offsetof(struct pt_regs, ip)) {
		if (user_64bit_mode(regs))
			return SEG_REG_IGNORE;
		else
			return SEG_REG_CS;
		return SEG_REG_CS;
	}

	/*
	 * If the (E)DI register is used with string instructions, the ES
	 * segment register is always used.
	 */
	if ((regoff == offsetof(struct pt_regs, di)) &&
	    is_string_instruction(insn)) {
		if (user_64bit_mode(regs))
			return SEG_REG_IGNORE;
		else
			return SEG_REG_ES;
		return SEG_REG_CS;
	}

	/* Then check if we have segment overrides prefixes*/
	for (i = 0; i < insn->prefixes.nbytes; i++) {
		switch (insn->prefixes.bytes[i]) {
		case SEG_REG_CS:
			seg_register = SEG_REG_CS;
			sel_overrides++;
			break;
		case SEG_REG_SS:
			seg_register = SEG_REG_SS;
			sel_overrides++;
			break;
		case SEG_REG_DS:
			seg_register = SEG_REG_DS;
			sel_overrides++;
			break;
		case SEG_REG_ES:
			seg_register = SEG_REG_ES;
			sel_overrides++;
			break;
		case SEG_REG_FS:
			seg_register = SEG_REG_FS;
			sel_overrides++;
			break;
		case SEG_REG_GS:
			seg_register = SEG_REG_GS;
			sel_overrides++;
			break;
		default:
			return SEG_REG_INVAL;
		}
	}

	/*
	 * Having more than one segment override prefix leads to undefined
	 * behavior. If this is the case, return with error.
	 */
	if (sel_overrides > 1)
		return SEG_REG_INVAL;

	if (sel_overrides == 1) {
		/*
		 * If in long mode all segment registers but FS and GS are
		 * ignored.
		 */
		if (user_64bit_mode(regs) && !(seg_register == SEG_REG_FS ||
					       seg_register == SEG_REG_GS))
			return SEG_REG_IGNORE;

		return seg_register;
	}

	/* In long mode, all segment registers except FS and GS are ignored */
	if (user_64bit_mode(regs))
		return SEG_REG_IGNORE;

	/*
	 * Lastly, if no segment overrides were found, determine the default
	 * segment register as described in the Intel documentation: SS for
	 * (E)SP or (E)BP. DS for all data references, AX, CX and DX are not
	 * valid register operands in 16-bit address encodings.
	 * -EDOM is reserved to identify for cases in which no register is used
	 * the default segment register (displacement-only addressing). The
	 * default segment register used in these cases is DS.
	 */

	switch (regoff) {
	case offsetof(struct pt_regs, ax):
		/* fall through */
	case offsetof(struct pt_regs, cx):
		/* fall through */
	case offsetof(struct pt_regs, dx):
		if (insn && insn->addr_bytes == 2)
			return SEG_REG_INVAL;
	case offsetof(struct pt_regs, di):
		/* fall through */
	case -EDOM:
		/* fall through */
	case offsetof(struct pt_regs, bx):
		/* fall through */
	case offsetof(struct pt_regs, si):
		return SEG_REG_DS;
	case offsetof(struct pt_regs, bp):
		/* fall through */
	case offsetof(struct pt_regs, sp):
		return SEG_REG_SS;
	case offsetof(struct pt_regs, ip):
		return SEG_REG_CS;
	default:
		return SEG_REG_INVAL;
	}
}

/**
 * get_segment_selector() - obtain segment selector
 * @regs:	Structure with register values as seen when entering kernel mode
 * @seg_reg:	Segment register to use
 *
 * Obtain the segment selector from any of the CS, SS, DS, ES, FS, GS segment
 * registers. In CONFIG_X86_32, the segment is obtained from either pt_regs or
 * kernel_vm86_regs as applicable. In CONFIG_X86_64, CS and SS are obtained
 * from pt_regs. DS, ES, FS and GS are obtained by reading the actual CPU
 * registers. This done for only for completeness as in CONFIG_X86_64 segment
 * registers are ignored.
 *
 * Return: Value of the segment selector, including null when running in
 * long mode. -1 on error.
 */
static unsigned short get_segment_selector(struct pt_regs *regs,
					   enum segment_register seg_reg)
{
#ifdef CONFIG_X86_64
	unsigned short sel;

	switch (seg_reg) {
	case SEG_REG_IGNORE:
		return 0;
	case SEG_REG_CS:
		return (unsigned short)(regs->cs & 0xffff);
	case SEG_REG_SS:
		return (unsigned short)(regs->ss & 0xffff);
	case SEG_REG_DS:
		savesegment(ds, sel);
		return sel;
	case SEG_REG_ES:
		savesegment(es, sel);
		return sel;
	case SEG_REG_FS:
		savesegment(fs, sel);
		return sel;
	case SEG_REG_GS:
		savesegment(gs, sel);
		return sel;
	default:
		return -1;
	}
#else /* CONFIG_X86_32 */
	struct kernel_vm86_regs *vm86regs = (struct kernel_vm86_regs *)regs;

	if (v8086_mode(regs)) {
		switch (seg_reg) {
		case SEG_REG_CS:
			return (unsigned short)(regs->cs & 0xffff);
		case SEG_REG_SS:
			return (unsigned short)(regs->ss & 0xffff);
		case SEG_REG_DS:
			return vm86regs->ds;
		case SEG_REG_ES:
			return vm86regs->es;
		case SEG_REG_FS:
			return vm86regs->fs;
		case SEG_REG_GS:
			return vm86regs->gs;
		case SEG_REG_IGNORE:
			/* fall through */
		default:
			return -1;
		}
	}

	switch (seg_reg) {
	case SEG_REG_CS:
		return (unsigned short)(regs->cs & 0xffff);
	case SEG_REG_SS:
		return (unsigned short)(regs->ss & 0xffff);
	case SEG_REG_DS:
		return (unsigned short)(regs->ds & 0xffff);
	case SEG_REG_ES:
		return (unsigned short)(regs->es & 0xffff);
	case SEG_REG_FS:
		return (unsigned short)(regs->fs & 0xffff);
	case SEG_REG_GS:
		/*
		 * GS may or may not be in regs as per CONFIG_X86_32_LAZY_GS.
		 * The macro below takes care of both cases.
		 */
		return get_user_gs(regs);
	case SEG_REG_IGNORE:
		/* fall through */
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
		if (X86_REX_B(insn->rex_prefix.value))
			regno += 8;
		break;

	case REG_TYPE_INDEX:
		regno = X86_SIB_INDEX(insn->sib.value);
		if (X86_REX_X(insn->rex_prefix.value))
			regno += 8;
		/*
		 * If ModRM.mod !=3 and SIB.index (regno=4) the scale*index
		 * portion of the address computation is null. This is
		 * true only if REX.X is 0. In such a case, the SIB index
		 * is used in the address computation.
		 */
		if (X86_MODRM_MOD(insn->modrm.value) != 3 && regno == 4)
			return -EDOM;
		break;

	case REG_TYPE_BASE:
		regno = X86_SIB_BASE(insn->sib.value);
		/*
		 * If ModRM.mod is 0 and SIB.base == 5, the base of the
		 * register-indirect addressing is 0. In this case, a
		 * 32-bit displacement is expected in this case; the
		 * instruction decoder finds such displacement for us.
		 */
		if (!X86_MODRM_MOD(insn->modrm.value) && regno == 5)
			return -EDOM;

		if (X86_REX_B(insn->rex_prefix.value))
			regno += 8;
		break;

	default:
		printk_ratelimited(KERN_ERR "insn-eval: x86: invalid register type");
		return -EINVAL;
	}

	if (regno >= nr_registers) {
		WARN_ONCE(1, "decoded an instruction with an invalid register");
		return -EINVAL;
	}
	return regoff[regno];
}

/**
 * get_desc() - Obtain address of segment descriptor
 * @sel:	Segment selector
 *
 * Given a segment selector, obtain a pointer to the segment descriptor.
 * Both global and local descriptor tables are supported.
 *
 * Return: pointer to segment descriptor on success. NULL on failure.
 */
static struct desc_struct *get_desc(unsigned short sel)
{
	struct desc_ptr gdt_desc = {0, 0};
	struct desc_struct *desc = NULL;
	unsigned long desc_base;

#ifdef CONFIG_MODIFY_LDT_SYSCALL
	if ((sel & SEGMENT_TI_MASK) == SEGMENT_LDT) {
		/* Bits [15:3] contain the index of the desired entry. */
		sel >>= 3;

		mutex_lock(&current->active_mm->context.lock);
		/* The size of the LDT refers to the number of entries. */
		if (!current->active_mm->context.ldt ||
		    sel >= current->active_mm->context.ldt->size) {
			mutex_unlock(&current->active_mm->context.lock);
			return NULL;
		}

		desc = &current->active_mm->context.ldt->entries[sel];
		mutex_unlock(&current->active_mm->context.lock);
		return desc;
	}
#endif
	native_store_gdt(&gdt_desc);

	/*
	 * Segment descriptors have a size of 8 bytes. Thus, the index is
	 * multiplied by 8 to obtain the offset of the desired descriptor from
	 * the start of the GDT. As bits [15:3] of the segment selector contain
	 * the index, it can be regarded multiplied by 8 already. All that
	 * remains is to clear bits [2:0].
	 */
	desc_base = sel & ~(SEGMENT_RPL_MASK | SEGMENT_TI_MASK);

	if (desc_base > gdt_desc.size)
		return NULL;

	desc = (struct desc_struct *)(gdt_desc.address + desc_base);
	return desc;
}

/**
 * insn_get_reg_offset_modrm_rm() - Obtain register in r/m part of ModRM byte
 * @insn:	Instruction structure containing the ModRM byte
 * @regs:	Structure with register values as seen when entering kernel mode
 *
 * Return: The register indicated by the r/m part of the ModRM byte. The
 * register is obtained as an offset from the base of pt_regs. In specific
 * cases, the returned value can be -EDOM to indicate that the particular value
 * of ModRM does not refer to a register and shall be ignored.
 */
int insn_get_modrm_rm_off(struct insn *insn, struct pt_regs *regs)
{
	return get_reg_offset(insn, regs, REG_TYPE_RM);
}

/*
 * return the address being referenced be instruction
 * for rm=3 returning the content of the rm reg
 * for rm!=3 calculates the address using SIB and Disp
 */
void __user *insn_get_addr_ref(struct insn *insn, struct pt_regs *regs)
{
	unsigned long linear_addr;
	long eff_addr, base, indx;
	int addr_offset, base_offset, indx_offset;
	insn_byte_t sib;

	insn_get_modrm(insn);
	insn_get_sib(insn);
	sib = insn->sib.value;

	if (X86_MODRM_MOD(insn->modrm.value) == 3) {
		addr_offset = get_reg_offset(insn, regs, REG_TYPE_RM);
		if (addr_offset < 0)
			goto out_err;
		eff_addr = regs_get_register(regs, addr_offset);
	} else {
		if (insn->sib.nbytes) {
			/*
			 * Negative values in the base and index offset means
			 * an error when decoding the SIB byte. Except -EDOM,
			 * which means that the registers should not be used
			 * in the address computation.
			 */
			base_offset = get_reg_offset(insn, regs, REG_TYPE_BASE);
			if (base_offset == -EDOM)
				base = 0;
			else if (base_offset < 0)
				goto out_err;
			else
				base = regs_get_register(regs, base_offset);

			indx_offset = get_reg_offset(insn, regs, REG_TYPE_INDEX);
			if (indx_offset == -EDOM)
				indx = 0;
			else if (indx_offset < 0)
				goto out_err;
			else
				indx = regs_get_register(regs, indx_offset);

			eff_addr = base + indx * (1 << X86_SIB_SCALE(sib));
		} else {
			addr_offset = get_reg_offset(insn, regs, REG_TYPE_RM);
			if (addr_offset < 0)
				goto out_err;
			eff_addr = regs_get_register(regs, addr_offset);
		}
		eff_addr += insn->displacement.value;
	}
	linear_addr = (unsigned long)eff_addr;

	return (void __user *)linear_addr;
out_err:
	return (void __user *)-1;
}
