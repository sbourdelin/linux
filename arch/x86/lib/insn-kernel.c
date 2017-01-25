/*
 * Utility functions for x86 operand and address decoding
 *
 * Copyright (C) Intel Corporation 2016
 */
#include <linux/kernel.h>
#include <linux/string.h>
#include <asm/inat.h>
#include <asm/insn.h>
#include <asm/insn-kernel.h>
#include <asm/vm86.h>

enum reg_type {
	REG_TYPE_RM = 0,
	REG_TYPE_INDEX,
	REG_TYPE_BASE,
};

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
			return -EINVAL;
		break;

	case REG_TYPE_BASE:
		regno = X86_SIB_BASE(insn->sib.value);
		/*
		 * If R/EBP (regno = 5) is indicated in the base part of the SIB
		 * byte, an explicit displacement must be specified. In other
		 * words, the mod part of the ModRM byte cannot be zero.
		 */
		if (regno == 5 && X86_MODRM_MOD(insn->modrm.value) == 0)
			return -EINVAL;

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

#ifdef CONFIG_VM86

/*
 * Obtain the segment selector to use based on any prefixes in the instruction
 * or in the offset of the register given by the r/m part of the ModRM byte. The
 * register offset is as found in struct pt_regs.
 */
static unsigned short __get_segment_selector_16(struct pt_regs *regs,
						struct insn *insn, int regoff)
{
	int i;

	struct kernel_vm86_regs *vm86regs = (struct kernel_vm86_regs *)regs;

	/*
	 * If not in virtual-8086 mode, the segment selector is not used
	 * to compute addresses but to select the segment descriptor. Return
	 * 0 to ease the computation of address.
	 */
	if (!v8086_mode(regs))
		return 0;

	insn_get_prefixes(insn);

	/* Check first if we have selector overrides */
	for (i = 0; i < insn->prefixes.nbytes; i++) {
		switch (insn->prefixes.bytes[i]) {
		/*
		 * Code and stack segment selector register are saved in all
		 * processor modes. Thus, it makes sense to take them
		 * from pt_regs.
		 */
		case 0x2e:
			return (unsigned short)regs->cs;
		case 0x36:
			return (unsigned short)regs->ss;
		/*
		 * The rest of the segment selector registers are only saved
		 * in virtual-8086 mode. Thus, we must obtain them from the
		 * vm86 register structure.
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

	/*
	 * If no overrides, use default selectors as described in the
	 * Intel documentationn.
	 */
	switch (regoff) {
	case -EINVAL: /* no register involved in address computation */
	case offsetof(struct pt_regs, bx):
	case offsetof(struct pt_regs, di):
	case offsetof(struct pt_regs, si):
		return vm86regs->ds;
	case offsetof(struct pt_regs, bp):
	case offsetof(struct pt_regs, sp):
		return (unsigned short)regs->ss;
	/* ax, cx, dx are not valid registers for 16-bit addressing*/
	default:
		return -EINVAL;
	}
}

/*
 * Obtain offsets from pt_regs to the two registers indicated by the
 * r/m part of the ModRM byte. A negative offset indicates that the
 * register should not be used.
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
		-EINVAL,
		-EINVAL,
		-EINVAL,
		-EINVAL,
	};

	if (!offs1 || !offs2)
		return -EINVAL;

	/* operand is a register, use the generic function */
	if (X86_MODRM_MOD(insn->modrm.value) == 3) {
		*offs1 = insn_get_reg_offset_rm(insn, regs);
		*offs2 = -EINVAL;
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
		*offs1 = -EINVAL;

	return 0;
}

static void __user *insn_get_addr_ref_16(struct insn *insn,
					 struct pt_regs *regs)
{
	unsigned long addr;
	unsigned short addr1 = 0, addr2 = 0;
	int addr_offset1, addr_offset2;
	int ret;
	unsigned short seg = 0;

	insn_get_displacement(insn);

	/*
	 * If operand is a register, the layout is the same as in
	 * 32-bit and 64-bit addressing.
	 */
	if (X86_MODRM_MOD(insn->modrm.value) == 3) {
		addr_offset1 = get_reg_offset(insn, regs, REG_TYPE_RM);
		if (addr_offset1 < 0)
			goto out_err;
		seg = __get_segment_selector_16(regs, insn, addr_offset1);
		addr = (seg << 4) + regs_get_register(regs, addr_offset1);
	} else {
		ret = get_reg_offset_16(insn, regs, &addr_offset1,
					&addr_offset2);
		if (ret < 0)
			goto out_err;
		/*
		 * Don't fail on invalid offset values. They might be invalid
		 * because they are not supported. Instead, use them in the
		 * calculation only if they contain a valid value.
		 */
		if (addr_offset1 >= 0)
			addr1 = regs_get_register(regs, addr_offset1);
		if (addr_offset2 >= 0)
			addr2 = regs_get_register(regs, addr_offset2);
		seg = __get_segment_selector_16(regs, insn, addr_offset1);
		if (seg < 0)
			goto out_err;
		addr =  (seg << 4) + addr1 + addr2;
	}
	addr += insn->displacement.value;

	return (void __user *)addr;
out_err:
	return (void __user *)-1;
}
#else

static void __user *insn_get_addr_ref_16(struct insn *insn,
					 struct pt_regs *regs)
{
	return (void __user *)-1;
}

#endif /* CONFIG_VM86 */

int insn_get_reg_offset_rm(struct insn *insn, struct pt_regs *regs)
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
	unsigned long addr, base, indx;
	int addr_offset, base_offset, indx_offset;
	insn_byte_t sib;

	if (insn->addr_bytes == 2)
		return insn_get_addr_ref_16(insn, regs);

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
			base_offset = get_reg_offset(insn, regs, REG_TYPE_BASE);
			if (base_offset < 0)
				goto out_err;

			indx_offset = get_reg_offset(insn, regs, REG_TYPE_INDEX);
			/*
			 * A negative offset means that the register cannot be
			 * be used as an index.
			 */
			if (indx_offset < 0)
				indx = 0;
			else
				indx = regs_get_register(regs, indx_offset);

			base = regs_get_register(regs, base_offset);
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
