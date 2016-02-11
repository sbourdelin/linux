#ifndef _LINUX_FTRACE_CALLER_H
#define _LINUX_FTRACE_CALLER_H

#include <asm/ftrace.h>

/* All archs should have this, but we define it for consistency */
#ifndef ftrace_return_address0
# define ftrace_return_address0		__builtin_return_address(0)
#endif

/* Archs may use other ways for ADDR1 and beyond */
#ifndef ftrace_return_address
# ifdef CONFIG_FRAME_POINTER
#  define ftrace_return_address(n)	__builtin_return_address(n)
# else
#  define ftrace_return_address(n)	0UL
# endif
#endif

#define CALLER_ADDR0 ((unsigned long)ftrace_return_address0)
#define CALLER_ADDR1 ((unsigned long)ftrace_return_address(1))
#define CALLER_ADDR2 ((unsigned long)ftrace_return_address(2))
#define CALLER_ADDR3 ((unsigned long)ftrace_return_address(3))
#define CALLER_ADDR4 ((unsigned long)ftrace_return_address(4))
#define CALLER_ADDR5 ((unsigned long)ftrace_return_address(5))
#define CALLER_ADDR6 ((unsigned long)ftrace_return_address(6))

#endif
