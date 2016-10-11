/*
 * MIPS Remote Processor driver
 *
 * Copyright (C) 2016 Imagination Technologies
 * Lisa Parratt <lisa.parratt@imgtec.com>
 * Matt Redfearn <matt.redfearn@imgtec.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 */

#include <linux/cpu.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/module.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/remoteproc.h>

#include <asm/smp-cps.h>
#include <asm/tlbflush.h>
#include <asm/tlbmisc.h>

#include "remoteproc_internal.h"

struct mips_rproc {
	struct rproc		*rproc;
	struct task_struct	*tsk;
	struct device		dev;
	unsigned int		cpu;
	int			ipi_linux;
	int			ipi_remote;
};

static struct rproc *mips_rprocs[NR_CPUS];

static int cpuhp_state;

#define to_mips_rproc(d) container_of(d, struct mips_rproc, dev)


/* Compute the largest page mask a physical address can be mapped with */
static unsigned long mips_rproc_largest_pm(unsigned long pa,
					   unsigned long maxmask)
{
	unsigned long mask;
	/* Find address bits limiting alignment */
	unsigned long shift = ffs(pa);

	/* Obey MIPS restrictions on page sizes */
	if (pa) {
		if (shift & 1)
			shift -= 2;
		else
			shift--;
	}
	mask = ULONG_MAX << shift;
	return maxmask & ~mask;
}

/* Compute the next largest page mask for a given page mask */
static unsigned long mips_rproc_next_pm(unsigned long pm, unsigned long maxmask)
{
	if (pm != PM_4K)
		return ((pm << 2) | pm) & maxmask;
	else
		return PM_16K;
}

static void mips_map_page(unsigned long da, unsigned long pa, int c,
			  unsigned long pagemask, unsigned long pagesize)
{
	unsigned long pa2 = pa + (pagesize / 2);
	unsigned long entryhi, entrylo0, entrylo1;

	/* Compute the mapping */
	pa = (pa >> 6) & (ULONG_MAX << MIPS_ENTRYLO_PFN_SHIFT);
	pa2 = (pa2 >> 6) & (ULONG_MAX << MIPS_ENTRYLO_PFN_SHIFT);
	entryhi = da & 0xfffffe000;
	entrylo0 = (c << ENTRYLO_C_SHIFT) | ENTRYLO_D | ENTRYLO_V | pa;
	entrylo1 = (c << ENTRYLO_C_SHIFT) | ENTRYLO_D | ENTRYLO_V | pa2;

	pr_debug("Create wired entry %d, CCA %d\n", read_c0_wired(), c);
	pr_debug(" EntryHi: 0x%016lx\n", entryhi);
	pr_debug(" EntryLo0: 0x%016lx\n", entrylo0);
	pr_debug(" EntryLo1: 0x%016lx\n", entrylo1);
	pr_debug(" Pagemask: 0x%016lx\n", pagemask);
	pr_debug("\n");

	add_wired_entry(entrylo0, entrylo1, entryhi, pagemask);
}

/*
 * Compute the page required to fulfill a mapping. Escapes alignment derived
 * page size limitations before using biggest fit to map the remainder.
 */
static inline void mips_rproc_fit_page(unsigned long da, unsigned long pa,
					int c, unsigned long size,
					unsigned long maxmask)
{
	unsigned long bigmask, nextmask;
	unsigned long pagemask, pagesize;
	unsigned long distance, target;

	do {
		/* Compute the current largest page mask */
		bigmask = mips_rproc_largest_pm(pa, maxmask);
		/* Compute the next largest pagesize */
		nextmask = mips_rproc_next_pm(bigmask, maxmask);
		/*
		 * Compute the distance from our current physical address to
		 * the next page boundary.
		 */
		distance = (nextmask + 0x2000) - (pa & nextmask);
		/*
		 * Decide between searching to get to the next highest page
		 * boundary or finishing.
		 */
		target = distance < size ? distance : size;
		/* Fit */
		while (target) {
			/* Find the largest supported page size that will fit */
			for (pagesize = maxmask + 0x2000;
			     (pagesize > 0x2000) && (pagesize > target);
			     pagesize /= 4) {
			}
			/* Convert it to a page mask */
			pagemask = pagesize - 0x2000;
			/* Emit it */
			mips_map_page(da, pa, c, pagemask, pagesize);
			/* Move to next step */
			size -= pagesize;
			da += pagesize;
			pa += pagesize;
			target -= pagesize;
		}
	} while (size);
}

static int mips_rproc_carveouts(struct rproc *rproc, int max_pagemask)
{
	struct rproc_mem_entry *carveout;

	list_for_each_entry(carveout, &rproc->carveouts, node) {
		int c = CONF_CM_CACHABLE_COW;

		dev_dbg(&rproc->dev,
			"carveout mapping da 0x%x -> %pad length 0x%x, CCA %d",
			carveout->da, &carveout->dma, carveout->len, c);

		mips_rproc_fit_page(carveout->da, carveout->dma, c,
				    carveout->len, max_pagemask);
	}
	return 0;
}

static int mips_rproc_vdevs(struct rproc *rproc, int max_pagemask)
{
	struct rproc_vdev *rvdev;

	list_for_each_entry(rvdev, &rproc->rvdevs, node) {
		int i, size;

		for (i = 0; i < ARRAY_SIZE(rvdev->vring); i++) {
			struct rproc_vring *vring = &rvdev->vring[i];
			unsigned long pa = vring->dma;
			int c;

			if (hw_coherentio) {
				/*
				 * The DMA API will allocate cacheable buffers
				 * for shared resources, so the firmware should
				 * also access those buffers cached
				 */
				c = (_page_cachable_default >> _CACHE_SHIFT);
			} else {
				/*
				 * Otherwise, shared buffers should be accessed
				 * uncached
				 */
				c = CONF_CM_UNCACHED;
			}

			/* actual size of vring (in bytes) */
			size = PAGE_ALIGN(vring_size(vring->len, vring->align));

			dev_dbg(&rproc->dev,
				"vring mapping da %pad -> %pad length 0x%x, CCA %d",
				&vring->dma, &vring->dma, size, c);

			mips_rproc_fit_page(pa, pa, c, size, max_pagemask);
		}
	}
	return 0;
}

static void mips_rproc_cpu_entry(void)
{
	struct rproc *rproc = mips_rprocs[smp_processor_id()];
	struct mips_rproc *mproc = *(struct mips_rproc **)rproc->priv;
	int ipi_to_remote = ipi_get_hwirq(mproc->ipi_remote, mproc->cpu);
	int ipi_from_remote = ipi_get_hwirq(mproc->ipi_linux, 0);
	unsigned long old_pagemask, max_pagemask;

	if (!rproc)
		return;

	dev_info(&rproc->dev, "Starting %s on MIPS CPU%d\n",
		 rproc->firmware, mproc->cpu);

	/* Get the maximum pagemask supported on this CPU */
	old_pagemask = read_c0_pagemask();
	write_c0_pagemask(PM_HUGE_MASK);
	mtc0_tlbw_hazard();
	max_pagemask = read_c0_pagemask();
	write_c0_pagemask(old_pagemask);
	mtc0_tlbw_hazard();

	/* Start with no wired entries */
	write_c0_wired(0);

	/* Flush all previous TLB entries */
	local_flush_tlb_all();

	/* Map firmware resources into virtual memory */
	mips_rproc_carveouts(rproc, max_pagemask);
	mips_rproc_vdevs(rproc, max_pagemask);

	dev_dbg(&rproc->dev, "IPI to remote: %d\n", ipi_to_remote);
	dev_dbg(&rproc->dev, "IPI from remote: %d\n", ipi_from_remote);

	/* Hand off the CPU to the firmware */
	dev_dbg(&rproc->dev, "Jumping to firmware at 0x%x\n", rproc->bootaddr);

	write_c0_entryhi(0); /* Set ASID 0 */
	tlbw_use_hazard();

	/* Firmware protocol */
	__asm__("addiu $a0, $zero, -3");
	__asm__("move $a1, %0" :: "r" (ipi_to_remote));
	__asm__("move $a2, %0" :: "r" (ipi_from_remote));
	__asm__("move $a3, $zero");
	__asm__("jr %0" :: "r" (rproc->bootaddr));
}

static irqreturn_t mips_rproc_ipi_handler(int irq, void *dev_id)
{
	/* Synthetic interrupts shouldn't need acking */
	return IRQ_WAKE_THREAD;
}

static irqreturn_t mips_rproc_vq_int(int irq, void *p)
{
	struct rproc *rproc = (struct rproc *)p;
	void *entry;
	int id;

	/* We don't have a mailbox, so iterate over all vqs and kick them. */
	idr_for_each_entry(&rproc->notifyids, entry, id)
		rproc_vq_interrupt(rproc, id);

	return IRQ_HANDLED;
}

/* Helper function to find the IPI domain */
static struct irq_domain *ipi_domain(void)
{
	struct device_node *node = of_irq_find_parent(of_root);
	struct irq_domain *ipidomain;

	ipidomain = irq_find_matching_host(node, DOMAIN_BUS_IPI);
	/*
	 * Some platforms have half DT setup. So if we found irq node but
	 * didn't find an ipidomain, try to search for one that is not in the
	 * DT.
	 */
	if (node && !ipidomain)
		ipidomain = irq_find_matching_host(NULL, DOMAIN_BUS_IPI);

	return ipidomain;
}

int mips_rproc_op_start(struct rproc *rproc)
{
	struct mips_rproc *mproc = *(struct mips_rproc **)rproc->priv;
	int err;
	int cpu = mproc->cpu;

	pr_info("%s\n", __func__);

	/* Create task for the CPU to use before handing off to firmware */
	mproc->tsk = fork_idle(cpu);
	if (IS_ERR(mproc->tsk)) {
		dev_err(&rproc->dev, "fork_idle() failed for CPU%d\n", cpu);
		return -ENOMEM;
	}

	/* We won't be needing the Linux IPIs anymore */
	if (mips_smp_ipi_free(get_cpu_mask(cpu))) {
		dev_err(&mproc->dev, "Failed to reserve incoming kick\n");
		goto exit_free_tsk;
	}

	/*
	 * Direct IPIs from the remote processor to CPU0 since that can't be
	 * offlined while the remote CPU is running.
	 */
	mproc->ipi_linux = irq_reserve_ipi(ipi_domain(), get_cpu_mask(0));
	if (!mproc->ipi_linux) {
		dev_err(&mproc->dev, "Failed to reserve incoming kick\n");
		goto exit_restore_ipi;
	}

	mproc->ipi_remote = irq_reserve_ipi(ipi_domain(), get_cpu_mask(cpu));
	if (!mproc->ipi_remote) {
		dev_err(&mproc->dev, "Failed to reserve outgoing kick\n");
		goto exit_destroy_ipi_linux;
	}

	/* register incoming ipi */
	err = request_threaded_irq(mproc->ipi_linux, mips_rproc_ipi_handler,
				   mips_rproc_vq_int, 0,
				   "mips-rproc IPI in", rproc);
	if (err) {
		dev_err(&mproc->dev, "Failed to register incoming kick: %d\n",
			err);
		goto exit_destroy_ipi_remote;
	}

	if (mips_cps_steal_cpu_and_execute(cpu, &mips_rproc_cpu_entry,
						mproc->tsk)) {
		dev_err(&mproc->dev, "Failed to steal CPU%d for remote\n", cpu);
		goto exit_free_irq;
	}

	dev_info(&mproc->dev, "CPU%d started\n", cpu);
	return 0;

exit_free_irq:
	free_irq(mproc->ipi_linux, rproc);
exit_destroy_ipi_remote:
	irq_destroy_ipi(mproc->ipi_remote, get_cpu_mask(cpu));
exit_destroy_ipi_linux:
	irq_destroy_ipi(mproc->ipi_linux, get_cpu_mask(0));
exit_restore_ipi:
	/* Set up the Linux IPIs again */
	mips_smp_ipi_allocate(get_cpu_mask(cpu));
exit_free_tsk:
	free_task(mproc->tsk);

	return -EINVAL;
}

int mips_rproc_op_stop(struct rproc *rproc)
{
	struct mips_rproc *mproc = *(struct mips_rproc **)rproc->priv;

	free_irq(mproc->ipi_linux, rproc);

	irq_destroy_ipi(mproc->ipi_linux, get_cpu_mask(0));
	irq_destroy_ipi(mproc->ipi_remote, get_cpu_mask(mproc->cpu));

	/* Set up the Linux IPIs again */
	mips_smp_ipi_allocate(get_cpu_mask(mproc->cpu));

	free_task(mproc->tsk);

	return mips_cps_halt_and_return_cpu(mproc->cpu);
}

void mips_rproc_op_kick(struct rproc *rproc, int vqid)
{
	struct mips_rproc *mproc = *(struct mips_rproc **)rproc->priv;

	if (rproc->state == RPROC_RUNNING)
		ipi_send_single(mproc->ipi_remote, mproc->cpu);
}

struct rproc_ops mips_rproc_proc_ops = {
	.start	= mips_rproc_op_start,
	.stop	= mips_rproc_op_stop,
	.kick	= mips_rproc_op_kick,
};


static int mips_rproc_probe(struct platform_device *pdev)
{
	return 0;
}

static int mips_rproc_remove(struct platform_device *pdev)
{
	return 0;
}

static struct platform_driver mips_rproc_driver = {
	.probe = mips_rproc_probe,
	.remove = mips_rproc_remove,
	.driver = {
		.name = "mips-rproc"
	},
};

static void mips_rproc_release(struct device *dev)
{
}

static int mips_rproc_uevent(struct device *dev, struct kobj_uevent_env *env)
{
	struct mips_rproc *mproc = to_mips_rproc(dev);

	if (!mproc)
		return -ENODEV;

	return 0;
}

static struct device_type mips_rproc_type = {
	.release	= mips_rproc_release,
	.uevent		= mips_rproc_uevent
};

/* Create an rproc instance in response to CPU down */
static int mips_rproc_device_register(unsigned int cpu)
{
	char name[64], *template = "mips-cpu%u";
	struct rproc *rproc;
	struct mips_rproc *mproc;
	int err;

	pr_debug("Allocating MIPS rproc for cpu%d\n", cpu);

	if (mips_rprocs[cpu]) {
		dev_err(&rproc->dev, "CPU%d in use\n", cpu);
		return -EBUSY;
	}

	mproc = kzalloc(sizeof(struct mips_rproc), GFP_KERNEL);
	if (!mproc) {
		err = -ENOMEM;
		goto exit;
	}

	mproc->dev.driver = &mips_rproc_driver.driver;
	mproc->dev.type = &mips_rproc_type;
	mproc->dev.id = cpu;
	snprintf(name, sizeof(name), template, cpu);
	dev_set_name(&mproc->dev, name);
	mproc->cpu = cpu;

	/* Set device to have coherent DMA ops */
	arch_setup_dma_ops(&mproc->dev, 0, 0, NULL, 1);

	rproc = rproc_alloc(&mproc->dev, dev_name(&mproc->dev),
			    &mips_rproc_proc_ops, NULL,
			    sizeof(struct mips_rproc *));
	if (!rproc) {
		pr_err("Error allocting rproc\n");
		err = -ENOMEM;
		goto exit_free_mproc;
	}

	*(struct mips_rproc **)rproc->priv = mproc;

	err = device_register(&mproc->dev);
	if (err) {
		pr_err("device add failed\n");
		goto exit_free_rproc;
	}

	err = rproc_add(rproc);
	if (err) {
		dev_err(&mproc->dev, "Failed to add rproc: %d\n", err);
		goto exit_free_rproc;
	}

	mips_rprocs[cpu] = rproc;
	return 0;

exit_free_rproc:
	rproc_put(rproc);
exit_free_mproc:
	kfree(mproc);
exit:
	return err;
}

/* Destroy rproc instance in response to CPU up */
static int mips_rproc_device_unregister(unsigned int cpu)
{
	struct rproc *rproc = mips_rprocs[cpu];
	struct mips_rproc *mproc;

	if (!rproc)
		/* No rproc class has been created for this CPU */
		return 0;

	pr_debug("Deallocating MIPS rproc for cpu%d\n", cpu);

	rproc_del(rproc);
	rproc_put(rproc);

	mproc = *(struct mips_rproc **)rproc->priv;
	device_unregister(&mproc->dev);
	kfree(mproc);

	mips_rprocs[cpu] = NULL;
	return 0;
}

static int __init mips_rproc_init(void)
{
	int cpu;

	/*
	 * Register with the cpu hotplug state machine.
	 * This driver requires opposite sense to "normal" drivers, since the
	 * driver is activated for offline CPUs via the teardown callback and
	 * deactivated via the online callback.
	 */
	cpuhp_state = cpuhp_setup_state(CPUHP_AP_ONLINE_DYN, "MIPS:REMOTEPROC",
					mips_rproc_device_unregister,
					mips_rproc_device_register);

	/* Register a device for each offline CPU */
	get_online_cpus();
	for_each_possible_cpu(cpu)
		if (!cpu_online(cpu))
			mips_rproc_device_register(cpu);
	put_online_cpus();

	return 0;
}

static void __exit mips_rproc_exit(void)
{
	int cpu;

	if (cpuhp_state) {
		/*
		 * Unregister with the cpu hotplug state machine, but don't call
		 * the teardown callback, since that would try to start the
		 * remote processor device.
		 */
		__cpuhp_remove_state(cpuhp_state, false);
		cpuhp_state = 0;
	}

	get_online_cpus();
	/* Unregister devices created for any offline CPUs */
	for_each_possible_cpu(cpu)
		mips_rproc_device_unregister(cpu);
	put_online_cpus();
}

subsys_initcall(mips_rproc_init);
module_exit(mips_rproc_exit);

module_platform_driver(mips_rproc_driver);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("MIPS Remote Processor control driver");
