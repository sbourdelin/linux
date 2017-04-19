#ifndef _PERF_BRANCH_H
#define _PERF_BRANCH_H 1

#include <stdint.h>
#include "../perf.h"

struct branch_type_stat {
	u64 counts[PERF_BR_MAX];
	u64 jcc_fwd;
	u64 jcc_bwd;
	u64 cross_4k;
	u64 cross_2m;
};

struct branch_flags;

void branch_type_count(struct branch_type_stat *stat,
		       struct branch_flags *flags,
		       u64 from, u64 to);

const char *branch_type_name(int type);

#endif /* _PERF_BRANCH_H */
