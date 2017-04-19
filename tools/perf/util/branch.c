#include "perf.h"
#include "util/util.h"
#include "util/debug.h"
#include "util/branch.h"

static bool cross_area(u64 addr1, u64 addr2, int size)
{
	u64 align1, align2;

	align1 = addr1 & ~(size - 1);
	align2 = addr2 & ~(size - 1);

	return (align1 != align2) ? true : false;
}

#define AREA_4K		4096
#define AREA_2M		(2 * 1024 * 1024)

void branch_type_count(struct branch_type_stat *stat,
		       struct branch_flags *flags,
		       u64 from, u64 to)
{
	if (flags->type == PERF_BR_NONE || from == 0)
		return;

	stat->counts[flags->type]++;

	if (flags->type == PERF_BR_JCC) {
		if (to > from)
			stat->jcc_fwd++;
		else
			stat->jcc_bwd++;
	}

	if (cross_area(from, to, AREA_2M))
		stat->cross_2m++;
	else if (cross_area(from, to, AREA_4K))
		stat->cross_4k++;
}

const char *branch_type_name(int type)
{
	const char *branch_names[PERF_BR_MAX] = {
		"N/A",
		"JCC",
		"JMP",
		"IND_JMP",
		"CALL",
		"IND_CALL",
		"RET",
		"SYSCALL",
		"SYSRET",
		"IRQ",
		"INT",
		"IRET",
		"FAR_BRANCH",
	};

	if (type >= 0 && type < PERF_BR_MAX)
		return branch_names[type];

	return NULL;
}
