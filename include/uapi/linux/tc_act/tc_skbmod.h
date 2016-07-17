/*
 * Copyright (c) 2016, Jamal Hadi Salim
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 59 Temple
 * Place - Suite 330, Boston, MA 02111-1307 USA.
 *
 * Author: Jamal Hadi Salim
 */

#ifndef __LINUX_TC_SKBMOD_H
#define __LINUX_TC_SKBMOD_H

#include <linux/pkt_cls.h>

#define TCA_ACT_SKBMOD 15

#define SKBMOD_F_DMAC	0x1
#define SKBMOD_F_SMAC	0x2
#define SKBMOD_F_ETYPE	0x4

struct tc_skbmod {
	tc_gen;
};

enum {
	TCA_SKBMOD_UNSPEC,
	TCA_SKBMOD_TM,
	TCA_SKBMOD_PARMS,
	TCA_SKBMOD_DMAC,
	TCA_SKBMOD_SMAC,
	TCA_SKBMOD_ETYPE,
	TCA_SKBMOD_PAD,
	__TCA_SKBMOD_MAX
};
#define TCA_SKBMOD_MAX (__TCA_SKBMOD_MAX - 1)

#endif
