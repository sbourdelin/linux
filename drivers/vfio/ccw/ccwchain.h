/*
 * ccwchain interfaces
 *
 * Copyright IBM Corp. 2016
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License (version 2 only)
 * as published by the Free Software Foundation.
 *
 * Author(s): Dong Jia Shi <bjsdjshi@linux.vnet.ibm.com>
 *            Xiao Feng Ren <renxiaof@linux.vnet.ibm.com>
 */

#ifndef _CCW_CHAIN_H_
#define _CCW_CHAIN_H_

#include <asm/cio.h>
#include <asm/scsw.h>

/**
 * struct ccwchain_cmd - manage information for ccw program
 * @u_ccwchain: handle of a user-space ccw program
 * @k_ccwchain: handle of a kernel-space ccw program
 * @nr: number of ccws in the ccw program
 *
 * @u_ccwchain is an user-space virtual address of a buffer where a user-space
 * ccw program is stored. Size of this buffer is 4K bytes, of which the low 2K
 * is for the ccws and the upper 2K for cda data.
 *
 * @k_ccwchain is a kernel-space physical address of a ccwchain struct, that
 * points to the translated result of @u_ccwchain. This is opaque to user-space
 * programs.
 *
 * @nr is the number of ccws in both user-space ccw program and kernel-space ccw
 * program.
 */
struct ccwchain_cmd {
	void *u_ccwchain;
	void *k_ccwchain;
	int nr;
};

extern int ccwchain_alloc(struct ccwchain_cmd *cmd);
extern void ccwchain_free(struct ccwchain_cmd *cmd);
extern int ccwchain_prefetch(struct ccwchain_cmd *cmd);
extern struct ccw1 *ccwchain_get_cpa(struct ccwchain_cmd *cmd);
extern void ccwchain_update_scsw(struct ccwchain_cmd *cmd, union scsw *scsw);

#endif
