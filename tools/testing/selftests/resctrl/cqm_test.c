// SPDX-License-Identifier: GPL-2.0
/*
 * Cache Monitoring Technology (CQM) test
 *
 * Copyright (C) 2018 Intel Corporation
 *
 * Authors:
 *    Arshiya Hayatkhan Pathan <arshiya.hayatkhan.pathan@intel.com>
 *    Sai Praneeth Prakhya <sai.praneeth.prakhya@intel.com>,
 *    Fenghua Yu <fenghua.yu@intel.com>
 */
#include "resctrl.h"
#include <unistd.h>

#define RESULT_FILE_NAME	"result_cqm"
#define NUM_OF_RUNS		5
#define MAX_DIFF		2000000
#define MAX_DIFF_PERCENT	15

int count_of_bits;
char cbm_mask[256];
unsigned long long_mask;
unsigned long cache_size;

static int cqm_setup(int num, ...)
{
	struct resctrl_val_param *p;
	va_list param;

	va_start(param, num);
	p = va_arg(param, struct resctrl_val_param *);
	va_end(param);

	/* Run NUM_OF_RUNS times */
	if (p->num_of_runs >= NUM_OF_RUNS)
		return -1;

	p->num_of_runs++;

	return 0;
}

static void show_cache_info(unsigned long sum_llc_occu_resc, int no_of_bits,
			    unsigned long span)
{
	unsigned long avg_llc_occu_resc = 0;
	long avg_diff = 0;
	float diff_percent;

	avg_llc_occu_resc = sum_llc_occu_resc / (NUM_OF_RUNS - 1);
	avg_diff = (long long)abs(span - avg_llc_occu_resc);

	printf("\nResults are displayed in (Bytes)\n");
	printf("\nNumber of bits: %d \t", no_of_bits);
	printf("Avg_llc_occu_resc: %lu \t", avg_llc_occu_resc);
	printf("llc_occu_exp (span): %lu \t", span);

	diff_percent = (((float)span - avg_llc_occu_resc) / span) * 100;

	printf("Diff: %ld \t", avg_diff);
	printf("Percent diff=%d\t", abs((int)diff_percent));

	if ((abs((int)diff_percent) <= MAX_DIFF_PERCENT) ||
	    (abs(avg_diff) <= MAX_DIFF))
		printf("Passed\n");
	else
		printf("Failed\n");
}

static int check_results(struct resctrl_val_param *param, int no_of_bits)
{
	char *token_array[8], temp[512];
	unsigned long sum_llc_occu_resc = 0;
	int runs = 0;
	FILE *fp;

	printf("\nchecking for pass/fail\n");
	fp = fopen(param->filename, "r");
	if (!fp) {
		perror("Error in opening file\n");

		return errno;
	}

	while (fgets(temp, 1024, fp)) {
		char *token = strtok(temp, ":\t");
		int fields = 0;

		while (token) {
			token_array[fields++] = token;
			token = strtok(NULL, ":\t");
		}

		/* Field 3 is llc occ resc value */
		if (runs > 0)
			sum_llc_occu_resc += atol(token_array[3]);
		runs++;
	}
	fclose(fp);
	show_cache_info(sum_llc_occu_resc, no_of_bits, param->span);

	return 0;
}

void cqm_test_cleanup(void)
{
	remove(RESULT_FILE_NAME);
}

int cqm_resctrl_val(int core_id, int n, char **benchmark_cmd)
{
	int ret, mum_resctrlfs;

	cache_size = 0;
	mum_resctrlfs = 1;

	ret = remount_resctrlfs(mum_resctrlfs);
	if (ret)
		return ret;

	ret = get_cbm_mask("L3");
	if (ret)
		return ret;

	long_mask = strtoul(cbm_mask, NULL, 16);

	ret = get_cache_size(core_id, 3, &cache_size);
	if (ret)
		return ret;
	printf("cache size :%lu\n", cache_size);

	count_of_bits = count_bits(long_mask);

	if (n < 1 || n > count_of_bits) {
		printf("Invalid input value for numbr_of_bits n!\n");
		printf("Please Enter value in range 1 to %d\n", count_of_bits);
		return -1;
	}

	struct resctrl_val_param param = {
		.resctrl_val	= "cqm",
		.ctrlgrp	= "c1",
		.mongrp		= "m1",
		.cpu_no		= core_id,
		.mum_resctrlfs	= 0,
		.filename	= RESULT_FILE_NAME,
		.mask		= ~(long_mask << n) & long_mask,
		.span		= cache_size * n / count_of_bits,
		.num_of_runs	= 0,
		.setup		= cqm_setup,
	};

	if (strcmp(benchmark_cmd[0], "fill_buf") == 0)
		sprintf(benchmark_cmd[1], "%llu", param.span);

	remove(RESULT_FILE_NAME);

	ret = resctrl_val(benchmark_cmd, &param);
	if (ret)
		return ret;

	ret = check_results(&param, n);
	if (ret)
		return ret;

	cqm_test_cleanup();

	return 0;
}
