#ifndef ARCH_ANNOTATE_INS_H
#define ARCH_ANNOTATE_INS_H

#define ARCH_INSTRUCTIONS { \
	{ .name = "add",   .ops  = &mov_ops, }, \
	{ .name = "and",   .ops  = &mov_ops, }, \
	{ .name = "b",     .ops  = &jump_ops, }, /* might also be a call */ \
	{ .name = "b.al",  .ops  = &jump_ops, }, \
	{ .name = "b.cc",  .ops  = &jump_ops, }, \
	{ .name = "b.cs",  .ops  = &jump_ops, }, \
	{ .name = "b.eq",  .ops  = &jump_ops, }, \
	{ .name = "b.ge",  .ops  = &jump_ops, }, \
	{ .name = "b.gt",  .ops  = &jump_ops, }, \
	{ .name = "b.hi",  .ops  = &jump_ops, }, \
	{ .name = "b.hs",  .ops  = &jump_ops, }, \
	{ .name = "b.le",  .ops  = &jump_ops, }, \
	{ .name = "b.lo",  .ops  = &jump_ops, }, \
	{ .name = "b.ls",  .ops  = &jump_ops, }, \
	{ .name = "b.lt",  .ops  = &jump_ops, }, \
	{ .name = "b.mi",  .ops  = &jump_ops, }, \
	{ .name = "b.ne",  .ops  = &jump_ops, }, \
	{ .name = "b.nv",  .ops  = &jump_ops, }, \
	{ .name = "b.pl",  .ops  = &jump_ops, }, \
	{ .name = "b.vc",  .ops  = &jump_ops, }, \
	{ .name = "b.vs",  .ops  = &jump_ops, }, \
	{ .name = "bl",    .ops  = &call_ops, }, \
	{ .name = "blr",   .ops  = &call_ops, }, \
	{ .name = "cbnz",  .ops  = &jump_ops, }, \
	{ .name = "cbz",   .ops  = &jump_ops, }, \
	{ .name = "cmp",   .ops  = &mov_ops, }, \
	{ .name = "mov",   .ops  = &mov_ops, }, \
	{ .name = "nop",   .ops  = &nop_ops, }, \
	{ .name = "orr",   .ops  = &mov_ops, }, \
	{ .name = "tbnz",  .ops  = &jump_ops, }, \
	{ .name = "tbz",   .ops  = &jump_ops, }, \
	}

#define ARCH_ACTIONS "Actions are only available for 'ret' & branch instructions."

#endif /* ARCH_ANNOTATE_INS_H */
