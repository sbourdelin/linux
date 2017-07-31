/*
 * lmb.h: Power specific logical memory block representation
 *
 * ** Add (C) **
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */

#ifndef _ASM_POWERPC_LMB_H
#define _ASM_POWERPC_LMB_H

extern struct lmb_data *lmb_array;
extern int n_mem_addr_cells, n_mem_size_cells;

struct lmb {
	u64	base_address;
	u32	drc_index;
	u32	aa_index;
	u32	flags;
};

struct lmb_data {
	struct lmb	*lmbs;
	int		num_lmbs;
	u32		lmb_size;
};

extern struct lmb_data *lmb_array;

#define for_each_lmb(_lmb)					\
	for (_lmb = &lmb_array->lmbs[0];			\
	     _lmb != &lmb_array->lmbs[lmb_array->num_lmbs];	\
	     _lmb++)

extern int lmb_init(void);
extern u32 lmb_get_lmb_size(void);
extern u64 lmb_get_max_memory(void);
extern unsigned long read_n_cells(int n, const __be32 **buf);
extern void get_n_mem_cells(int *n_addr_cells, int *n_size_cells);

#endif
