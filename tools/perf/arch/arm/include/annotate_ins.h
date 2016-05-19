#ifndef ARCH_ANNOTATE_INS_H
#define ARCH_ANNOTATE_INS_H

#define ARCH_INSTRUCTIONS { \
	{ .name = "add",   .ops  = &mov_ops, }, \
	{ .name = "and",   .ops  = &mov_ops, }, \
	{ .name = "b",     .ops  = &jump_ops, }, /* might also be a call */ \
	{ .name = "bcc",   .ops  = &jump_ops, }, \
	{ .name = "bcs",   .ops  = &jump_ops, }, \
	{ .name = "beq",   .ops  = &jump_ops, }, \
	{ .name = "bge",   .ops  = &jump_ops, }, \
	{ .name = "bgt",   .ops  = &jump_ops, }, \
	{ .name = "bhi",   .ops  = &jump_ops, }, \
	{ .name = "bl",    .ops  = &call_ops, }, \
	{ .name = "bls",   .ops  = &jump_ops, }, \
	{ .name = "blt",   .ops  = &jump_ops, }, \
	{ .name = "blx",   .ops  = &call_ops, }, \
	{ .name = "bne",   .ops  = &jump_ops, }, \
	{ .name = "cmp",   .ops  = &mov_ops, }, \
	{ .name = "mov",   .ops  = &mov_ops, }, \
	{ .name = "nop",   .ops  = &nop_ops, }, \
	{ .name = "orr",   .ops  = &mov_ops, }, \
	}

#endif /* ARCH_ANNOTATE_INS_H */
