/*
 * Utility functions for x86 operand and address decoding
 *
 * Copyright (C) Intel Corporation 2016
 */
#include <linux/kernel.h>
#include <linux/string.h>
#include <asm/inat.h>
#include <asm/insn.h>
#include <asm/insn-eval.h>

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
