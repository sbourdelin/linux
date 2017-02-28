/*
 * Copyright (c) 2017 Linaro Limited. All rights reserved.
 *
 * Author: Leo Yan <leo.yan@linaro.org>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */
#include <linux/amba/bus.h>
#include <linux/coresight.h>
#include <linux/cpu.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/moduleparam.h>
#include <linux/slab.h>
#include <linux/smp.h>
#include <linux/types.h>
#include <linux/uaccess.h>

#include "coresight-priv.h"

#define EDPCSR				0x0A0
#define EDCIDSR				0x0A4
#define EDVIDSR				0x0A8
#define EDPCSR_HI			0x0AC
#define EDOSLAR				0x300
#define EDPRSR				0x314
#define EDDEVID1			0xFC4
#define EDDEVID				0xFC8

#define EDPCSR_PROHIBITED		0xFFFFFFFF

/* bits definition for EDPCSR */
#ifndef CONFIG_64BIT
#define EDPCSR_THUMB			BIT(0)
#define EDPCSR_ARM_INST_MASK		GENMASK(31, 2)
#define EDPCSR_THUMB_INST_MASK		GENMASK(31, 1)
#endif

/* bits definition for EDPRSR */
#define EDPRSR_DLK			BIT(6)
#define EDPRSR_PU			BIT(0)

/* bits definition for EDVIDSR */
#define EDVIDSR_NS			BIT(31)
#define EDVIDSR_E2			BIT(30)
#define EDVIDSR_E3			BIT(29)
#define EDVIDSR_HV			BIT(28)
#define EDVIDSR_VMID			GENMASK(7, 0)

/* bits definition for EDDEVID1 */
#define EDDEVID1_PCSR_OFFSET_MASK	GENMASK(3, 0)
#define EDDEVID1_PCSR_OFFSET_INS_SET	(0x0)

/* bits definition for EDDEVID */
#define EDDEVID_PCSAMPLE_MODE		GENMASK(3, 0)
#define EDDEVID_IMPL_EDPCSR_EDCIDSR	(0x2)
#define EDDEVID_IMPL_FULL		(0x3)

struct debug_drvdata {
	void __iomem	*base;
	struct device	*dev;
	int		cpu;

	bool		edpcsr_present;
	bool		edvidsr_present;
	bool		pc_has_offset;

	u32		eddevid;
	u32		eddevid1;

	u32		edpcsr;
	u32		edpcsr_hi;
	u32		edprsr;
	u32		edvidsr;
	u32		edcidsr;
};

static DEFINE_PER_CPU(struct debug_drvdata *, debug_drvdata);

static void debug_os_unlock(struct debug_drvdata *drvdata)
{
	/* Unlocks the debug registers */
	writel_relaxed(0x0, drvdata->base + EDOSLAR);
	wmb();
}

/*
 * According to ARM DDI 0487A.k, before access external debug
 * registers should firstly check the access permission; if any
 * below condition has been met then cannot access debug
 * registers to avoid lockup issue:
 *
 * - CPU power domain is powered off;
 * - The OS Double Lock is locked;
 *
 * By checking EDPRSR can get to know if meet these conditions.
 */
static bool debug_access_permitted(struct debug_drvdata *drvdata)
{
	/* CPU is powered off */
	if (!(drvdata->edprsr & EDPRSR_PU))
		return false;

	/* The OS Double Lock is locked */
	if (drvdata->edprsr & EDPRSR_DLK)
		return false;

	return true;
}

static void debug_read_regs(struct debug_drvdata *drvdata)
{
	drvdata->edprsr = readl_relaxed(drvdata->base + EDPRSR);

	if (!debug_access_permitted(drvdata))
		return;

	if (!drvdata->edpcsr_present)
		return;

	CS_UNLOCK(drvdata->base);

	debug_os_unlock(drvdata);

	drvdata->edpcsr = readl_relaxed(drvdata->base + EDPCSR);

	/*
	 * As described in ARM DDI 0487A.k, if the processing
	 * element (PE) is in debug state, or sample-based
	 * profiling is prohibited, EDPCSR reads as 0xFFFFFFFF;
	 * EDCIDSR, EDVIDSR and EDPCSR_HI registers also become
	 * UNKNOWN state. So directly bail out for this case.
	 */
	if (drvdata->edpcsr == EDPCSR_PROHIBITED) {
		CS_LOCK(drvdata->base);
		return;
	}

	/*
	 * A read of the EDPCSR normally has the side-effect of
	 * indirectly writing to EDCIDSR, EDVIDSR and EDPCSR_HI;
	 * at this point it's safe to read value from them.
	 */
	drvdata->edcidsr = readl_relaxed(drvdata->base + EDCIDSR);
#ifdef CONFIG_64BIT
	drvdata->edpcsr_hi = readl_relaxed(drvdata->base + EDPCSR_HI);
#endif

	if (drvdata->edvidsr_present)
		drvdata->edvidsr = readl_relaxed(drvdata->base + EDVIDSR);

	CS_LOCK(drvdata->base);
}

#ifndef CONFIG_64BIT
static bool debug_pc_has_offset(struct debug_drvdata *drvdata)
{
	u32 pcsr_offset;

	pcsr_offset = drvdata->eddevid1 & EDDEVID1_PCSR_OFFSET_MASK;

	return (pcsr_offset == EDDEVID1_PCSR_OFFSET_INS_SET);
}

static unsigned long debug_adjust_pc(struct debug_drvdata *drvdata,
				     unsigned long pc)
{
	unsigned long arm_inst_offset = 0, thumb_inst_offset = 0;

	if (debug_pc_has_offset(drvdata)) {
		arm_inst_offset = 8;
		thumb_inst_offset = 4;
	}

	/* Handle thumb instruction */
	if (pc & EDPCSR_THUMB) {
		pc = (pc & EDPCSR_THUMB_INST_MASK) - thumb_inst_offset;
		return pc;
	}

	/*
	 * Handle arm instruction offset, if the arm instruction
	 * is not 4 byte alignment then it's possible the case
	 * for implementation defined; keep original value for this
	 * case and print info for notice.
	 */
	if (pc & BIT(1))
		pr_emerg("Instruction offset is implementation defined\n");
	else
		pc = (pc & EDPCSR_ARM_INST_MASK) - arm_inst_offset;

	return pc;
}
#endif

static void debug_dump_regs(struct debug_drvdata *drvdata)
{
	unsigned long pc;

	pr_emerg("\tEDPRSR:  %08x (Power:%s DLK:%s)\n", drvdata->edprsr,
		 drvdata->edprsr & EDPRSR_PU ? "On" : "Off",
		 drvdata->edprsr & EDPRSR_DLK ? "Lock" : "Unlock");

	if (!debug_access_permitted(drvdata) || !drvdata->edpcsr_present) {
		pr_emerg("No permission to access debug registers!\n");
		return;
	}

	if (drvdata->edpcsr == EDPCSR_PROHIBITED) {
		pr_emerg("CPU is in Debug state or profiling is prohibited!\n");
		return;
	}

#ifdef CONFIG_64BIT
	pc = (unsigned long)drvdata->edpcsr_hi << 32 |
	     (unsigned long)drvdata->edpcsr;
#else
	pc = debug_adjust_pc(drvdata, (unsigned long)drvdata->edpcsr);
#endif

	pr_emerg("\tEDPCSR:  [<%p>] %pS\n", (void *)pc, (void *)pc);
	pr_emerg("\tEDCIDSR: %08x\n", drvdata->edcidsr);

	if (!drvdata->edvidsr_present)
		return;

	pr_emerg("\tEDVIDSR: %08x (State:%s Mode:%s Width:%s VMID:%x)\n",
		 drvdata->edvidsr,
		 drvdata->edvidsr & EDVIDSR_NS ? "Non-secure" : "Secure",
		 drvdata->edvidsr & EDVIDSR_E3 ? "EL3" :
			(drvdata->edvidsr & EDVIDSR_E2 ? "EL2" : "EL1/0"),
		 drvdata->edvidsr & EDVIDSR_HV ? "64bits" : "32bits",
		 drvdata->edvidsr & (u32)EDVIDSR_VMID);
}

/*
 * Dump out information on panic.
 */
static int debug_notifier_call(struct notifier_block *self,
			       unsigned long v, void *p)
{
	int cpu;

	pr_emerg("ARM external debug module:\n");

	for_each_possible_cpu(cpu) {

		if (!per_cpu(debug_drvdata, cpu))
			continue;

		pr_emerg("CPU[%d]:\n", per_cpu(debug_drvdata, cpu)->cpu);

		debug_read_regs(per_cpu(debug_drvdata, cpu));
		debug_dump_regs(per_cpu(debug_drvdata, cpu));
	}

	return 0;
}

static struct notifier_block debug_notifier = {
	.notifier_call = debug_notifier_call,
};

static void debug_init_arch_data(void *info)
{
	struct debug_drvdata *drvdata = info;
	u32 mode;

	CS_UNLOCK(drvdata->base);

	debug_os_unlock(drvdata);

	/* Read device info */
	drvdata->eddevid  = readl_relaxed(drvdata->base + EDDEVID);
	drvdata->eddevid1 = readl_relaxed(drvdata->base + EDDEVID1);

	/* Parse implementation feature */
	mode = drvdata->eddevid & EDDEVID_PCSAMPLE_MODE;
	if (mode == EDDEVID_IMPL_FULL) {
		drvdata->edpcsr_present  = true;
		drvdata->edvidsr_present = true;
	} else if (mode == EDDEVID_IMPL_EDPCSR_EDCIDSR) {
		drvdata->edpcsr_present  = true;
		drvdata->edvidsr_present = false;
	} else {
		drvdata->edpcsr_present  = false;
		drvdata->edvidsr_present = false;
	}

	CS_LOCK(drvdata->base);
}

static int debug_probe(struct amba_device *adev, const struct amba_id *id)
{
	void __iomem *base;
	struct device *dev = &adev->dev;
	struct debug_drvdata *drvdata;
	struct resource *res = &adev->res;
	struct device_node *np = adev->dev.of_node;
	char buf[32];
	static int debug_count;

	drvdata = devm_kzalloc(dev, sizeof(*drvdata), GFP_KERNEL);
	if (!drvdata)
		return -ENOMEM;

	drvdata->cpu = np ? of_coresight_get_cpu(np) : 0;
	drvdata->dev = &adev->dev;

	dev_set_drvdata(dev, drvdata);

	/* Validity for the resource is already checked by the AMBA core */
	base = devm_ioremap_resource(dev, res);
	if (IS_ERR(base))
		return PTR_ERR(base);

	drvdata->base = base;

	get_online_cpus();
	per_cpu(debug_drvdata, drvdata->cpu) = drvdata;

	if (smp_call_function_single(drvdata->cpu,
				debug_init_arch_data, drvdata, 1))
		dev_err(dev, "Debug arch init failed\n");

	put_online_cpus();

	if (!debug_count++)
		atomic_notifier_chain_register(&panic_notifier_list,
					       &debug_notifier);

	sprintf(buf, (char *)id->data, drvdata->cpu);
	dev_info(dev, "%s initialized\n", buf);
	return 0;
}

static struct amba_id debug_ids[] = {
	{       /* Debug for Cortex-A53 */
		.id	= 0x000bbd03,
		.mask	= 0x000fffff,
		.data   = "Coresight debug-CPU%d",
	},
	{       /* Debug for Cortex-A57 */
		.id	= 0x000bbd07,
		.mask	= 0x000fffff,
		.data   = "Coresight debug-CPU%d",
	},
	{       /* Debug for Cortex-A72 */
		.id	= 0x000bbd08,
		.mask	= 0x000fffff,
		.data   = "Coresight debug-CPU%d",
	},
	{ 0, 0 },
};

static struct amba_driver debug_driver = {
	.drv = {
		.name   = "coresight-debug",
		.suppress_bind_attrs = true,
	},
	.probe		= debug_probe,
	.id_table	= debug_ids,
};
builtin_amba_driver(debug_driver);
