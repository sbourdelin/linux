#ifndef _FTRACE_H
#define _FTRACE_H

#ifndef __ASSEMBLER__

unsigned long prepare_ftrace_return(unsigned long parent, unsigned long ip);

#endif	/* __ASSEMBLER__ */
#endif	/* _FTRACE_H */
