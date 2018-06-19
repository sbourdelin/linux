/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_JUMP_LABEL_H
#define _ASM_X86_JUMP_LABEL_H

#define JUMP_LABEL_NOP_SIZE 5

#ifdef CONFIG_X86_64
# define STATIC_KEY_INIT_NOP P6_NOP5_ATOMIC
#else
# define STATIC_KEY_INIT_NOP GENERIC_NOP5_ATOMIC
#endif

#include <asm/asm.h>
#include <asm/nops.h>

#ifndef __ASSEMBLY__

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

#include <linux/stringify.h>
#include <linux/types.h>

static __always_inline bool arch_static_branch(struct static_key *key, bool branch)
{
	asm_volatile_goto("STATIC_BRANCH_NOP l_yes=\"%l[l_yes]\" key=\"%c0\" "
			  "branch=\"%c1\""
			: :  "i" (key), "i" (branch) : : l_yes);

	return false;
l_yes:
	return true;
}

static __always_inline bool arch_static_branch_jump(struct static_key *key, bool branch)
{
	asm_volatile_goto("STATIC_BRANCH_JMP l_yes=\"%l[l_yes]\" key=\"%c0\" "
			  "branch=\"%c1\""
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
	jump_label_t code;
	jump_label_t target;
	jump_label_t key;
};

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
	_ASM_ALIGN
	_ASM_PTR	.Lstatic_jump_\@, \target, \key
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
	_ASM_ALIGN
	_ASM_PTR	.Lstatic_jump_\@, \target, \key + 1
	.popsection
.endm

.macro STATIC_BRANCH_NOP l_yes:req key:req branch:req
1:
	.byte STATIC_KEY_INIT_NOP
	.pushsection __jump_table, "aw"
	_ASM_ALIGN
	_ASM_PTR 1b, \l_yes, \key + \branch
	.popsection
.endm

.macro STATIC_BRANCH_JMP l_yes:req key:req branch:req
1:
	.byte 0xe9
	.long \l_yes - 2f
2:
	.pushsection __jump_table, "aw"
	_ASM_ALIGN
	_ASM_PTR 1b, \l_yes, \key + \branch
	.popsection
.endm

#endif	/* __ASSEMBLY__ */

#endif
