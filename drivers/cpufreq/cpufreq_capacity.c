/*
 * Default CPU capacity calculation for u-arch invariance
 *
 * Copyright (C) 2015 ARM Ltd.
 * Juri Lelli <juri.lelli@arm.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed "as is" WITHOUT ANY WARRANTY of any
 * kind, whether express or implied; without even the implied warranty
 * of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */
#include <linux/cpufreq.h>
#include <linux/sched.h>

static unsigned long long elapsed[NR_CPUS];

/*
 * Don't let compiler optimize following two functions;
 * otherwise we might loose u-arch differences.
 * Also, my_int_sqrt is cut-and-paste from lib/int_sqrt.c.
 */
static unsigned long __attribute__((optimize("O0")))
my_int_sqrt(unsigned long x)
{
	unsigned long b, m, y = 0;

	if (x <= 1)
		return x;

	m = 1UL << (BITS_PER_LONG - 2);
	while (m != 0) {
		b = y + m;
		y >>= 1;

		if (x >= b) {
			x -= b;
			y += m;
		}
		m >>= 2;
	}

	return y;
}

static unsigned long __attribute__((optimize("O0")))
bogus_bench(void)
{
	unsigned long i, res;

	for (i = 0; i < 100000; i++)
		res = my_int_sqrt(i);

	return res;
}

static int run_bogus_benchmark(int cpu)
{
	int ret, trials = 25;
	u64 begin, end, diff, diff_avg = 0, count = 0;
	unsigned long res;

	ret = set_cpus_allowed_ptr(current, cpumask_of(cpu));
	if (ret) {
		pr_warn("%s: failed to set allowed ptr\n", __func__);
		return -EINVAL;
	}

	while (trials--) {
		begin = local_clock();
		res = bogus_bench();
		end = local_clock();
		diff = end - begin;
		diff_avg = diff_avg * count + diff;
		diff_avg = div64_u64(diff_avg, ++count);
		pr_debug("%s: cpu=%d begin=%llu end=%llu"
			 " diff=%llu diff_avg=%llu count=%llu res=%lu\n",
			__func__, cpu, begin, end, diff,
			diff_avg, count, res);
	}
	elapsed[cpu] = diff_avg;

	ret = set_cpus_allowed_ptr(current, cpu_active_mask);
	if (ret) {
		pr_warn("%s: failed to set allowed ptr\n", __func__);
		return -EINVAL;
	}

	return 0;
}

bool __weak arch_wants_init_cpu_capacity(void)
{
	return false;
}

void __weak set_capacity_scale(int cpu, unsigned long capacity) { }

void cpufreq_init_cpu_capacity(void)
{
	int cpu, fcpu;
	unsigned long long elapsed_min = ULLONG_MAX;
	unsigned int curr_min, curr_max;
	struct cpufreq_policy *policy;

	if (!arch_wants_init_cpu_capacity())
		return;

	for_each_possible_cpu(cpu) {
		policy = cpufreq_cpu_get(cpu);
		if (IS_ERR_OR_NULL(policy))
			return;

		/*
		 * We profile only first CPU of each frequency domain;
		 * and use that value as capacity of every CPU in the domain.
		 */
		fcpu = cpumask_first(policy->related_cpus);
		if (cpu != fcpu) {
			elapsed[cpu] = elapsed[fcpu];
			cpufreq_cpu_put(policy);
			continue;
		}

		down_write(&policy->rwsem);
		curr_min = policy->user_policy.min;
		curr_max = policy->user_policy.max;
		policy->user_policy.min = policy->cpuinfo.max_freq;
		policy->user_policy.max = policy->cpuinfo.max_freq;
		up_write(&policy->rwsem);
		cpufreq_cpu_put(policy);
		cpufreq_update_policy(cpu);

		run_bogus_benchmark(cpu);
		if (elapsed[cpu] < elapsed_min)
			elapsed_min = elapsed[cpu];
		pr_debug("%s: cpu=%d elapsed=%llu (min=%llu)\n",
				__func__, cpu, elapsed[cpu], elapsed_min);

		policy = cpufreq_cpu_get(cpu);
		down_write(&policy->rwsem);
		policy->user_policy.min = curr_min;
		policy->user_policy.max = curr_max;
		up_write(&policy->rwsem);
		cpufreq_cpu_put(policy);
		cpufreq_update_policy(cpu);
	}

	for_each_possible_cpu(cpu) {
		unsigned long capacity;

		capacity = div64_u64((elapsed_min << 10), elapsed[cpu]);
		pr_debug("%s: CPU%d capacity=%lu\n", __func__, cpu, capacity);
		set_capacity_scale(cpu, capacity);
	}

	pr_info("dynamic CPUs capacity installed\n");
}
