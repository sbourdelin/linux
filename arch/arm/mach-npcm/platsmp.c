/*
 * Copyright (C) 2002 ARM Ltd.
 * Copyright (C) 2008 STMicroelctronics.
 * Copyright (C) 2009 ST-Ericsson.
 * Copyright 2017 Google, Inc.
 *
 * This file is based on arm realview platform
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/smp.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_platform.h>
#include <linux/of_address.h>
#include <asm/cacheflush.h>
#include <asm/smp.h>
#include <asm/smp_plat.h>
#include <asm/smp_scu.h>

#define NPCM7XX_SCRPAD_REG 0x13c

static void __iomem *gcr_base;
static void __iomem *scu_base;

/* This is called from headsmp.S to wakeup the secondary core */
extern void npcm7xx_secondary_startup(void);
extern void npcm7xx_wakeup_z1(void);

/*
 * Write pen_release in a way that is guaranteed to be visible to all
 * observers, irrespective of whether they're taking part in coherency
 * or not.  This is necessary for the hotplug code to work reliably.
 */
static void npcm7xx_write_pen_release(int val)
{
	pen_release = val;
	/* write to pen_release must be visible to all observers. */
	smp_wmb();
	__cpuc_flush_dcache_area((void *)&pen_release, sizeof(pen_release));
	outer_clean_range(__pa(&pen_release), __pa(&pen_release + 1));
}

static DEFINE_SPINLOCK(boot_lock);

static void npcm7xx_smp_secondary_init(unsigned int cpu)
{
	/*
	 * let the primary processor know we're out of the
	 * pen, then head off into the C entry point
	 */
	npcm7xx_write_pen_release(-1);

	/*
	 * Synchronise with the boot thread.
	 */
	spin_lock(&boot_lock);
	spin_unlock(&boot_lock);
}

static int npcm7xx_smp_boot_secondary(unsigned int cpu, struct task_struct *idle)
{
	unsigned long timeout;

	/*
	 * set synchronisation state between this boot processor
	 * and the secondary one
	 */
	spin_lock(&boot_lock);

	/*
	 * The secondary processor is waiting to be released from
	 * the holding pen - release it, then wait for it to flag
	 * that it has been released by resetting pen_release.
	 */
	npcm7xx_write_pen_release(cpu_logical_map(cpu));
	iowrite32(virt_to_phys(npcm7xx_secondary_startup),
		  gcr_base + NPCM7XX_SCRPAD_REG);
	/* make npcm7xx_secondary_startup visible to all observers. */
	smp_rmb();

	arch_send_wakeup_ipi_mask(cpumask_of(cpu));
	timeout  = jiffies + (HZ * 1);
	while (time_before(jiffies, timeout)) {
		/* make sure we see any writes to pen_release. */
		smp_rmb();
		if (pen_release == -1)
			break;

		udelay(10);
	}

	/*
	 * now the secondary core is starting up let it run its
	 * calibrations, then wait for it to finish
	 */
	spin_unlock(&boot_lock);

	return pen_release != -1 ? -EIO : 0;
}


static void __init npcm7xx_wakeup_secondary(void)
{
	/*
	 * write the address of secondary startup into the backup ram register
	 * at offset 0x1FF4, then write the magic number 0xA1FEED01 to the
	 * backup ram register at offset 0x1FF0, which is what boot rom code
	 * is waiting for. This would wake up the secondary core from WFE
	 */
	iowrite32(virt_to_phys(npcm7xx_secondary_startup), gcr_base +
		  NPCM7XX_SCRPAD_REG);
	/* make sure npcm7xx_secondary_startup is seen by all observers. */
	smp_wmb();
	dsb_sev();

	/* make sure write buffer is drained */
	mb();
}

static void __init npcm7xx_smp_init_cpus(void)
{
	struct device_node *gcr_np, *scu_np;
	unsigned int i, ncores;

	gcr_np = of_find_compatible_node(NULL, NULL, "nuvoton,npcm750-gcr");
	gcr_base = of_iomap(gcr_np, 0);
	if (IS_ERR(gcr_base)) {
		pr_err("no gcr device node: %ld\n", PTR_ERR(gcr_base));
		return;
	}

	scu_np = of_find_compatible_node(NULL, NULL, "arm,cortex-a9-scu");
	scu_base = of_iomap(scu_np, 0);
	if (IS_ERR(scu_base)) {
		pr_err("no scu device node: %ld\n", PTR_ERR(scu_base));
		return;
	}

	ncores = scu_get_core_count(scu_base);

	if (ncores > nr_cpu_ids) {
		pr_warn("SMP: %u cores greater than maximum (%u), clipping\n",
			ncores, nr_cpu_ids);
		ncores = nr_cpu_ids;
	}

	for (i = 0; i < ncores; i++)
		set_cpu_possible(i, true);
}

static void __init npcm7xx_smp_prepare_cpus(unsigned int max_cpus)
{
	scu_enable(scu_base);
	npcm7xx_wakeup_secondary();
}

static struct smp_operations npcm7xx_smp_ops __initdata = {
	.smp_init_cpus    = npcm7xx_smp_init_cpus,
	.smp_prepare_cpus = npcm7xx_smp_prepare_cpus,
	.smp_boot_secondary = npcm7xx_smp_boot_secondary,
	.smp_secondary_init = npcm7xx_smp_secondary_init,
};

CPU_METHOD_OF_DECLARE(npcm7xx_smp, "nuvoton,npcm7xx-smp", &npcm7xx_smp_ops);
