#include <linux/init.h>
#include <linux/smp.h>
#include "smc.h"

static int tango_boot_secondary(unsigned int cpu, struct task_struct *idle)
{
	tango_set_aux_boot_addr(virt_to_phys(secondary_startup));
	tango_start_aux_core(cpu);
	return 0;
}

#ifdef CONFIG_HOTPLUG_CPU
static int tango_cpu_kill(unsigned int cpu)
{
	return tango_aux_core_kill(cpu);
}

static void tango_cpu_die(unsigned int cpu)
{
	tango_aux_core_die(cpu);
}
#else
#define tango_cpu_kill	NULL
#define tango_cpu_die	NULL
#endif

static const struct smp_operations tango_smp_ops __initconst = {
	.smp_boot_secondary	= tango_boot_secondary,
	.cpu_kill		= tango_cpu_kill,
	.cpu_die		= tango_cpu_die,
};

CPU_METHOD_OF_DECLARE(tango4_smp, "sigma,tango4-smp", &tango_smp_ops);
