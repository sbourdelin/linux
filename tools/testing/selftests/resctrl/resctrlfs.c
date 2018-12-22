// SPDX-License-Identifier: GPL-2.0
/*
 * Basic resctrl file system operations
 *
 * Copyright (C) 2018 Intel Corporation
 *
 * Authors:
 *    Arshiya Hayatkhan Pathan <arshiya.hayatkhan.pathan@intel.com>
 *    Sai Praneeth Prakhya <sai.praneeth.prakhya@intel.com>,
 *    Fenghua Yu <fenghua.yu@intel.com>
 */
#include "resctrl.h"

#define RESCTRL_MBM		"L3 monitoring detected"
#define RESCTRL_MBA		"MB allocation detected"
#define RESCTRL_CQM		"L3 monitoring detected"
#define RESCTRL_L3_CAT		"L3 allocation detected"
#define MAX_RESCTRL_FEATURES	4
#define CORE_SIBLINGS_PATH	"/sys/bus/cpu/devices/cpu"

char cbm_mask[256];

/*
 * remount_resctrlfs - Remount resctrl FS at /sys/fs/resctrl
 * @mum_resctrlfs:	Should the resctrl FS be remounted?
 *
 * If not mounted, mount it.
 * If mounted and mum_resctrlfs then remount resctrl FS.
 * If mounted and !mum_resctrlfs then noop
 *
 * Return: 0 on success, non-zero on failure
 */
int remount_resctrlfs(bool mum_resctrlfs)
{
	DIR *dp;
	struct dirent *ep;
	unsigned int count = 0;

	/*
	 * If kernel is built with CONFIG_RESCTRL, then /sys/fs/resctrl should
	 * be present by default
	 */
	dp = opendir(RESCTRL_PATH);
	if (dp) {
		while ((ep = readdir(dp)) != NULL)
			count++;
		closedir(dp);
	} else {
		perror("Unable to read /sys/fs/resctrl");

		return -1;
	}

	/*
	 * If resctrl FS has more than two entries, it means that resctrl FS has
	 * already been mounted. The two default entries are "." and "..", these
	 * are present even when resctrl FS is not mounted
	 */
	if (count > 2) {
		if (mum_resctrlfs) {
			if (umount(RESCTRL_PATH) != 0) {
				perror("Unable to umount resctrl");

				return errno;
			}
			printf("umount: done!\n");
		} else {
			printf("Mounted already. Not remounting!\n");

			return 0;
		}
	}

	if (mount("resctrl", RESCTRL_PATH, "resctrl", 0, NULL) != 0) {
		perror("Unable to mount resctrl FS at /sys/fs/resctrl");

		return errno;
	}

	return 0;
}

int umount_resctrlfs(void)
{
	if (umount(RESCTRL_PATH)) {
		perror("Unable to umount resctrl");

		return errno;
	}

	return 0;
}

/*
 * get_sock_num - Get socket number for a specified CPU
 * @cpu_no:	CPU number
 * @sock_num:	pointer to socket_number
 *
 * Return: >= 0 on success, < 0 on failure.
 */
int get_sock_num(int cpu_no, char *sock_num)
{
	char phys_pkg_path[1024];
	FILE *fp;

	sprintf(phys_pkg_path, "%s%d/topology/physical_package_id",
		PHYS_ID_PATH, cpu_no);
	fp = fopen(phys_pkg_path, "r");
	if (!fp) {
		perror("Failed to open physical_package_id");

		return -1;
	}
	if (fscanf(fp, "%c", sock_num) <= 0) {
		perror("Could not get socket number");
		fclose(fp);

		return -1;
	}
	fclose(fp);

	return 0;
}

/*
 * get_cache_size - Get cache size for a specified CPU
 * @cpu_no:	CPU number
 * @cache_num:	Cache level 2 for L2, 3 for L3
 * @cache_size:	pointer to cache_size
 *
 * Return: = 0 on success, < 0 on failure.
 */
int get_cache_size(int cpu_no, int cache_num, unsigned long *cache_size)
{
	char cache_path[1024], cache_str[64];
	FILE *fp;
	int length, i;

	sprintf(cache_path, "/sys/bus/cpu/devices/cpu%d/cache/index%d/size",
		cpu_no, cache_num);
	fp = fopen(cache_path, "r");
	if (!fp) {
		perror("Failed to open cache size");

		return -1;
	}
	if (fscanf(fp, "%s", cache_str) <= 0) {
		perror("Could not get cache_size");
		fclose(fp);

		return -1;
	}
	fclose(fp);

	length = (int)strlen(cache_str);

	*cache_size = 0;

	for (i = 0; i < length; i++) {
		if ((cache_str[i] >= '0') && (cache_str[i] <= '9'))

			*cache_size = *cache_size * 10 + (cache_str[i] - '0');

		else if (cache_str[i] == 'K')

			*cache_size = *cache_size * 1024;

		else if (cache_str[i] == 'M')

			*cache_size = *cache_size * 1024 * 1024;

		else
			break;
	}

	return 0;
}

/*
 * get_cbm_mask - Get cbm mask for given cache
 * @cache_type:	Cache level L2/L3
 *
 * Mask is stored in cbm_mask which is global variable.
 *
 * Return: = 0 on success, < 0 on failure.
 */
int get_cbm_mask(char *cache_type)
{
	char cbm_mask_path[1024];
	FILE *fp;

	sprintf(cbm_mask_path, "%s/%s/cbm_mask", CBM_MASK_PATH, cache_type);

	fp = fopen(cbm_mask_path, "r");
	if (!fp) {
		perror("Failed to open cache level");

		return -1;
	}
	if (fscanf(fp, "%s", cbm_mask) <= 0) {
		perror("Could not get max cbm_mask");
		fclose(fp);

		return -1;
	}
	fclose(fp);

	return 0;
}

/*
 * get_core_sibling - Get sibling core id from the same socket for given CPU
 * @cpu_no:	CPU number
 *
 * Return:	> 0 on success, < 0 on failure.
 */
int get_core_sibling(int cpu_no)
{
	char core_siblings_path[1024], cpu_list_str[64];
	int sibling_core_id = -1;
	FILE *fp;

	sprintf(core_siblings_path, "%s%d/topology/core_siblings_list",
		CORE_SIBLINGS_PATH, cpu_no);

	fp = fopen(core_siblings_path, "r");
	if (!fp) {
		perror("Failed to open core siblings path");

		return -1;
	}
	if (fscanf(fp, "%s", cpu_list_str) <= 0) {
		perror("Could not get core_siblings list");
		fclose(fp);

		return -1;
	}
	fclose(fp);

	char *token = strtok(cpu_list_str, "-,");

	while (token) {
		sibling_core_id = atoi(token);
		/* Skipping core 0 as we don't want to run test on core 0 */
		if (sibling_core_id != 0)
			break;
		token = strtok(NULL, "-,");
	}

	return sibling_core_id;
}

/*
 * taskset_benchmark - Taskset PID (i.e. benchmark) to a specified cpu
 * @bm_pid:	PID that should be binded
 * @cpu_no:	CPU number at which the PID would be binded
 *
 * Return: 0 on success, non-zero on failure
 */
int taskset_benchmark(pid_t bm_pid, int cpu_no)
{
	cpu_set_t my_set;

	CPU_ZERO(&my_set);
	CPU_SET(cpu_no, &my_set);

	if (sched_setaffinity(bm_pid, sizeof(cpu_set_t), &my_set)) {
		perror("Unable to taskset benchmark");

		return -1;
	}

	printf("Taskset benchmark: done!\n");

	return 0;
}

/*
 * run_benchmark - Run a specified benchmark or fill_buf (default benchmark)
 *		   in specified signal. Direct benchmark stdio to /dev/null.
 * @signum:	signal number
 * @info:	signal info
 * @ucontext:	user context in signal handling
 *
 * Return: void
 */
void run_benchmark(int signum, siginfo_t *info, void *ucontext)
{
	unsigned long long buffer_span;
	int span, operation, ret, malloc_and_init_memory, memflush;
	char **benchmark_cmd;
	char resctrl_val[64];
	FILE *fp;

	benchmark_cmd = info->si_ptr;

	/*
	 * Direct stdio of child to /dev/null, so that only parent writes to
	 * stdio (console)
	 */
	fp = freopen("/dev/null", "w", stdout);
	if (!fp)
		PARENT_EXIT("Unable to direct benchmark status to /dev/null");

	if (strcmp(benchmark_cmd[0], "fill_buf") == 0) {
		/* Execute default fill_buf benchmark */
		span = atoi(benchmark_cmd[1]);
		malloc_and_init_memory = atoi(benchmark_cmd[2]);
		memflush =  atoi(benchmark_cmd[3]);
		operation = atoi(benchmark_cmd[4]);
		sprintf(resctrl_val, "%s", benchmark_cmd[5]);

		if (strcmp(resctrl_val, "cqm") != 0)
			buffer_span = span * MB;
		else
			buffer_span = span;

		if (run_fill_buf(buffer_span, malloc_and_init_memory, memflush,
				 operation, resctrl_val))
			fprintf(stderr, "Error in running fill buffer\n");
	} else {
		/* Execute specified benchmark */
		ret = execvp(benchmark_cmd[0], benchmark_cmd);
		if (ret)
			perror("wrong\n");
	}

	fclose(stdout);
	PARENT_EXIT("Unable to run specified benchmark");
}

/*
 * create_grp - Create a group only if one doesn't exist
 * @grp_name:	Name of the group
 * @grp:	Full path and name of the group
 * @parent_grp:	Full path and name of the parent group
 *
 * Return: 0 on success, non-zero on failure
 */
static int create_grp(const char *grp_name, char *grp, const char *parent_grp)
{
	int found_grp = 0;
	struct dirent *ep;
	DIR *dp;

	/*
	 * At this point, we are guaranteed to have resctrl FS mounted and if
	 * length of grp_name == 0, it means, user wants to use root con_mon
	 * grp, so do nothing
	 */
	if (strlen(grp_name) == 0)
		return 0;

	/* Check if requested grp exists or not */
	dp = opendir(parent_grp);
	if (dp) {
		while ((ep = readdir(dp)) != NULL) {
			if (strcmp(ep->d_name, grp_name) == 0)
				found_grp = 1;
		}
		closedir(dp);
	} else {
		perror("Unable to open resctrl for group");

		return -1;
	}

	/* Requested grp doesn't exist, hence create it */
	if (found_grp == 0) {
		if (mkdir(grp, 0) == -1) {
			perror("Unable to create group");

			return -1;
		}
	}

	return 0;
}

static int write_pid_to_tasks(char *tasks, pid_t pid)
{
	FILE *fp;

	fp = fopen(tasks, "w");
	if (!fp) {
		perror("Failed to open tasks file");

		return -1;
	}
	if (fprintf(fp, "%d\n", pid) < 0) {
		perror("Failed to wr pid to tasks file");
		fclose(fp);

		return -1;
	}
	fclose(fp);

	return 0;
}

/*
 * write_bm_pid_to_resctrl - Write a PID (i.e. benchmark) to resctrl FS
 * @bm_pid:		PID that should be written
 * @ctrlgrp:		Name of the control monitor group (con_mon grp)
 * @mongrp:		Name of the monitor group (mon grp)
 * @resctrl_val:	Resctrl feature (Eg: mbm, mba.. etc)
 *
 * If a con_mon grp is requested, create it and write pid to it, otherwise
 * write pid to root con_mon grp.
 * If a mon grp is requested, create it and write pid to it, otherwise
 * pid is not written, this means that pid is in con_mon grp and hence
 * should consult con_mon grp's mon_data directory for results.
 *
 * Return: 0 on success, non-zero on failure
 */
int write_bm_pid_to_resctrl(pid_t bm_pid, char *ctrlgrp, char *mongrp,
			    char *resctrl_val)
{
	char controlgroup[128], monitorgroup[512], monitorgroup_p[256];
	char tasks[1024];
	int ret;

	if (strlen(ctrlgrp))
		sprintf(controlgroup, "%s/%s", RESCTRL_PATH, ctrlgrp);
	else
		sprintf(controlgroup, "%s", RESCTRL_PATH);

	/* Create control and monitoring group and write pid into it */
	ret = create_grp(ctrlgrp, controlgroup, RESCTRL_PATH);
	if (ret)
		return ret;
	sprintf(tasks, "%s/tasks", controlgroup);
	ret = write_pid_to_tasks(tasks, bm_pid);
	if (ret)
		return ret;

	/* Create mon grp and write pid into it for "mbm" and "cqm" test */
	if ((strcmp(resctrl_val, "cqm") == 0) ||
	    (strcmp(resctrl_val, "mbm") == 0)) {
		if (strlen(mongrp)) {
			sprintf(monitorgroup_p, "%s/mon_groups", controlgroup);
			sprintf(monitorgroup, "%s/%s", monitorgroup_p, mongrp);
			ret = create_grp(mongrp, monitorgroup, monitorgroup_p);
			if (ret)
				return ret;

			sprintf(tasks, "%s/mon_groups/%s/tasks",
				controlgroup, mongrp);
			ret = write_pid_to_tasks(tasks, bm_pid);
			if (ret)
				return ret;
		}
	}

	printf("Write benchmark to resctrl FS: done!\n");

	return 0;
}

/*
 * write_schemata - Update schemata of a con_mon grp
 * @ctrlgrp:		Name of the con_mon grp
 * @schemata:		Schemata that should be updated to
 * @cpu_no:		CPU number that the benchmark PID is binded to
 * @resctrl_val:	Resctrl feature (Eg: mbm, mba.. etc)
 *
 * Update schemata of a con_mon grp *only* if requested resctrl feature is
 * allocation type
 *
 * Return: 0 on success, non-zero on failure
 */
int write_schemata(char *ctrlgrp, char *schemata, int cpu_no, char *resctrl_val)
{
	char sock_num, controlgroup[1024], schema[1024];
	FILE *fp;
	int ret;

	if ((strcmp(resctrl_val, "mba") == 0) ||
	    (strcmp(resctrl_val, "cat") == 0) ||
	    (strcmp(resctrl_val, "cqm") == 0)) {
		if (!schemata) {
			printf("Schemata empty, so not updating\n");

			return 0;
		}
		ret = get_sock_num(cpu_no, &sock_num);
		if (ret < 0)
			return -1;

		if (strlen(ctrlgrp) != 0)
			sprintf(controlgroup, "%s/%s/schemata", RESCTRL_PATH,
				ctrlgrp);
		else
			sprintf(controlgroup, "%s/schemata", RESCTRL_PATH);

		if (!strcmp(resctrl_val, "cat") || !strcmp(resctrl_val, "cqm"))
			sprintf(schema, "%s%c%c%s", "L3:", sock_num, '=',
				schemata);
		if (strcmp(resctrl_val, "mba") == 0)
			sprintf(schema, "%s%c%c%s", "MB:", sock_num, '=',
				schemata);

		fp = fopen(controlgroup, "w");
		if (!fp) {
			perror("Failed to open control group");

			return -1;
		}

		if (fprintf(fp, "%s\n", schema) < 0) {
			perror("Failed to write schemata in control group");
			fclose(fp);

			return -1;
		}
		fclose(fp);

		printf("Write schemata with %s to resctrl FS: done!\n", schema);
	}

	return 0;
}

/*
 * validate_resctrl_feature_request - Check if requested feature is valid.
 * @resctrl_val:	Requested feature
 *
 * Return: 0 on success, non-zero on failure
 */
int validate_resctrl_feature_request(char *resctrl_val)
{
	int resctrl_features_supported[MAX_RESCTRL_FEATURES] = {0, 0, 0, 0};
	const char *resctrl_features_list[MAX_RESCTRL_FEATURES] = {
			"mbm", "mba", "cat", "cqm"};
	int i, valid_resctrl_feature = -1;
	char line[1024];
	FILE *fp;

	if (!resctrl_val) {
		fprintf(stderr, "resctrl feature cannot be NULL\n");

		return -1;
	}

	/* Is the resctrl feature request valid? */
	for (i = 0; i < MAX_RESCTRL_FEATURES; i++) {
		if (strcmp(resctrl_features_list[i], resctrl_val) == 0)
			valid_resctrl_feature = i;
	}
	if (valid_resctrl_feature == -1) {
		fprintf(stderr, "Not a valid resctrl feature request\n");

		return -1;
	}

	/* Enumerate resctrl features supported by this platform */
	if (system("dmesg > dmesg") != 0) {
		perror("Could not create custom dmesg file");

		return -1;
	}

	fp = fopen("dmesg", "r");
	if (!fp) {
		perror("Could not read custom created dmesg");

		return -1;
	}

	while (fgets(line, 1024, fp)) {
		if ((strstr(line, RESCTRL_MBM)) != NULL)
			resctrl_features_supported[0] = 1;
		if ((strstr(line, RESCTRL_MBA)) != NULL)
			resctrl_features_supported[1] = 1;
		if ((strstr(line, RESCTRL_L3_CAT)) != NULL)
			resctrl_features_supported[2] = 1;
		if ((strstr(line, RESCTRL_CQM)) != NULL)
			resctrl_features_supported[3] = 1;
	}
	fclose(fp);

	if (system("rm -rf dmesg") != 0)
		perror("Unable to remove 'dmesg' file");

	/* Is the resctrl feature request supported? */
	if (!resctrl_features_supported[valid_resctrl_feature]) {
		fprintf(stderr, "resctrl feature not supported!");

		return -1;
	}

	return 0;
}

int validate_bw_report_request(char *bw_report)
{
	if (strcmp(bw_report, "reads") == 0)
		return 0;
	if (strcmp(bw_report, "writes") == 0)
		return 0;
	if (strcmp(bw_report, "nt-writes") == 0) {
		strcpy(bw_report, "writes");
		return 0;
	}
	if (strcmp(bw_report, "total") == 0)
		return 0;

	fprintf(stderr, "Requested iMC B/W report type unavailable\n");

	return -1;
}

int perf_event_open(struct perf_event_attr *hw_event, pid_t pid, int cpu,
		    int group_fd, unsigned long flags)
{
	int ret;

	ret = syscall(__NR_perf_event_open, hw_event, pid, cpu,
		      group_fd, flags);
	return ret;
}

void ctrlc_handler(int signum, siginfo_t *info, void *ptr)
{
	kill(bm_pid, SIGKILL);
	umount_resctrlfs();
	tests_cleanup();
	printf("Ending\n\n");

	exit(EXIT_SUCCESS);
}

unsigned int count_bits(unsigned long n)
{
	unsigned int count = 0;

	while (n) {
		count += n & 1;
		n >>= 1;
	}

	return count;
}
