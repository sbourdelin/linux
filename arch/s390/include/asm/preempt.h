#ifndef __ASM_PREEMPT_H
#define __ASM_PREEMPT_H

#include <linux/thread_info.h>
#include <asm/lowcore.h>

#define PREEMPT_ENABLED	(0 + PREEMPT_NEED_RESCHED)

static __always_inline int preempt_count(void)
{
	return (S390_lowcore.preempt_count & ~PREEMPT_NEED_RESCHED);
}

static __always_inline void preempt_count_set(int pc)
{
	S390_lowcore.preempt_count = pc;
}

#define init_task_preempt_count(p)	do { } while (0)

#define init_idle_preempt_count(p, cpu)	do { \
	S390_lowcore.preempt_count = PREEMPT_ENABLED; \
} while (0)

static __always_inline void set_preempt_need_resched(void)
{
	S390_lowcore.preempt_count &= ~PREEMPT_NEED_RESCHED;
}

static __always_inline void clear_preempt_need_resched(void)
{
	S390_lowcore.preempt_count |= PREEMPT_NEED_RESCHED;
}

static __always_inline bool test_preempt_need_resched(void)
{
	return !(S390_lowcore.preempt_count & PREEMPT_NEED_RESCHED);
}

static __always_inline int __preempt_count_laa(int val)
{
	int old_val;

	asm volatile(
		"	laa	%0,%0,%1\n"
		: "=d" (old_val), "+Q" (S390_lowcore.preempt_count)
		: "0" (val) : "cc", "memory");
	return old_val;
}

static __always_inline void __preempt_count_add(int val)
{
	if (IS_ENABLED(CONFIG_HAVE_MARCH_Z196_FEATURES) &&
	    (!__builtin_constant_p(val) || (val < -128) || (val > 127))) {
		__preempt_count_laa(val);
		return;
	}
	S390_lowcore.preempt_count += val;
}

static __always_inline void __preempt_count_sub(int val)
{
	if (IS_ENABLED(CONFIG_HAVE_MARCH_Z196_FEATURES) &&
	    (!__builtin_constant_p(val) || (val < -127) || (val > 128))) {
		__preempt_count_laa(-val);
		return;
	}
	S390_lowcore.preempt_count -= val;
}

static __always_inline bool __preempt_count_dec_and_test(void)
{
	if (IS_ENABLED(CONFIG_HAVE_MARCH_Z196_FEATURES))
		return __preempt_count_laa(-1) == 1;
	return !--S390_lowcore.preempt_count;
}

static __always_inline bool should_resched(int preempt_offset)
{
	return unlikely(S390_lowcore.preempt_count == preempt_offset);
}

#ifdef CONFIG_PREEMPT
extern asmlinkage void preempt_schedule(void);
#define __preempt_schedule() preempt_schedule()
extern asmlinkage void preempt_schedule_notrace(void);
#define __preempt_schedule_notrace() preempt_schedule_notrace()
#endif /* CONFIG_PREEMPT */

#endif /* __ASM_PREEMPT_H */
