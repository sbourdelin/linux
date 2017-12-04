/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LINUX_STACKTRACE_H
#define __LINUX_STACKTRACE_H

#include <linux/types.h>
#include <asm-generic/sections.h>

struct task_struct;
struct pt_regs;

#ifdef CONFIG_STACKTRACE
struct stack_trace {
	unsigned int nr_entries, max_entries;
	unsigned long *entries;
	int skip;	/* input argument: How many entries to skip */
};

extern void save_stack_trace(struct stack_trace *trace);
extern void save_stack_trace_regs(struct pt_regs *regs,
				  struct stack_trace *trace);
extern void save_stack_trace_tsk(struct task_struct *tsk,
				struct stack_trace *trace);
extern int save_stack_trace_tsk_reliable(struct task_struct *tsk,
					 struct stack_trace *trace);

extern void print_stack_trace(struct stack_trace *trace, int spaces);
extern int snprint_stack_trace(char *buf, size_t size,
			struct stack_trace *trace, int spaces);

static inline int in_irqentry_text(unsigned long ptr)
{
	return (ptr >= (unsigned long)&__irqentry_text_start &&
		ptr < (unsigned long)&__irqentry_text_end) ||
		(ptr >= (unsigned long)&__softirqentry_text_start &&
		 ptr < (unsigned long)&__softirqentry_text_end);
}

static inline void filter_irq_stacks(struct stack_trace *trace)
{
	int i;

	if (!trace->nr_entries)
		return;
	for (i = 0; i < trace->nr_entries; i++)
		if (in_irqentry_text(trace->entries[i])) {
			/* Include the irqentry function into the stack. */
			trace->nr_entries = i + 1;
			break;
		}
}

#ifdef CONFIG_USER_STACKTRACE_SUPPORT
extern void save_stack_trace_user(struct stack_trace *trace);
#else
# define save_stack_trace_user(trace)              do { } while (0)
#endif

#else /* !CONFIG_STACKTRACE */
# define save_stack_trace(trace)			do { } while (0)
# define save_stack_trace_tsk(tsk, trace)		do { } while (0)
# define save_stack_trace_user(trace)			do { } while (0)
# define print_stack_trace(trace, spaces)		do { } while (0)
# define snprint_stack_trace(buf, size, trace, spaces)	do { } while (0)
# define filter_irq_stacks(trace)			do { } while (0)
# define in_irqentry_text(ptr)				do { } while (0)
# define save_stack_trace_tsk_reliable(tsk, trace)	({ -ENOSYS; })
#endif /* CONFIG_STACKTRACE */

#endif /* __LINUX_STACKTRACE_H */
