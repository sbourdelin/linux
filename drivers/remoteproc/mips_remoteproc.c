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
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/module.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/remoteproc.h>

#include <asm/dma-coherence.h>
#include <asm/smp-cps.h>
#include <asm/tlbflush.h>
#include <asm/tlbmisc.h>

#include "remoteproc_internal.h"

struct mips_rproc {
	struct rproc		*rproc;
	char			*firmware;
	struct task_struct	*tsk;
	struct device		dev;
	unsigned int		cpu;
	int			ipi_linux;
	int			ipi_remote;
};

static struct rproc *mips_rprocs[NR_CPUS];

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
		 mproc->firmware, mproc->cpu);

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

	if (mips_rprocs[cpu]) {
		dev_err(&rproc->dev, "CPU%d in use\n", cpu);
		return -EBUSY;
	}
	mips_rprocs[cpu] = rproc;

	/* Create task for the CPU to use before handing off to firmware */
	mproc->tsk = fork_idle(cpu);
	if (IS_ERR(mproc->tsk)) {
		dev_err(&rproc->dev, "fork_idle() failed for CPU%d\n", cpu);
		return -ENOMEM;
	}

	/* We won't be needing the Linux IPIs anymore */
	if (mips_smp_ipi_free(get_cpu_mask(cpu)))
		return -EINVAL;

	/*
	 * Direct IPIs from the remote processor to CPU0 since that can't be
	 * offlined while the remote CPU is running.
	 */
	mproc->ipi_linux = irq_reserve_ipi(ipi_domain(), get_cpu_mask(0));
	if (!mproc->ipi_linux) {
		dev_err(&mproc->dev, "Failed to reserve incoming kick\n");
		goto exit_rproc_nofrom;
	}

	mproc->ipi_remote = irq_reserve_ipi(ipi_domain(), get_cpu_mask(cpu));
	if (!mproc->ipi_remote) {
		dev_err(&mproc->dev, "Failed to reserve outgoing kick\n");
		goto exit_rproc_noto;
	}

	/* register incoming ipi */
	err = devm_request_threaded_irq(&mproc->dev, mproc->ipi_linux,
					mips_rproc_ipi_handler,
					mips_rproc_vq_int, 0,
					"mips-rproc IPI in", mproc->rproc);
	if (err) {
		dev_err(&mproc->dev, "Failed to register incoming kick: %d\n",
			err);
		goto exit_rproc_noint;
	}

	if (!mips_cps_steal_cpu_and_execute(cpu, &mips_rproc_cpu_entry,
						mproc->tsk))
		return 0;

	dev_err(&mproc->dev, "Failed to steal CPU%d for remote\n", cpu);
	devm_free_irq(&mproc->dev, mproc->ipi_linux, mproc->rproc);
exit_rproc_noint:
	irq_destroy_ipi(mproc->ipi_remote, get_cpu_mask(cpu));
exit_rproc_noto:
	irq_destroy_ipi(mproc->ipi_linux, get_cpu_mask(0));
exit_rproc_nofrom:
	free_task(mproc->tsk);
	mips_rprocs[cpu] = NULL;

	/* Set up the Linux IPIs again */
	mips_smp_ipi_allocate(get_cpu_mask(cpu));
	return -EINVAL;
}

int mips_rproc_op_stop(struct rproc *rproc)
{
	struct mips_rproc *mproc = *(struct mips_rproc **)rproc->priv;

	if (mproc->ipi_linux)
		devm_free_irq(&mproc->dev, mproc->ipi_linux, mproc->rproc);

	irq_destroy_ipi(mproc->ipi_linux, get_cpu_mask(0));
	irq_destroy_ipi(mproc->ipi_remote, get_cpu_mask(mproc->cpu));

	/* Set up the Linux IPIs again */
	mips_smp_ipi_allocate(get_cpu_mask(mproc->cpu));

	free_task(mproc->tsk);

	mips_rprocs[mproc->cpu] = NULL;

	return mips_cps_halt_and_return_cpu(mproc->cpu);
}


void mips_rproc_op_kick(struct rproc *rproc, int vqid)
{
	struct mips_rproc *mproc = *(struct mips_rproc **)rproc->priv;

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


/* Steal a core and run some firmware on it */
int mips_rproc_start(struct mips_rproc *mproc, const char *firmware, size_t len)
{
	int err = -EINVAL;
	struct mips_rproc **priv;

	/* Duplicate the filename, dropping whitespace from the end via len */
	mproc->firmware = kstrndup(firmware, len, GFP_KERNEL);
	if (!mproc->firmware)
		return -ENOMEM;

	mproc->rproc = rproc_alloc(&mproc->dev, "mips", &mips_rproc_proc_ops,
				   mproc->firmware,
				   sizeof(struct mips_rproc *));
	if (!mproc->rproc)
		return -ENOMEM;

	priv = mproc->rproc->priv;
	*priv = mproc;

	/* go live! */
	err = rproc_add(mproc->rproc);
	if (err) {
		dev_err(&mproc->dev, "Failed to add rproc: %d\n", err);
		rproc_put(mproc->rproc);
		kfree(mproc->firmware);
		return -EINVAL;
	}

	return 0;
}

/* Stop a core, and return it to being offline */
int mips_rproc_stop(struct mips_rproc *mproc)
{
	rproc_shutdown(mproc->rproc);
	rproc_del(mproc->rproc);
	rproc_put(mproc->rproc);
	mproc->rproc = NULL;
	return 0;
}

/* sysfs interface to mips_rproc_start */
static ssize_t firmware_store(struct device *dev,
			      struct device_attribute *attr,
			      const char *buf, size_t count)
{
	struct mips_rproc *mproc = to_mips_rproc(dev);
	size_t len = count;
	int err = -EINVAL;

	if (buf[count - 1] == '\n')
		len--;

	if (!mproc->rproc && len)
		err = mips_rproc_start(mproc, buf, len);
	else if (len)
		err = -EBUSY;

	return err ? err : count;
}
static DEVICE_ATTR_WO(firmware);

/* sysfs interface to mips_rproc_stop */
static ssize_t stop_store(struct device *dev,
			      struct device_attribute *attr,
			      const char *buf, size_t count)
{
	struct mips_rproc *mproc = to_mips_rproc(dev);
	int err = -EINVAL;


	if (mproc->rproc)
		err = mips_rproc_stop(mproc);
	else
		err = -EBUSY;

	return err ? err : count;
}
static DEVICE_ATTR_WO(stop);

/* Boiler plate for devclarng mips-rproc sysfs devices */
static struct attribute *mips_rproc_attrs[] = {
	&dev_attr_firmware.attr,
	&dev_attr_stop.attr,
	NULL
};

static struct attribute_group mips_rproc_devgroup = {
	.attrs = mips_rproc_attrs
};

static const struct attribute_group *mips_rproc_devgroups[] = {
	&mips_rproc_devgroup,
	NULL
};

static char *mips_rproc_devnode(struct device *dev, umode_t *mode)
{
	return kasprintf(GFP_KERNEL, "mips-rproc/%s", dev_name(dev));
}

static struct class mips_rproc_class = {
	.name		= "mips-rproc",
	.devnode	= mips_rproc_devnode,
	.dev_groups	= mips_rproc_devgroups,
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

/* Helper function for locating the device for a CPU */
int mips_rproc_device_rproc_match(struct device *dev, const void *data)
{
	struct mips_rproc *mproc = to_mips_rproc(dev);
	unsigned int cpu = *(unsigned int *)data;

	return mproc->cpu == cpu;
}

/* Create a sysfs device in response to CPU down */
int mips_rproc_device_register(unsigned int cpu)
{
	struct mips_rproc *dev;

	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -EINVAL;

	dev->dev.driver = &mips_rproc_driver.driver;
	dev->dev.type = &mips_rproc_type;
	dev->dev.class = &mips_rproc_class;
	dev->dev.id = cpu;
	dev_set_name(&dev->dev, "rproc%u", cpu);
	dev->cpu = cpu;

	return device_register(&dev->dev);
}

/* Destroy a sysfs device in response to CPU up */
int mips_rproc_device_unregister(unsigned int cpu)
{
	struct device *dev = class_find_device(&mips_rproc_class, NULL, &cpu,
					       mips_rproc_device_rproc_match);
	struct mips_rproc *mproc = to_mips_rproc(dev);

	if (mips_rprocs[cpu])
		mips_rproc_stop(mproc);

	put_device(dev);
	device_unregister(dev);
	kfree(mproc);
	return 0;
}

/* Intercept CPU hotplug events for syfs purposes */
static int mips_rproc_callback(struct notifier_block *nfb, unsigned long action,
			       void *hcpu)
{
	unsigned int cpu = (unsigned long)hcpu;

	switch (action) {
	case CPU_UP_PREPARE:
	case CPU_DOWN_FAILED:
		mips_rproc_device_unregister(cpu);
		break;
	case CPU_DOWN_PREPARE:
		mips_rproc_device_register(cpu);
		break;
	}

	return NOTIFY_OK;
}

static struct notifier_block mips_rproc_notifier __refdata = {
	.notifier_call = mips_rproc_callback
};

static int __init mips_rproc_init(void)
{
	int cpu;
	/* create mips-rproc device class for sysfs */
	int err = class_register(&mips_rproc_class);

	if (err) {
		pr_err("mips-proc: unable to register mips-rproc class\n");
		return err;
	}

	/* Dynamically create mips-rproc class devices based on hotplug data */
	get_online_cpus();
	for_each_possible_cpu(cpu)
		if (!cpu_online(cpu))
			mips_rproc_device_register(cpu);
	register_hotcpu_notifier(&mips_rproc_notifier);
	put_online_cpus();

	return 0;
}

static void __exit mips_rproc_exit(void)
{
	int cpu;
	/* Destroy mips-rproc class devices */
	get_online_cpus();
	unregister_hotcpu_notifier(&mips_rproc_notifier);
	for_each_possible_cpu(cpu)
		if (!cpu_online(cpu))
			mips_rproc_device_unregister(cpu);
	put_online_cpus();

	class_unregister(&mips_rproc_class);
}

subsys_initcall(mips_rproc_init);
module_exit(mips_rproc_exit);

module_platform_driver(mips_rproc_driver);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("MIPS Remote Processor control driver");
