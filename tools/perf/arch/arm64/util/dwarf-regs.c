/*
 * Mapping of DWARF debug register numbers into register names.
 *
 * Copyright (C) 2010 Will Deacon, ARM Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <stddef.h>
#include <linux/ptrace.h> /* for struct user_pt_regs */
#include <dwarf-regs.h>

struct pt_regs_offset {
	const char *name;
	int offset;
};

/*
 * Reference:
 * http://infocenter.arm.com/help/topic/com.arm.doc.ihi0057b/IHI0057B_aadwarf64.pdf
 */
#define REG_OFFSET_NAME(r, num) {.name = "%" #r,			\
			.offset = offsetof(struct user_pt_regs, regs[num])}
#define REG_OFFSET_END {.name = NULL, .offset = 0}
#define GPR_OFFSET_NAME(r) \
	{.name = "%x" #r, .offset = offsetof(struct user_pt_regs, regs[r])}

/* This table is for reverse searching for the offset or register
 * names in aarch64_regstr_tbl[].
 */
static const struct pt_regs_offset regoffset_table[] = {
	GPR_OFFSET_NAME(0),
	GPR_OFFSET_NAME(1),
	GPR_OFFSET_NAME(2),
	GPR_OFFSET_NAME(3),
	GPR_OFFSET_NAME(4),
	GPR_OFFSET_NAME(5),
	GPR_OFFSET_NAME(6),
	GPR_OFFSET_NAME(7),
	GPR_OFFSET_NAME(8),
	GPR_OFFSET_NAME(9),
	GPR_OFFSET_NAME(10),
	GPR_OFFSET_NAME(11),
	GPR_OFFSET_NAME(12),
	GPR_OFFSET_NAME(13),
	GPR_OFFSET_NAME(14),
	GPR_OFFSET_NAME(15),
	GPR_OFFSET_NAME(16),
	GPR_OFFSET_NAME(17),
	GPR_OFFSET_NAME(18),
	GPR_OFFSET_NAME(19),
	GPR_OFFSET_NAME(20),
	GPR_OFFSET_NAME(21),
	GPR_OFFSET_NAME(22),
	GPR_OFFSET_NAME(23),
	GPR_OFFSET_NAME(24),
	GPR_OFFSET_NAME(25),
	GPR_OFFSET_NAME(26),
	GPR_OFFSET_NAME(27),
	GPR_OFFSET_NAME(28),
	GPR_OFFSET_NAME(29),
	REG_OFFSET_NAME(lr, 30),
	REG_OFFSET_NAME(sp, 31),
	REG_OFFSET_END,
};

/* Minus 1 for the ending REG_OFFSET_END */
#define ARCH_MAX_REGS ((sizeof(regoffset_table) /		\
			sizeof(regoffset_table[0])) - 1)

/* Return architecture dependent register string (for kprobe-tracer) */
const char *get_arch_regstr(unsigned int n)
{
	return (n < ARCH_MAX_REGS) ? regoffset_table[n].name : NULL;
}
