#include "util/mem-events.h"
#include "util/symbol.h"
#include "linux/perf_event.h"
#include "util/debug.h"
#include "tests.h"
#include <string.h>

static int check(union perf_mem_data_src data_src,
		  const char *string)
{
	char out[100];
	char failure[100];
	struct mem_info mi = { .data_src = data_src };

	int n;

	n = perf_mem__snp_scnprintf(out, sizeof out, &mi);
	n += perf_mem__lvl_scnprintf(out + n, sizeof out - n, &mi);
	snprintf(failure, sizeof failure, "unexpected %s", out);
	TEST_ASSERT_VAL(failure, !strcmp(string, out));
	return 0;
}

int test__mem(int subtest __maybe_unused)
{
	int ret = 0;

	ret |= check(((union perf_mem_data_src) {
				.mem_lvl = PERF_MEM_LVL_HIT,
				.mem_lvlx = PERF_MEM_LVLX_L4 }), "N/AL4 hit");

	ret |= check(((union perf_mem_data_src) {
				.mem_lvl = PERF_MEM_LVL_MISS,
				.mem_lvlx = PERF_MEM_LVLX_PMEM }), "N/APMEM miss");

	ret |= check(((union perf_mem_data_src) {
				.mem_snoopx = PERF_MEM_SNOOPX_FWD,
				.mem_lvl = PERF_MEM_LVL_MISS,
				.mem_lvlx = PERF_MEM_LVLX_RAM }), "ForwardRAM miss");

	return ret;
}
