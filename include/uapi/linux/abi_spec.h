#ifndef ABI_SPEC_H_
#define ABI_SPEC_H_

#include <linux/fcntl.h>
#include <linux/stat.h>
#define MAX_CONSTRAINTS 10
#define MAX_ARGS 10

#define	TYPE_FD		1
#define TYPE_INT	2
#define TYPE_PTR	3
#define TYPE_STRING	4
/* ... */

#define CONSTRAINT_NON_NULL	(1<<0)
#define CONSTRAINT_RANGE	(1<<1)
#define CONSTRAINT_ADDRESS_TYPE	(1<<2)
#define CONSTRAINT_FD_TYPE	(1<<3)
#define CONSTRAINT_ERRNO	(1<<4)
#define CONSTRAINT_BITMASK	(1<<5)
#define CONSTRAINT_PATH		(1<<6)
/* ... */
/* A generic constraint on an argument or a return value */
struct constraint {
	unsigned int flags;		/* bitmask of applied constraints */
	union {
		struct {		/* int range */
			int int_min;
			int int_max;
		};
		unsigned long bitmask;	/* allowed flags bitmask */
		unsigned long address_flags;	/* Type of allowed addr */
		unsigned long fd_flags;	/* Type of allowed fd */
	};
};

/* A generic argument (or return value) */
struct argument {
	const char *name;
	int type;			/* should be a nicer way to do this */

	unsigned int nconstraints;	/* can there be more than 1-2? */
	struct constraint constraints[MAX_CONSTRAINTS];
};

/* A generic syscall */
struct syscall_spec {
	const char *name;
	struct argument retval;

	unsigned int nargs;
	struct argument args[MAX_ARGS];
};

void abispec_check_pre(const struct syscall_spec *s, ...);
void abispec_check_post(const struct syscall_spec *s, long retval, ...);

#endif
