/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_S390_JUMP_LABEL_H
#define _ASM_S390_JUMP_LABEL_H

#ifndef __ASSEMBLY__

#include <linux/types.h>
#include <linux/stringify.h>

#define JUMP_LABEL_NOP_SIZE 6
#define JUMP_LABEL_NOP_OFFSET 2

/*
 * We use a brcl 0,2 instruction for jump labels at compile time so it
 * can be easily distinguished from a hotpatch generated instruction.
 */
static __always_inline bool arch_static_branch(struct static_key *key, bool branch)
{
	asm_volatile_goto("0:	brcl 0,"__stringify(JUMP_LABEL_NOP_OFFSET)"\n"
		".pushsection __jump_table, \"aw\"\n"
		".balign 8\n"
		".quad 0b, %l[label], %0\n"
		".popsection\n"
		: : "X" (&((char *)key)[branch]) : : label);

	return false;
label:
	return true;
}

static __always_inline bool arch_static_branch_jump(struct static_key *key, bool branch)
{
	asm_volatile_goto("0:	brcl 15, %l[label]\n"
		".pushsection __jump_table, \"aw\"\n"
		".balign 8\n"
		".quad 0b, %l[label], %0\n"
		".popsection\n"
		: : "X" (&((char *)key)[branch]) : : label);

	return false;
label:
	return true;
}

typedef unsigned long jump_label_t;

struct jump_entry {
	jump_label_t code;
	jump_label_t target;
	jump_label_t key;
};

static inline jump_label_t jump_entry_code(const struct jump_entry *entry)
{
	return entry->code;
}

static inline struct static_key *jump_entry_key(const struct jump_entry *entry)
{
	return (struct static_key *)((unsigned long)entry->key & ~1UL);
}

static inline bool jump_entry_is_branch(const struct jump_entry *entry)
{
	return (unsigned long)entry->key & 1UL;
}

static inline bool jump_entry_is_module_init(const struct jump_entry *entry)
{
	return entry->code == 0;
}

static inline void jump_entry_set_module_init(struct jump_entry *entry)
{
	entry->code = 0;
}

#define jump_label_swap			NULL

#endif  /* __ASSEMBLY__ */
#endif
