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
 * this program; if not, see <http://www.gnu.org/licenses/>.
 *
 * Author: Jamal Hadi Salim <jhs@mojatatu.com>
 */

#ifndef __NET_TC_SKBMOD_H
#define __NET_TC_SKBMOD_H

#include <net/act_api.h>
#include <linux/tc_act/tc_skbmod.h>

struct tcf_skbmod {
	struct tc_action	common;
	u64	flags; /*up to 64 types of operations; extend if needed */
	u8	eth_dst[ETH_ALEN];
	u16	eth_type;
	u8	eth_src[ETH_ALEN];
};
#define to_skbmod(a) ((struct tcf_skbmod *)a)

#endif /* __NET_TC_SKBMOD_H */
