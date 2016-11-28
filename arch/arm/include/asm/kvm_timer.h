/*
 * Copyright (C) 2016 - Columbia University
 * Author: Jintack Lim <jintack@cs.columbia.edu>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef __ARM_KVM_TIMER_H__
#define __ARM_KVM_TIMER_H__

#include <clocksource/arm_arch_timer.h>

static inline u32 __hyp_text get_el1pcten(void)
{
	return CNTHCTL_EL1PCTEN_NVHE;
}

static inline u32 __hyp_text get_el1pcen(void)
{
	return CNTHCTL_EL1PCEN_NVHE;
}

#endif	/* __ARM_KVM_TIMER_H__ */
