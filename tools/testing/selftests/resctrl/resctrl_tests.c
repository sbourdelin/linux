// SPDX-License-Identifier: GPL-2.0
/*
 * Resctrl tests
 *
 * Copyright (C) 2018 Intel Corporation
 *
 * Authors:
 *    Arshiya Hayatkhan Pathan <arshiya.hayatkhan.pathan@intel.com>
 *    Sai Praneeth Prakhya <sai.praneeth.prakhya@intel.com>,
 *    Fenghua Yu <fenghua.yu@intel.com>
 */
#include "resctrl.h"

#define BENCHMARK_ARGS		64
#define BENCHMARK_ARG_SIZE	64

static void cmd_help(void)
{
	printf("usage: resctrl_tests [-h] [-b \"benchmark_cmd [options]\"] [-t test list] [-n no_of_bits]\n");
	printf("\t-b benchmark_cmd [options]: run specified benchmark for MBM, MBA and CQM");
	printf("\t default benchmark is builtin fill_buf\n");
	printf("\t-t test list: run tests specified in the test list, ");
	printf("e.g. -t mbm, mba, cqm, cat\n");
	printf("\t-n no_of_bits: run cache tests using specified no of bits in cache bit mask\n");
	printf("\t-h: help\n");
}

void tests_cleanup(void)
{
	mbm_test_cleanup();
	mba_test_cleanup();
	cqm_test_cleanup();
	cat_test_cleanup();
}

int main(int argc, char **argv)
{
	char benchmark_cmd_area[BENCHMARK_ARGS][BENCHMARK_ARG_SIZE];
	int res, c, core_id = 1, span = 250, argc_new = argc, i, no_of_bits = 5;
	int ben_count, ben_ind;
	bool has_ben = false, mbm_test = true, mba_test = true;
	bool cqm_test = true, cat_test = true;
	char *benchmark_cmd[BENCHMARK_ARGS];
	char bw_report[64], bm_type[64];

	for (int i = 0; i < argc; i++) {
		if (strcmp(argv[i], "-b") == 0) {
			ben_ind = i + 1;
			ben_count = argc - ben_ind;
			argc_new = ben_ind - 1;
			has_ben = 1;
			break;
		}
	}

	while ((c = getopt(argc_new, argv, "ht:n:p:")) != -1) {
		char *token;

		switch (c) {
		case 't':
			token = strtok(optarg, ",");

			mbm_test = false;
			mba_test = false;
			cqm_test = false;
			cat_test = false;
			while (token) {
				if (!strcmp(token, "mbm")) {
					mbm_test = true;
				} else if (!strcmp(token, "mba")) {
					mba_test = true;
				} else if (!strcmp(token, "cqm")) {
					cqm_test = true;
				} else if (!strcmp(token, "cat")) {
					cat_test = true;
				} else {
					printf("invalid argument\n");

					return -1;
				}
				token = strtok(NULL, ":\t");
			}
			break;
		case 'n':
			no_of_bits = atoi(optarg);
			break;
		case 'p':
			core_id = atoi(optarg);
			break;
		case 'h':
			cmd_help();

			return 0;
		default:
			printf("invalid argument\n");

			return -1;
		}
	}

	/*
	 * We need root privileges to run because
	 * 1. We write to resctrl FS
	 * 2. We execute perf commands
	 */
	if (geteuid() != 0) {
		perror("Please run this program as root\n");

		return errno;
	}

	if (has_ben) {
		/* Extract benchmark command from command line. */
		for (int i = ben_ind; i < argc; i++) {
			benchmark_cmd[i - ben_ind] = benchmark_cmd_area[i];
			sprintf(benchmark_cmd[i - ben_ind], "%s", argv[i]);
		}
		benchmark_cmd[ben_count] = benchmark_cmd_area[ben_count];
		sprintf(benchmark_cmd[ben_count], "%s", "NULL");
	} else {
		/* If no benchmark is given by "-b" argument, use fill_buf. */
		for (i = 0; i < 6; i++)
			benchmark_cmd[i] = benchmark_cmd_area[i];

		strcpy(benchmark_cmd[0], "fill_buf");
		sprintf(benchmark_cmd[1], "%d", span);
		strcpy(benchmark_cmd[2], "1");
		strcpy(benchmark_cmd[3], "1");
		strcpy(benchmark_cmd[4], "0");
		strcpy(benchmark_cmd[5], "");
		benchmark_cmd[6] = NULL;
	}

	sprintf(bw_report, "reads");
	sprintf(bm_type, "fill_buf");

	if (mbm_test) {
		printf("\nMBM BW Change Starting..\n");
		if (!has_ben)
			sprintf(benchmark_cmd[5], "%s", "mbm");
		res = mbm_bw_change(span, core_id, bw_report, benchmark_cmd);
		if (res)
			printf("Error in running tests for mbm bw change!\n");
		mbm_test_cleanup();
	}

	if (mba_test) {
		printf("\nMBA Schemata Change Starting..\n");
		if (!has_ben)
			sprintf(benchmark_cmd[5], "%s", "mba");
		res = mba_schemata_change(core_id, bw_report, benchmark_cmd);
		if (res)
			printf("Error in tests for mba-change-schemata!\n");
		mba_test_cleanup();
	}

	if (cqm_test) {
		printf("\nCQM Test Starting..\n");
		if (!has_ben)
			sprintf(benchmark_cmd[5], "%s", "cqm");
		res = cqm_resctrl_val(core_id, no_of_bits, benchmark_cmd);
		if (res)
			printf("Error in CQM test!\n");
		cqm_test_cleanup();
	}
	if (cat_test) {
		printf("\nCAT Test Starting..\n");
		res = cat_perf_miss_val(core_id, no_of_bits);
		if (res)
			printf("Error in CAT test!\n");
		cat_test_cleanup();
	}

	return 0;
}
