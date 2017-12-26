/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_JUMP_LABEL_H
#define _ASM_X86_JUMP_LABEL_H

#ifndef HAVE_JUMP_LABEL
/*
 * For better or for worse, if jump labels (the gcc extension) are missing,
 * then the entire static branch patching infrastructure is compiled out.
 * If that happens, the code in here will malfunction.  Raise a compiler
 * error instead.
 *
 * In theory, jump labels and the static branch patching infrastructure
 * could be decoupled to fix this.
 */
#error asm/jump_label.h included on a non-jump-label kernel
#endif

#define JUMP_LABEL_NOP_SIZE 5

#ifdef CONFIG_X86_64
# define STATIC_KEY_INIT_NOP P6_NOP5_ATOMIC
#else
# define STATIC_KEY_INIT_NOP GENERIC_NOP5_ATOMIC
#endif

#include <asm/asm.h>
#include <asm/nops.h>

#ifndef __ASSEMBLY__

#include <linux/stringify.h>
#include <linux/types.h>

static __always_inline bool arch_static_branch(struct static_key *key, bool branch)
{
	asm_volatile_goto("1:"
		".byte " __stringify(STATIC_KEY_INIT_NOP) "\n\t"
		".pushsection __jump_table,  \"aw\" \n\t"
		".balign 4\n\t"
		".long 1b - ., %l[l_yes] - ., %c0 + %c1 - .\n\t"
		".popsection \n\t"
		: :  "i" (key), "i" (branch) : : l_yes);

	return false;
l_yes:
	return true;
}

static __always_inline bool arch_static_branch_jump(struct static_key *key, bool branch)
{
	asm_volatile_goto("1:"
		".byte 0xe9\n\t .long %l[l_yes] - 2f\n\t"
		"2:\n\t"
		".pushsection __jump_table,  \"aw\" \n\t"
		".balign 4\n\t"
		".long 1b - ., %l[l_yes] - ., %c0 + %c1 - .\n\t"
		".popsection \n\t"
		: :  "i" (key), "i" (branch) : : l_yes);

	return false;
l_yes:
	return true;
}

#ifdef CONFIG_X86_64
typedef u64 jump_label_t;
#else
typedef u32 jump_label_t;
#endif

struct jump_entry {
	s32 code;
	s32 target;
	s32 key;
};

static inline jump_label_t jump_entry_code(const struct jump_entry *entry)
{
	return (jump_label_t)&entry->code + entry->code;
}

static inline jump_label_t jump_entry_target(const struct jump_entry *entry)
{
	return (jump_label_t)&entry->target + entry->target;
}

static inline struct static_key *jump_entry_key(const struct jump_entry *entry)
{
	unsigned long key = (unsigned long)&entry->key + entry->key;

	return (struct static_key *)(key & ~1UL);
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

void jump_label_swap(void *a, void *b, int size);

#else	/* __ASSEMBLY__ */

.macro STATIC_JUMP_IF_TRUE target, key, def
.Lstatic_jump_\@:
	.if \def
	/* Equivalent to "jmp.d32 \target" */
	.byte		0xe9
	.long		\target - .Lstatic_jump_after_\@
.Lstatic_jump_after_\@:
	.else
	.byte		STATIC_KEY_INIT_NOP
	.endif
	.pushsection __jump_table, "aw"
	.balign		4
	.long		.Lstatic_jump_\@ - ., \target - ., \key - .
	.popsection
.endm

.macro STATIC_JUMP_IF_FALSE target, key, def
.Lstatic_jump_\@:
	.if \def
	.byte		STATIC_KEY_INIT_NOP
	.else
	/* Equivalent to "jmp.d32 \target" */
	.byte		0xe9
	.long		\target - .Lstatic_jump_after_\@
.Lstatic_jump_after_\@:
	.endif
	.pushsection __jump_table, "aw"
	.balign		4
	.long		.Lstatic_jump_\@ - ., \target - ., \key - . + 1
	.popsection
.endm

#endif	/* __ASSEMBLY__ */

#endif
