/*
 * ccwprogram interfaces
 *
 * Copyright IBM Corp. 2017
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License (version 2 only)
 * as published by the Free Software Foundation.
 *
 * Author(s): Dong Jia Shi <bjsdjshi@linux.vnet.ibm.com>
 *            Xiao Feng Ren <renxiaof@linux.vnet.ibm.com>
 */

#ifndef _VFIO_CCW_CP_H_
#define _VFIO_CCW_CP_H_

#include <asm/cio.h>
#include <asm/scsw.h>

#include "orb.h"

/**
 * struct ccwprogram - manage information for ccw program
 * @ccwchain_list: list head of ccwchains
 * @orb: orb for the currently processed ssch request
 * @mdev: the mediated device to perform page pinning/unpinning
 *
 * @ccwchain_list is the head of a ccwchain list, that contents the
 * translated result of the guest ccw program that pointed out by
 * the iova parameter when calling cp_init.
 */
struct ccwprogram {
	struct list_head ccwchain_list;
	union orb orb;
	struct device *mdev;
};

extern int cp_init(struct ccwprogram *cp, struct device *mdev, union orb *orb);
extern void cp_free(struct ccwprogram *cp);
extern int cp_prefetch(struct ccwprogram *cp);
extern union orb *cp_get_orb(struct ccwprogram *cp, u32 intparm, u8 lpm);
extern void cp_update_scsw(struct ccwprogram *cp, union scsw *scsw);
extern bool cp_iova_pinned(struct ccwprogram *cp, u64 iova);

#endif
