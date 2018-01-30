// SPDX-License-Identifier: GPL-2.0

#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <sched.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <linux/bpf.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/wait.h>

#include "libbpf.h"
#include "bpf_load.h"

#define MAX_CPU			8
#define MAX_PSTATE_ENTRIES	5
#define MAX_CSTATE_ENTRIES	3
#define MAX_STARS		40

#define CPUFREQ_MAX_SYSFS_PATH	"/sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq"
#define CPUFREQ_LOWEST_FREQ	"208000"
#define CPUFREQ_HIGHEST_FREQ	"12000000"

struct cpu_hist {
	unsigned long cstate[MAX_CSTATE_ENTRIES];
	unsigned long pstate[MAX_PSTATE_ENTRIES];
};

static struct cpu_hist cpu_hist[MAX_CPU];
static unsigned long max_data;

static void stars(char *str, long val, long max, int width)
{
	int i;

	for (i = 0; i < (width * val / max) - 1 && i < width - 1; i++)
		str[i] = '*';
	if (val > max)
		str[i - 1] = '+';
	str[i] = '\0';
}

static void print_hist(void)
{
	char starstr[MAX_STARS];
	struct cpu_hist *hist;
	int i, j;

	/* ignore without data */
	if (max_data == 0)
		return;

	/* clear screen */
	printf("\033[2J");

	for (j = 0; j < MAX_CPU; j++) {
		hist = &cpu_hist[j];

		printf("CPU %d\n", j);
		printf("State    : Duration(ms)  Distribution\n");
		for (i = 0; i < MAX_CSTATE_ENTRIES; i++) {
			stars(starstr, hist->cstate[i], max_data, MAX_STARS);
			printf("cstate %d : %-8ld     |%-*s|\n", i,
				hist->cstate[i] / 1000000, MAX_STARS, starstr);
		}

		for (i = 0; i < MAX_PSTATE_ENTRIES; i++) {
			stars(starstr, hist->pstate[i], max_data, MAX_STARS);
			printf("pstate %d : %-8ld     |%-*s|\n", i,
				hist->pstate[i] / 1000000, MAX_STARS, starstr);
		}

		printf("\n");
	}
}

static void get_data(int cstate_fd, int pstate_fd)
{
	unsigned long key, value;
	int c, i;

	max_data = 0;

	for (c = 0; c < MAX_CPU; c++) {
		for (i = 0; i < MAX_CSTATE_ENTRIES; i++) {
			key = c * MAX_CSTATE_ENTRIES + i;
			bpf_map_lookup_elem(cstate_fd, &key, &value);
			cpu_hist[c].cstate[i] = value;

			if (value > max_data)
				max_data = value;
		}

		for (i = 0; i < MAX_PSTATE_ENTRIES; i++) {
			key = c * MAX_PSTATE_ENTRIES + i;
			bpf_map_lookup_elem(pstate_fd, &key, &value);
			cpu_hist[c].pstate[i] = value;

			if (value > max_data)
				max_data = value;
		}
	}
}

/*
 * This function is copied from function idlestat_wake_all()
 * in idlestate.c, it set the self task affinity to cpus
 * one by one so can wake up the CPU to handle the scheduling;
 * as result all cpus can be waken up once and produce trace
 * event 'cpu_idle'.
 */
static int cpu_stat_inject_cpu_idle_event(void)
{
	int rcpu, i, ret;
	cpu_set_t cpumask;
	cpu_set_t original_cpumask;

	ret = sysconf(_SC_NPROCESSORS_CONF);
	if (ret < 0)
		return -1;

	rcpu = sched_getcpu();
	if (rcpu < 0)
		return -1;

	/* Keep track of the CPUs we will run on */
	sched_getaffinity(0, sizeof(original_cpumask), &original_cpumask);

	for (i = 0; i < ret; i++) {

		/* Pointless to wake up ourself */
		if (i == rcpu)
			continue;

		/* Pointless to wake CPUs we will not run on */
		if (!CPU_ISSET(i, &original_cpumask))
			continue;

		CPU_ZERO(&cpumask);
		CPU_SET(i, &cpumask);

		sched_setaffinity(0, sizeof(cpumask), &cpumask);
	}

	/* Enable all the CPUs of the original mask */
	sched_setaffinity(0, sizeof(original_cpumask), &original_cpumask);
	return 0;
}

/*
 * It's possible to have long time have no any frequency change
 * and cannot get trace event 'cpu_frequency' for long time, this
 * can introduce big deviation for pstate statistics.
 *
 * To solve this issue, we can force to set 'scaling_max_freq' to
 * trigger trace event 'cpu_frequency' and then we can recovery
 * back the maximum frequency value.  For this purpose, below
 * firstly set highest frequency to 208MHz and then recovery to
 * 1200MHz again.
 */
static int cpu_stat_inject_cpu_frequency_event(void)
{
	int len, fd;

	fd = open(CPUFREQ_MAX_SYSFS_PATH, O_WRONLY);
	if (fd < 0) {
		printf("failed to open scaling_max_freq, errno=%d\n", errno);
		return fd;
	}

	len = write(fd, CPUFREQ_LOWEST_FREQ, strlen(CPUFREQ_LOWEST_FREQ));
	if (len < 0) {
		printf("failed to open scaling_max_freq, errno=%d\n", errno);
		goto err;
	}

	len = write(fd, CPUFREQ_HIGHEST_FREQ, strlen(CPUFREQ_HIGHEST_FREQ));
	if (len < 0) {
		printf("failed to open scaling_max_freq, errno=%d\n", errno);
		goto err;
	}

err:
	close(fd);
	return len;
}

static void int_exit(int sig)
{
	cpu_stat_inject_cpu_idle_event();
	cpu_stat_inject_cpu_frequency_event();
	get_data(map_fd[1], map_fd[2]);
	print_hist();
	exit(0);
}

int main(int argc, char **argv)
{
	char filename[256];
	int ret;

	snprintf(filename, sizeof(filename), "%s_kern.o", argv[0]);

	if (load_bpf_file(filename)) {
		printf("%s", bpf_log_buf);
		return 1;
	}

	ret = cpu_stat_inject_cpu_idle_event();
	if (ret < 0)
		return 1;

	ret = cpu_stat_inject_cpu_frequency_event();
	if (ret < 0)
		return 1;

	signal(SIGINT, int_exit);
	signal(SIGTERM, int_exit);

	while (1) {
		get_data(map_fd[1], map_fd[2]);
		print_hist();
		sleep(5);
	}

	return 0;
}
