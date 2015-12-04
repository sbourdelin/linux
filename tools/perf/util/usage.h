#ifndef __PERF_USAGE_H
#define __PERF_USAGE_H

#include "compat-util.h"

extern void usage(const char *err) NORETURN;
extern void die(const char *err, ...) NORETURN __attribute__((format (printf, 1, 2)));
extern int error(const char *err, ...) __attribute__((format (printf, 1, 2)));
extern void warning(const char *err, ...) __attribute__((format (printf, 1, 2)));

#include "../../../include/linux/stringify.h"

#define DIE_IF(cnd)	\
	do { if (cnd)	\
		die(" at (" __FILE__ ":" __stringify(__LINE__) "): "	\
		    __stringify(cnd) "\n");				\
	} while (0)


extern void set_die_routine(void (*routine)(const char *err, va_list params) NORETURN);
extern void set_warning_routine(void (*routine)(const char *err, va_list params));

#endif /* __PERF_USAGE_H */
