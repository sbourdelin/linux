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
	if (flags->type == PERF_BR_UNKNOWN || from == 0)
		return;

	stat->counts[flags->type]++;

	if (flags->type == PERF_BR_COND) {
		if (to > from)
			stat->cond_fwd++;
		else
			stat->cond_bwd++;
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
		"COND",
		"UNCOND",
		"IND",
		"CALL",
		"IND_CALL",
		"RET",
		"SYSCALL",
		"SYSRET",
		"COND_CALL",
		"COND_RET"
	};

	if (type >= 0 && type < PERF_BR_MAX)
		return branch_names[type];

	return NULL;
}

void branch_type_stat_display(FILE *fp, struct branch_type_stat *stat)
{
	u64 total = 0;
	int i;

	for (i = 0; i < PERF_BR_MAX; i++)
		total += stat->counts[i];

	if (total == 0)
		return;

	fprintf(fp, "\n#");
	fprintf(fp, "\n# Branch Statistics:");
	fprintf(fp, "\n#");

	if (stat->cond_fwd > 0) {
		fprintf(fp, "\n%8s: %5.1f%%",
			"COND_FWD",
			100.0 * (double)stat->cond_fwd / (double)total);
	}

	if (stat->cond_bwd > 0) {
		fprintf(fp, "\n%8s: %5.1f%%",
			"COND_BWD",
			100.0 * (double)stat->cond_bwd / (double)total);
	}

	if (stat->cross_4k > 0) {
		fprintf(fp, "\n%8s: %5.1f%%",
			"CROSS_4K",
			100.0 * (double)stat->cross_4k / (double)total);
	}

	if (stat->cross_2m > 0) {
		fprintf(fp, "\n%8s: %5.1f%%",
			"CROSS_2M",
			100.0 * (double)stat->cross_2m / (double)total);
	}

	for (i = 0; i < PERF_BR_MAX; i++) {
		if (stat->counts[i] > 0)
			fprintf(fp, "\n%8s: %5.1f%%",
				branch_type_name(i),
				100.0 *
				(double)stat->counts[i] / (double)total);
	}
}

static int count_str_printf(int index, const char *str,
	char *bf, int bfsize)
{
	int printed;

	printed = scnprintf(bf, bfsize,
		"%s%s",
		(index) ? " " : " (", str);

	return printed;
}

int branch_type_str(struct branch_type_stat *stat,
		    char *bf, int bfsize)
{
	int i, j = 0, printed = 0;
	u64 total = 0;

	for (i = 0; i < PERF_BR_MAX; i++)
		total += stat->counts[i];

	if (total == 0)
		return 0;

	if (stat->cond_fwd > 0) {
		printed += count_str_printf(j++, "COND_FWD",
				bf + printed, bfsize - printed);
	}

	if (stat->cond_bwd > 0) {
		printed += count_str_printf(j++, "COND_BWD",
				bf + printed, bfsize - printed);
	}

	for (i = 0; i < PERF_BR_MAX; i++) {
		if (i == PERF_BR_COND)
			continue;

		if (stat->counts[i] > 0) {
			printed += count_str_printf(j++, branch_type_name(i),
					bf + printed, bfsize - printed);
		}
	}

	if (stat->cross_4k > 0) {
		printed += count_str_printf(j++, "CROSS_4K",
				bf + printed, bfsize - printed);
	}

	if (stat->cross_2m > 0) {
		printed += count_str_printf(j++, "CROSS_2M",
				bf + printed, bfsize - printed);
	}

	return printed;
}
