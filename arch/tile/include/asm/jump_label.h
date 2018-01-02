/*
 * Copyright 2015 Tilera Corporation. All Rights Reserved.
 *
 *   This program is free software; you can redistribute it and/or
 *   modify it under the terms of the GNU General Public License
 *   as published by the Free Software Foundation, version 2.
 *
 *   This program is distributed in the hope that it will be useful, but
 *   WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE, GOOD TITLE or
 *   NON INFRINGEMENT.  See the GNU General Public License for
 *   more details.
 */

#ifndef _ASM_TILE_JUMP_LABEL_H
#define _ASM_TILE_JUMP_LABEL_H

#include <arch/opcode.h>

#define JUMP_LABEL_NOP_SIZE	TILE_BUNDLE_SIZE_IN_BYTES

static __always_inline bool arch_static_branch(struct static_key *key,
					       bool branch)
{
	asm_volatile_goto("1:\n\t"
		"nop" "\n\t"
		".pushsection __jump_table,  \"aw\"\n\t"
		".quad 1b, %l[l_yes], %0 + %1 \n\t"
		".popsection\n\t"
		: :  "i" (key), "i" (branch) : : l_yes);
	return false;
l_yes:
	return true;
}

static __always_inline bool arch_static_branch_jump(struct static_key *key,
						    bool branch)
{
	asm_volatile_goto("1:\n\t"
		"j %l[l_yes]" "\n\t"
		".pushsection __jump_table,  \"aw\"\n\t"
		".quad 1b, %l[l_yes], %0 + %1 \n\t"
		".popsection\n\t"
		: :  "i" (key), "i" (branch) : : l_yes);
	return false;
l_yes:
	return true;
}

typedef u64 jump_label_t;

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

#define jump_label_swap		NULL

#endif /* _ASM_TILE_JUMP_LABEL_H */
