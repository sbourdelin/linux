/*
 * x86_energy_perf_policy -- set the energy versus performance
 * policy preference bias on recent X86 processors.
 */
/*
 * Copyright (c) 2010, Intel Corporation.
 * Len Brown <len.brown@intel.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St - Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/time.h>
#include <stdlib.h>
#include <string.h>

#include "../../utils/utils.h"

unsigned int verbose;		/* set with -v */
unsigned int read_only;		/* set with -r */
char *progname;
unsigned long long new_bias;
int cpu = -1;

/*
 * Usage:
 *
 * -c cpu: limit action to a single CPU (default is all CPUs)
 * -v: verbose output (can invoke more than once)
 * -r: read-only, don't change any settings
 *
 *  performance
 *	Performance is paramount.
 *	Unwilling to sacrifice any performance
 *	for the sake of energy saving. (hardware default)
 *
 *  normal
 *	Can tolerate minor performance compromise
 *	for potentially significant energy savings.
 *	(reasonable default for most desktops and servers)
 *
 *  powersave
 *	Can tolerate significant performance hit
 *	to maximize energy savings.
 *
 * n
 *	a numerical value to write to the underlying MSR.
 */
void usage(void)
{
	printf("%s: [-c cpu] [-v] "
		"(-r | 'performance' | 'normal' | 'powersave' | n)\n",
		progname);
	exit(1);
}

#define	BIAS_PERFORMANCE		0
#define BIAS_BALANCE			6
#define	BIAS_POWERSAVE			15

void cmdline(int argc, char **argv)
{
	int opt;

	progname = argv[0];

	while ((opt = getopt(argc, argv, "+rvc:")) != -1) {
		switch (opt) {
		case 'c':
			cpu = atoi(optarg);
			break;
		case 'r':
			read_only = 1;
			break;
		case 'v':
			verbose++;
			break;
		default:
			usage();
		}
	}
	/* if -r, then should be no additional optind */
	if (read_only && (argc > optind))
		usage();

	/*
	 * if no -r , then must be one additional optind
	 */
	if (!read_only) {

		if (argc != optind + 1) {
			printf("must supply -r or policy param\n");
			usage();
			}

		if (!strcmp("performance", argv[optind])) {
			new_bias = BIAS_PERFORMANCE;
		} else if (!strcmp("normal", argv[optind])) {
			new_bias = BIAS_BALANCE;
		} else if (!strcmp("powersave", argv[optind])) {
			new_bias = BIAS_POWERSAVE;
		} else {
			char *endptr;

			new_bias = strtoull(argv[optind], &endptr, 0);
			if (endptr == argv[optind] ||
				new_bias > BIAS_POWERSAVE) {
					fprintf(stderr, "invalid value: %s\n",
						argv[optind]);
				usage();
			}
		}
	}
}

/*
 * validate_cpuid()
 * returns on success, quietly exits on failure (make verbose with -v)
 */
void validate_cpuid(void)
{
	unsigned int eax, ebx, ecx, edx, max_level;
	unsigned int fms, family, model, stepping;

	eax = ebx = ecx = edx = 0;

	asm("cpuid" : "=a" (max_level), "=b" (ebx), "=c" (ecx),
		"=d" (edx) : "a" (0));

	if (ebx != 0x756e6547 || edx != 0x49656e69 || ecx != 0x6c65746e) {
		if (verbose)
			fprintf(stderr, "%.4s%.4s%.4s != GenuineIntel",
				(char *)&ebx, (char *)&edx, (char *)&ecx);
		exit(1);
	}

	asm("cpuid" : "=a" (fms), "=c" (ecx), "=d" (edx) : "a" (1) : "ebx");
	family = (fms >> 8) & 0xf;
	model = (fms >> 4) & 0xf;
	stepping = fms & 0xf;
	if (family == 6 || family == 0xf)
		model += ((fms >> 16) & 0xf) << 4;

	if (verbose > 1)
		printf("CPUID %d levels family:model:stepping "
			"0x%x:%x:%x (%d:%d:%d)\n", max_level,
			family, model, stepping, family, model, stepping);

	if (!(edx & (1 << 5))) {
		if (verbose)
			printf("CPUID: no MSR\n");
		exit(1);
	}

	/*
	 * Support for MSR_IA32_ENERGY_PERF_BIAS
	 * is indicated by CPUID.06H.ECX.bit3
	 */
	asm("cpuid" : "=a" (eax), "=b" (ebx), "=c" (ecx), "=d" (edx) : "a" (6));
	if (verbose)
		printf("CPUID.06H.ECX: 0x%x\n", ecx);
	if (!(ecx & (1 << 3))) {
		if (verbose)
			printf("CPUID: No MSR_IA32_ENERGY_PERF_BIAS\n");
		exit(1);
	}
	return;	/* success */
}

void print_bias(int cpu)
{
	unsigned int val;

	val = get_pref_hint(cpu);
	if (val < 0)
		return;

	printf("cpu%d: 0x%08x\n", cpu, val);
}

void update_msr(int cpu)
{
	unsigned int previous_val;

	previous_val = get_pref_hint(cpu);
	if (previous_val < 0)
		return;

	if (set_pref_hint(cpu, new_bias))
		return;

	if (verbose)
		printf("cpu%d  pref hint: 0x%08x -> 0x%08x\n",
			cpu, previous_val, new_bias);

	return;
}

char *proc_stat = "/proc/stat";
/*
 * run func() on every cpu in /dev/cpu
 */
void for_every_cpu(void (func)(int))
{
	FILE *fp;
	int retval;

	fp = fopen(proc_stat, "r");
	if (fp == NULL) {
		perror(proc_stat);
		exit(1);
	}

	retval = fscanf(fp, "cpu %*d %*d %*d %*d %*d %*d %*d %*d %*d %*d\n");
	if (retval != 0) {
		perror("/proc/stat format");
		exit(1);
	}

	while (1) {
		int cpu;

		retval = fscanf(fp,
			"cpu%u %*d %*d %*d %*d %*d %*d %*d %*d %*d %*d\n",
			&cpu);
		if (retval != 1)
			break;

		func(cpu);
	}
	fclose(fp);
}

int main(int argc, char **argv)
{
	cmdline(argc, argv);

	if (verbose > 1)
		printf("x86_energy_perf_policy Nov 24, 2010"
				" - Len Brown <lenb@kernel.org>\n");
	if (verbose > 1 && !read_only)
		printf("new_bias %lld\n", new_bias);

	validate_cpuid();

	if (cpu != -1) {
		if (read_only)
			print_bias(cpu);
		else
			update_msr(cpu);
	} else {
		if (read_only)
			for_every_cpu(print_bias);
		else
			for_every_cpu(update_msr);
	}

	return 0;
}
