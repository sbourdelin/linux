#ifndef _ASM_ARM64_TRACE_CLOCK_H
#define _ASM_ARM64_TRACE_CLOCK_H

#include <linux/compiler.h>
#include <linux/types.h>
#include <asm/arch_timer.h>

/*
 * trace_clock_arm64_count_vct(): A clock that is just the cycle counter.
 * Unlike the other clocks, this is not in nanoseconds.
 */
static inline u64 notrace trace_clock_arm64_count_vct(void)
{
	return arch_counter_get_cntvct();
}

# define ARCH_TRACE_CLOCKS \
	{ trace_clock_arm64_count_vct,	"arm64-count-vct",	0 },

#endif  /* _ASM_ARM64_TRACE_CLOCK_H */
