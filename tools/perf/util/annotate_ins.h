#ifndef __ANNOTATE_INS_H
#define __ANNOTATE_INS_H

#include <linux/types.h>

extern bool arch_is_return_ins(const char *s);
extern char *arch_parse_mov_comment(const char *s);
extern char *arch_parse_call_target(char *t);

#ifdef HAVE_ANNOTATE_INS_SUPPORT
#include <annotate_ins.h>
#else
#define ARCH_INSTRUCTIONS { }
#endif

#endif /* __ANNOTATE_INS_H */
