#ifndef __TRACE_OUTPUT_STM_H
#define __TRACE_OUTPUT_STM_H

#include <linux/module.h>

#if IS_ENABLED(CONFIG_STM_SOURCE_FTRACE)
struct stm_ftrace;
extern void
trace_func_to_stm(unsigned long ip, unsigned long parent_ip);
extern void trace_add_output(struct stm_ftrace *stm);
extern void trace_rm_output(void);
#else
static inline void
trace_func_to_stm(unsigned long ip, unsigned long parent_ip) {}
#endif

#endif /* __TRACE_OUTPUT_STM_H */
