// SPDX-License-Identifier: GPL-2.0
#include <linux/smp.h>
#include <linux/sort.h>
#include <linux/timex.h>
#include <linux/string.h>
#include <linux/seq_file.h>
#include <linux/spinlock.h>
#include <linux/cpufreq.h>

#include "cpu.h"

/*
 *	Get CPU information for use by the procfs.
 */
static void show_cpuinfo_core(struct seq_file *m, struct cpuinfo_x86 *c,
			      unsigned int cpu)
{
#ifdef CONFIG_SMP
	seq_printf(m, "physical id\t: %d\n", c->phys_proc_id);
	seq_printf(m, "siblings\t: %d\n",
		   cpumask_weight(topology_core_cpumask(cpu)));
	seq_printf(m, "core id\t\t: %d\n", c->cpu_core_id);
	seq_printf(m, "cpu cores\t: %d\n", c->booted_cores);
	seq_printf(m, "apicid\t\t: %d\n", c->apicid);
	seq_printf(m, "initial apicid\t: %d\n", c->initial_apicid);
#endif
}

#ifdef CONFIG_X86_32
static void show_cpuinfo_misc(struct seq_file *m, struct cpuinfo_x86 *c)
{
	seq_printf(m,
		   "fdiv_bug\t: %s\n"
		   "f00f_bug\t: %s\n"
		   "coma_bug\t: %s\n"
		   "fpu\t\t: %s\n"
		   "fpu_exception\t: %s\n"
		   "cpuid level\t: %d\n"
		   "wp\t\t: yes\n",
		   static_cpu_has_bug(X86_BUG_FDIV) ? "yes" : "no",
		   static_cpu_has_bug(X86_BUG_F00F) ? "yes" : "no",
		   static_cpu_has_bug(X86_BUG_COMA) ? "yes" : "no",
		   static_cpu_has(X86_FEATURE_FPU) ? "yes" : "no",
		   static_cpu_has(X86_FEATURE_FPU) ? "yes" : "no",
		   c->cpuid_level);
}
#else
static void show_cpuinfo_misc(struct seq_file *m, struct cpuinfo_x86 *c)
{
	seq_printf(m,
		   "fpu\t\t: yes\n"
		   "fpu_exception\t: yes\n"
		   "cpuid level\t: %d\n"
		   "wp\t\t: yes\n",
		   c->cpuid_level);
}
#endif

#define X86_NR_CAPS	(32*NCAPINTS)
/*
 * x86_cap_flags[] is an array of string pointers.  This
 * (x86_sorted_cap_flags[]) is an array of array indexes
 * *referring* to x86_cap_flags[] entries.  It is sorted
 * to make it quick to print a sorted list of cpu flags in
 * /proc/cpuinfo.
 */
static unsigned short x86_sorted_cap_flags[X86_NR_CAPS] = { -1, };
static int x86_cmp_cap(const void *a_ptr, const void *b_ptr)
{
	unsigned short a = *(unsigned short *)a_ptr;
	unsigned short b = *(unsigned short *)b_ptr;

	/* Don't need to swap equal entries (presumably NULLs) */
	if (x86_cap_flags[a] == x86_cap_flags[b])
		return 0;
	/* Put NULL elements at the end: */
	if (x86_cap_flags[a] == NULL)
		return -1;
	if (x86_cap_flags[b] == NULL)
		return 1;

	return strcmp(x86_cap_flags[a], x86_cap_flags[b]);
}

static void x86_sort_cap_flags(void)
{
	static DEFINE_SPINLOCK(lock);
	int i;

	/*
	 * It's possible that multiple threads could race
	 * to here and both sort the list.  The lock keeps
	 * them from trying to sort concurrently.
	 */
	spin_lock(&lock);

	/* Initialize the list with 0->i, removing the -1's: */
	for (i = 0; i < X86_NR_CAPS; i++)
		x86_sorted_cap_flags[i] = i;

	sort(x86_sorted_cap_flags, X86_NR_CAPS,
	     sizeof(x86_sorted_cap_flags[0]),
	     x86_cmp_cap, NULL);

	spin_unlock(&lock);
}

static void show_cpuinfo_flags(struct seq_file *m, struct cpuinfo_x86 *c)
{
	int i;

	if (x86_sorted_cap_flags[0] == (unsigned short)-1)
		x86_sort_cap_flags();

	seq_puts(m, "flags\t\t:");

	for (i = 0; i < X86_NR_CAPS; i++) {
		/*
		 * Go through the flag list in alphabetical
		 * order to make reading this field easier.
		 */
		int cap = x86_sorted_cap_flags[i];

		if (cpu_has(c, cap) && x86_cap_flags[cap] != NULL)
			seq_printf(m, " %s", x86_cap_flags[cap]);
	}
}

static int show_cpuinfo(struct seq_file *m, void *v)
{
	struct cpuinfo_x86 *c = v;
	unsigned int cpu;
	int i;

	cpu = c->cpu_index;
	seq_printf(m, "processor\t: %u\n"
		   "vendor_id\t: %s\n"
		   "cpu family\t: %d\n"
		   "model\t\t: %u\n"
		   "model name\t: %s\n",
		   cpu,
		   c->x86_vendor_id[0] ? c->x86_vendor_id : "unknown",
		   c->x86,
		   c->x86_model,
		   c->x86_model_id[0] ? c->x86_model_id : "unknown");

	if (c->x86_stepping || c->cpuid_level >= 0)
		seq_printf(m, "stepping\t: %d\n", c->x86_stepping);
	else
		seq_puts(m, "stepping\t: unknown\n");
	if (c->microcode)
		seq_printf(m, "microcode\t: 0x%x\n", c->microcode);

	if (cpu_has(c, X86_FEATURE_TSC)) {
		unsigned int freq = aperfmperf_get_khz(cpu);

		if (!freq)
			freq = cpufreq_quick_get(cpu);
		if (!freq)
			freq = cpu_khz;
		seq_printf(m, "cpu MHz\t\t: %u.%03u\n",
			   freq / 1000, (freq % 1000));
	}

	/* Cache size */
	if (c->x86_cache_size)
		seq_printf(m, "cache size\t: %u KB\n", c->x86_cache_size);

	show_cpuinfo_core(m, c, cpu);
	show_cpuinfo_misc(m, c);
	show_cpuinfo_flags(m, c);

	seq_puts(m, "\nbugs\t\t:");
	for (i = 0; i < 32*NBUGINTS; i++) {
		unsigned int bug_bit = x86_NR_CAPS + i;

		if (cpu_has_bug(c, bug_bit) && x86_bug_flags[i])
			seq_printf(m, " %s", x86_bug_flags[i]);
	}

	seq_printf(m, "\nbogomips\t: %lu.%02lu\n",
		   c->loops_per_jiffy/(500000/HZ),
		   (c->loops_per_jiffy/(5000/HZ)) % 100);

#ifdef CONFIG_X86_64
	if (c->x86_tlbsize > 0)
		seq_printf(m, "TLB size\t: %d 4K pages\n", c->x86_tlbsize);
#endif
	seq_printf(m, "clflush size\t: %u\n", c->x86_clflush_size);
	seq_printf(m, "cache_alignment\t: %d\n", c->x86_cache_alignment);
	seq_printf(m, "address sizes\t: %u bits physical, %u bits virtual\n",
		   c->x86_phys_bits, c->x86_virt_bits);

	seq_puts(m, "power management:");
	for (i = 0; i < 32; i++) {
		if (c->x86_power & (1 << i)) {
			if (i < ARRAY_SIZE(x86_power_flags) &&
			    x86_power_flags[i])
				seq_printf(m, "%s%s",
					   x86_power_flags[i][0] ? " " : "",
					   x86_power_flags[i]);
			else
				seq_printf(m, " [%d]", i);
		}
	}

	seq_puts(m, "\n\n");

	return 0;
}

static void *c_start(struct seq_file *m, loff_t *pos)
{
	*pos = cpumask_next(*pos - 1, cpu_online_mask);
	if ((*pos) < nr_cpu_ids)
		return &cpu_data(*pos);
	return NULL;
}

static void *c_next(struct seq_file *m, void *v, loff_t *pos)
{
	(*pos)++;
	return c_start(m, pos);
}

static void c_stop(struct seq_file *m, void *v)
{
}

const struct seq_operations cpuinfo_op = {
	.start	= c_start,
	.next	= c_next,
	.stop	= c_stop,
	.show	= show_cpuinfo,
};
