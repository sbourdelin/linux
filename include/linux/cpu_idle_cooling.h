/*
 *  linux/drivers/thermal/cpu_idle_cooling.h
 *
 *  Copyright (C) 2017  Tao Wang <kevin.wangtao@hisilicon.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; version 2 of the License.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef __CPU_IDLE_COOLING_H__
#define __CPU_IDLE_COOLING_H__

#include <linux/cpumask.h>

#ifdef CONFIG_CPU_IDLE_THERMAL
unsigned long get_max_idle_state(const struct cpumask *clip_cpus);
void set_idle_state(const struct cpumask *clip_cpus,
			unsigned long idle_ratio);
#else
static inline unsigned long get_max_idle_state(const struct cpumask *clip_cpus)
{
	return 0;
}

static inline void set_idle_state(const struct cpumask *clip_cpus,
			unsigned long idle_ratio) {}
#endif	/* CONFIG_CPU_IDLE_THERMAL */

#endif /* __CPU_IDLE_COOLING_H__ */
