/*
 *  arch/arm/include/asm/pgtable-hwdef.h
 *
 *  Copyright (C) 1995-2002 Russell King
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#ifndef _ASMARM_PGTABLE_HWDEF_H
#define _ASMARM_PGTABLE_HWDEF_H

#ifdef CONFIG_ARM_LPAE
#include <asm/pgtable-3level-hwdef.h>
#else
#include <asm/pgtable-2level-hwdef.h>
#endif

#ifdef CONFIG_ARM_PV_FIXUP

#define MAX_ATTR_MOD_ENTRIES	64

#ifndef __ASSEMBLY__

struct attr_mod_entry {
	pmdval_t	test_mask;
	pmdval_t	test_value;
	pmdval_t	clear_mask;
	pmdval_t	set_mask;
};

bool attr_mod_add(struct attr_mod_entry *pmod);

extern int num_attr_mods;
extern struct attr_mod_entry attr_mod_table[MAX_ATTR_MOD_ENTRIES];

#endif	/* __ASSEMBLY__ */
#endif	/* CONFIG_ARM_PV_FIXUP */

#endif
