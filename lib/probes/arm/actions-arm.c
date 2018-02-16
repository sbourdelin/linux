/*
 * Copyright (C) 2012 Rabin Vincent <rabin at rab.in>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/stddef.h>
#include <linux/wait.h>
#include <linux/uprobes.h>
#include <linux/module.h>

#include "decode.h"
#include "decode-arm.h"
#include <../../../arm/include/asm/opcodes.h>

static int uprobes_substitute_pc(unsigned long *pinsn, u32 oregs)
{
	probes_opcode_t insn = __mem_to_opcode_arm(*pinsn);
	probes_opcode_t temp;
	probes_opcode_t mask;
	int freereg;
	u32 free = 0xffff;
	u32 regs;

	for (regs = oregs; regs; regs >>= 4, insn >>= 4) {
		if ((regs & 0xf) == REG_TYPE_NONE)
			continue;

		free &= ~(1 << (insn & 0xf));
	}

	/* No PC, no problem */
	if (free & (1 << 15))
		return 15;

	if (!free)
		return -1;

	/*
	 * fls instead of ffs ensures that for "ldrd r0, r1, [pc]" we would
	 * pick LR instead of R1.
	 */
	freereg = free = fls(free) - 1;

	temp = __mem_to_opcode_arm(*pinsn);
	insn = temp;
	regs = oregs;
	mask = 0xf;

	for (; regs; regs >>= 4, mask <<= 4, free <<= 4, temp >>= 4) {
		if ((regs & 0xf) == REG_TYPE_NONE)
			continue;

		if ((temp & 0xf) != 15)
			continue;

		insn &= ~mask;
		insn |= free & mask;
	}

	*pinsn = __opcode_to_mem_arm(insn);
	return freereg;
}

static void uprobe_set_pc(struct arch_uprobe *auprobe,
			  struct arch_uprobe_task *autask,
			  struct pt_regs *regs)
{
	u32 pcreg = auprobe->pcreg;

	if (pcreg == 15) {
		autask->backup = instruction_pointer(regs);
		instruction_pointer_set(regs, instruction_pointer(regs) + 8);
	} else {
		autask->backup = pt_regs_read_reg(regs, pcreg);
		pt_regs_write_reg(regs, pcreg, instruction_pointer(regs) + 8);
	}
}

static void uprobe_unset_pc(struct arch_uprobe *auprobe,
			    struct arch_uprobe_task *autask,
			    struct pt_regs *regs)
{
	/* PC will be taken care of by common code */
	if (auprobe->pcreg == 15)
		instruction_pointer_set(regs, autask->backup);
	else
		pt_regs_write_reg(regs, auprobe->pcreg, autask->backup);
}

static void uprobe_aluwrite_pc(struct arch_uprobe *auprobe,
			       struct arch_uprobe_task *autask,
			       struct pt_regs *regs)
{
	u32 pcreg = auprobe->pcreg;

	if (pcreg == 15) {
		alu_write_pc(instruction_pointer(regs), regs);
		instruction_pointer_set(regs, autask->backup);
	} else {
		alu_write_pc(pt_regs_read_reg(regs, pcreg), regs);
		pt_regs_write_reg(regs, pcreg, autask->backup);
	}
}

static void uprobe_write_pc(struct arch_uprobe *auprobe,
			    struct arch_uprobe_task *autask,
			    struct pt_regs *regs)
{
	u32 pcreg = auprobe->pcreg;

	if (pcreg == 15) {
		load_write_pc(instruction_pointer(regs), regs);
		instruction_pointer_set(regs, autask->backup);
	} else {
		load_write_pc(pt_regs_read_reg(regs, pcreg), regs);
		pt_regs_write_reg(regs, pcreg, autask->backup);
	}
}

enum probes_insn
decode_pc_ro(probes_opcode_t insn, struct arch_probes_insn *asi,
	     const struct decode_header *d)
{
	struct arch_uprobe *auprobe = container_of(asi, struct arch_uprobe,
							api);

	struct decode_emulate *decode = (struct decode_emulate *) d;
	u32 regs = decode->header.type_regs.bits >> DECODE_TYPE_BITS;
	int reg;

	reg = uprobes_substitute_pc((unsigned long *)&auprobe->ixol[0], regs);
	if (reg == 15)
		return INSN_GOOD;

	if (reg == -1)
		return INSN_REJECTED;

	auprobe->pcreg = reg;
	auprobe->prehandler = uprobe_set_pc;
	auprobe->posthandler = uprobe_unset_pc;

	return INSN_GOOD;
}

enum probes_insn
decode_wb_pc(probes_opcode_t insn, struct arch_probes_insn *asi,
	     const struct decode_header *d, bool alu)
{
	struct arch_uprobe *auprobe = container_of(asi, struct arch_uprobe,
						   api);

	enum probes_insn ret = decode_pc_ro(insn, asi, d);

	if (((insn >> 12) & 0xf) == 15)
		auprobe->posthandler = alu ? uprobe_aluwrite_pc
					   : uprobe_write_pc;

	return ret;
}

enum probes_insn
decode_rd12rn16rm0rs8_rwflags(probes_opcode_t insn,
			      struct arch_probes_insn *asi,
			      const struct decode_header *d)
{
	return decode_wb_pc(insn, asi, d, true);
}

enum probes_insn
decode_ldr(probes_opcode_t insn, struct arch_probes_insn *asi,
	   const struct decode_header *d)
{
	return decode_wb_pc(insn, asi, d, false);
}

/*
 * based on arm kprobes implementation
 */
static void __kprobes simulate_ldm1stm1(probes_opcode_t insn,
		struct arch_probes_insn *asi,
		struct pt_regs *regs)
{
	int rn = (insn >> 16) & 0xf;
	int lbit = insn & (1 << 20);
	int wbit = insn & (1 << 21);
	int ubit = insn & (1 << 23);
	int pbit = insn & (1 << 24);
	int reg;
	u32 *addr = (u32 *)pt_regs_read_reg(regs, rn);
	unsigned int reg_bit_vector;
	unsigned int reg_count;

	reg_count = 0;
	reg_bit_vector = insn & 0xffff;

	while (reg_bit_vector) {
		reg_bit_vector &= (reg_bit_vector - 1);
		++reg_count;
	}
	if (!ubit)
		addr -= reg_count;
	addr += (!pbit == !ubit);

	reg_bit_vector = insn & 0xffff;

	while (reg_bit_vector) {
		reg = __ffs(reg_bit_vector);
		reg_bit_vector &= (reg_bit_vector - 1);
		if (lbit) {	/* LDM */
			if (reg == 15)
				instruction_pointer_set(regs, (*addr++) - 4);
			else
				pt_regs_write_reg(regs, reg, *addr++);
		} else { /* STM */
			if (reg == 15)
				*addr++ = instruction_pointer(regs);
			else
				*addr++ = pt_regs_read_reg(regs, reg);
		}
	}

	/* write back new value of Rn */
	if (wbit) {
		if (!ubit)
			addr -= reg_count;
		addr -= (!pbit == !ubit);
		if (rn == 15)
			instruction_pointer_set(regs, (long)addr);
		else
			pt_regs_write_reg(regs, rn, (long)addr);
	}

	instruction_pointer_set(regs, instruction_pointer(regs) + 4);
}

static void __kprobes simulate_stm1_pc(probes_opcode_t insn,
		struct arch_probes_insn *asi,
		struct pt_regs *regs)
{
	unsigned long addr = instruction_pointer(regs) - 4;

	instruction_pointer_set(regs, (long)addr + str_pc_offset);
	simulate_ldm1stm1(insn, asi, regs);
	instruction_pointer_set(regs, (long)addr + 4);
}

static void __kprobes simulate_ldm1_pc(probes_opcode_t insn,
		struct arch_probes_insn *asi,
		struct pt_regs *regs)
{
	simulate_ldm1stm1(insn, asi, regs);
	load_write_pc(instruction_pointer(regs), regs);
}

enum probes_insn
uprobe_decode_ldmstm_aarch64(probes_opcode_t insn,
	     struct arch_probes_insn *asi,
	     const struct decode_header *d)
{
	probes_insn_handler_t *handler = 0;
	unsigned int reglist = insn & 0xffff;
	int is_ldm = insn & 0x100000;

	/* PC on a reglist? */
	if (reglist & 0x8000)
		handler = is_ldm ? simulate_ldm1_pc : simulate_stm1_pc;
	else
		handler = simulate_ldm1stm1;
	asi->insn_handler = handler;
	return INSN_GOOD_NO_SLOT;
}

enum probes_insn
uprobe_decode_ldmstm(probes_opcode_t insn,
		     struct arch_probes_insn *asi,
		     const struct decode_header *d)
{
	struct arch_uprobe *auprobe = container_of(asi, struct arch_uprobe,
			api);
	unsigned int reglist = insn & 0xffff;
	int rn = (insn >> 16) & 0xf;
	int lbit = insn & (1 << 20);
	unsigned int used = reglist | (1 << rn);

	if (rn == 15)
		return INSN_REJECTED;

	if (!(used & (1 << 15)))
		return INSN_GOOD;

	if (used & (1 << 14))
		return INSN_REJECTED;

	/* Use LR instead of PC */
	insn ^= 0xc000;

	auprobe->pcreg = 14;
	auprobe->ixol[0] = __opcode_to_mem_arm(insn);

	auprobe->prehandler = uprobe_set_pc;
	if (lbit)
		auprobe->posthandler = uprobe_write_pc;
	else
		auprobe->posthandler = uprobe_unset_pc;

	return INSN_GOOD;
}

const union decode_action uprobes_probes_actions[] = {
	[PROBES_PRELOAD_IMM] = {.handler = probes_simulate_nop},
	[PROBES_PRELOAD_REG] = {.handler = probes_simulate_nop},
	[PROBES_BRANCH_IMM] = {.handler = simulate_blx1},
	[PROBES_MRS] = {.handler = simulate_mrs},
	[PROBES_BRANCH_REG] = {.handler = simulate_blx2bx},
	[PROBES_CLZ] = {.handler = probes_simulate_nop},
	[PROBES_SATURATING_ARITHMETIC] = {.handler = probes_simulate_nop},
	[PROBES_MUL1] = {.handler = probes_simulate_nop},
	[PROBES_MUL2] = {.handler = probes_simulate_nop},
	[PROBES_SWP] = {.handler = probes_simulate_nop},
	[PROBES_LDRSTRD] = {.decoder = decode_pc_ro},
	[PROBES_LOAD_EXTRA] = {.decoder = decode_pc_ro},
	[PROBES_LOAD] = {.decoder = decode_ldr},
	[PROBES_STORE_EXTRA] = {.decoder = decode_pc_ro},
	[PROBES_STORE] = {.decoder = decode_pc_ro},
	[PROBES_MOV_IP_SP] = {.handler = simulate_mov_ipsp},
	[PROBES_DATA_PROCESSING_REG] = {
		.decoder = decode_rd12rn16rm0rs8_rwflags},
	[PROBES_DATA_PROCESSING_IMM] = {
		.decoder = decode_rd12rn16rm0rs8_rwflags},
	[PROBES_MOV_HALFWORD] = {.handler = probes_simulate_nop},
	[PROBES_SEV] = {.handler = probes_simulate_nop},
	[PROBES_WFE] = {.handler = probes_simulate_nop},
	[PROBES_SATURATE] = {.handler = probes_simulate_nop},
	[PROBES_REV] = {.handler = probes_simulate_nop},
	[PROBES_MMI] = {.handler = probes_simulate_nop},
	[PROBES_PACK] = {.handler = probes_simulate_nop},
	[PROBES_EXTEND] = {.handler = probes_simulate_nop},
	[PROBES_EXTEND_ADD] = {.handler = probes_simulate_nop},
	[PROBES_MUL_ADD_LONG] = {.handler = probes_simulate_nop},
	[PROBES_MUL_ADD] = {.handler = probes_simulate_nop},
	[PROBES_BITFIELD] = {.handler = probes_simulate_nop},
	[PROBES_BRANCH] = {.handler = simulate_bbl},
#ifdef CONFIG_ARM64
	[PROBES_LDMSTM] = {.decoder = uprobe_decode_ldmstm_aarch64}
#else
	[PROBES_LDMSTM] = {.decoder = uprobe_decode_ldmstm}
#endif
};
