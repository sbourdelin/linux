// SPDX-License-Identifier: GPL-2.0
#ifndef __PERSISTENT_CLOCK_H__
#define __PERSISTENT_CLOCK_H__

#ifdef CONFIG_PERSISTENT_CLOCK
extern int persistent_clock_init_and_register(u64 (*read)(void),
					      u64 mask, u32 freq,
					      u64 maxsec);
extern void persistent_clock_cleanup(void);
extern void persistent_clock_start_alarmtimer(void);
#else
static inline int persistent_clock_init_and_register(u64 (*read)(void),
						     u64 mask, u32 freq,
						     u64 maxsec)
{
	return 0;
}

static inline void persistent_clock_cleanup(void) { }
static inline void persistent_clock_start_alarmtimer(void) { }
#endif

#endif
