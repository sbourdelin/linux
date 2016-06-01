#ifndef __TRACE_OUTPUT_STM_H
#define __TRACE_OUTPUT_STM_H

#include <linux/module.h>

#ifdef CONFIG_STM_FTRACE
extern void stm_ftrace_write(const char *buf, unsigned int len,
			     unsigned int chan);
extern void ftrace_stm_func(unsigned long ip, unsigned long parent_ip);
#else
static inline void ftrace_stm_func(unsigned long ip, unsigned long parent_ip) {}
#endif

#endif /* __TRACE_OUTPUT_STM_H */
