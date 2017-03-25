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
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/pm_qos.h>
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
#define EDPRCR				0x310
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

/* bits definition for EDPRCR */
#define EDPRCR_COREPURQ			BIT(3)
#define EDPRCR_CORENPDRQ		BIT(0)

/* bits definition for EDPRSR */
#define EDPRSR_DLK			BIT(6)
#define EDPRSR_PU			BIT(0)

/* bits definition for EDVIDSR */
#define EDVIDSR_NS			BIT(31)
#define EDVIDSR_E2			BIT(30)
#define EDVIDSR_E3			BIT(29)
#define EDVIDSR_HV			BIT(28)
#define EDVIDSR_VMID			GENMASK(7, 0)

/*
 * bits definition for EDDEVID1:PSCROffset
 *
 * NOTE: armv8 and armv7 have different definition for the register,
 * so consolidate the bits definition as below:
 *
 * 0b0000 - Sample offset applies based on the instruction state, we
 *          rely on EDDEVID to check if EDPCSR is implemented or not
 * 0b0001 - No offset applies.
 * 0b0010 - No offset applies, but do not use in AArch32 mode
 *
 */
#define EDDEVID1_PCSR_OFFSET_MASK	GENMASK(3, 0)
#define EDDEVID1_PCSR_OFFSET_INS_SET	(0x0)
#define EDDEVID1_PCSR_NO_OFFSET_DIS_AARCH32	(0x2)

/* bits definition for EDDEVID */
#define EDDEVID_PCSAMPLE_MODE		GENMASK(3, 0)
#define EDDEVID_IMPL_NONE		(0x0)
#define EDDEVID_IMPL_EDPCSR		(0x1)
#define EDDEVID_IMPL_EDPCSR_EDCIDSR	(0x2)
#define EDDEVID_IMPL_FULL		(0x3)

#define DEBUG_WAIT_TIMEOUT		32

struct debug_drvdata {
	void __iomem	*base;
	struct device	*dev;
	int		cpu;

	bool		edpcsr_present;
	bool		edcidsr_present;
	bool		edvidsr_present;
	bool		pc_has_offset;

	u32		eddevid;
	u32		eddevid1;

	u32		edpcsr;
	u32		edpcsr_hi;
	u32		edprcr;
	u32		edprsr;
	u32		edvidsr;
	u32		edcidsr;
};

static DEFINE_MUTEX(debug_lock);
static DEFINE_PER_CPU(struct debug_drvdata *, debug_drvdata);
static int debug_count;
static struct dentry *debug_debugfs_dir;

static struct pm_qos_request debug_qos_req;
static int idle_constraint = PM_QOS_DEFAULT_VALUE;
module_param(idle_constraint, int, 0600);
MODULE_PARM_DESC(idle_constraint, "Latency requirement in microseconds for CPU "
		 "idle states (default is -1, which means have no limiation "
		 "to CPU idle states; 0 means disabling all idle states; user "
		 "can choose other platform dependent values so can disable "
		 "specific idle states for the platform)");

static bool debug_enable;
module_param_named(enable, debug_enable, bool, 0600);
MODULE_PARM_DESC(enable, "Knob to enable debug functionality "
		 "(default is 0, which means is disabled by default)");

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

static void debug_force_cpu_powered_up(struct debug_drvdata *drvdata)
{
	int timeout = DEBUG_WAIT_TIMEOUT;

	drvdata->edprsr = readl_relaxed(drvdata->base + EDPRSR);

	CS_UNLOCK(drvdata->base);

	/* Bail out if CPU is powered up yet */
	if (drvdata->edprsr & EDPRSR_PU)
		goto out_powered_up;

	/*
	 * Send request to power management controller and assert
	 * DBGPWRUPREQ signal; if power management controller has
	 * sane implementation, it should enable CPU power domain
	 * in case CPU is in low power state.
	 */
	drvdata->edprsr = readl(drvdata->base + EDPRCR);
	drvdata->edprsr |= EDPRCR_COREPURQ;
	writel(drvdata->edprsr, drvdata->base + EDPRCR);

	/* Wait for CPU to be powered up (timeout~=32ms) */
	while (timeout--) {
		drvdata->edprsr = readl_relaxed(drvdata->base + EDPRSR);
		if (drvdata->edprsr & EDPRSR_PU)
			break;

		usleep_range(1000, 2000);
	}

	/*
	 * Unfortunately the CPU cannot be powered up, so return
	 * back and later has no permission to access other
	 * registers. For this case, should set 'idle_constraint'
	 * to ensure CPU power domain is enabled!
	 */
	if (!(drvdata->edprsr & EDPRSR_PU)) {
		pr_err("%s: power up request for CPU%d failed\n",
			__func__, drvdata->cpu);
		goto out;
	}

out_powered_up:
	debug_os_unlock(drvdata);

	/*
	 * At this point the CPU is powered up, so set the no powerdown
	 * request bit so we don't lose power and emulate power down.
	 */
	drvdata->edprsr = readl(drvdata->base + EDPRCR);
	drvdata->edprsr |= EDPRCR_COREPURQ | EDPRCR_CORENPDRQ;
	writel(drvdata->edprsr, drvdata->base + EDPRCR);

out:
	CS_LOCK(drvdata->base);
}

static void debug_read_regs(struct debug_drvdata *drvdata)
{
	/*
	 * Ensure CPU power domain is enabled to let registers
	 * are accessiable.
	 */
	debug_force_cpu_powered_up(drvdata);

	if (!debug_access_permitted(drvdata))
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
	if (drvdata->edpcsr == EDPCSR_PROHIBITED)
		goto out;

	/*
	 * A read of the EDPCSR normally has the side-effect of
	 * indirectly writing to EDCIDSR, EDVIDSR and EDPCSR_HI;
	 * at this point it's safe to read value from them.
	 */
	if (IS_ENABLED(CONFIG_64BIT))
		drvdata->edpcsr_hi = readl_relaxed(drvdata->base + EDPCSR_HI);

	if (drvdata->edcidsr_present)
		drvdata->edcidsr = readl_relaxed(drvdata->base + EDCIDSR);

	if (drvdata->edvidsr_present)
		drvdata->edvidsr = readl_relaxed(drvdata->base + EDVIDSR);

out:
	CS_LOCK(drvdata->base);
}

#ifndef CONFIG_64BIT
static unsigned long debug_adjust_pc(struct debug_drvdata *drvdata,
				     unsigned long pc)
{
	unsigned long arm_inst_offset = 0, thumb_inst_offset = 0;

	if (drvdata->pc_has_offset) {
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

	if (!debug_access_permitted(drvdata)) {
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

	if (drvdata->edcidsr_present)
		pr_emerg("\tEDCIDSR: %08x\n", drvdata->edcidsr);

	if (drvdata->edvidsr_present)
		pr_emerg("\tEDVIDSR: %08x (State:%s Mode:%s Width:%dbits VMID:%x)\n",
			 drvdata->edvidsr,
			 drvdata->edvidsr & EDVIDSR_NS ? "Non-secure" : "Secure",
			 drvdata->edvidsr & EDVIDSR_E3 ? "EL3" :
				(drvdata->edvidsr & EDVIDSR_E2 ? "EL2" : "EL1/0"),
			 drvdata->edvidsr & EDVIDSR_HV ? 64 : 32,
			 drvdata->edvidsr & (u32)EDVIDSR_VMID);
}

static void debug_init_arch_data(void *info)
{
	struct debug_drvdata *drvdata = info;
	u32 mode, pcsr_offset;

	CS_UNLOCK(drvdata->base);

	debug_os_unlock(drvdata);

	/* Read device info */
	drvdata->eddevid  = readl_relaxed(drvdata->base + EDDEVID);
	drvdata->eddevid1 = readl_relaxed(drvdata->base + EDDEVID1);

	CS_LOCK(drvdata->base);

	/* Parse implementation feature */
	mode = drvdata->eddevid & EDDEVID_PCSAMPLE_MODE;
	pcsr_offset = drvdata->eddevid1 & EDDEVID1_PCSR_OFFSET_MASK;

	if (mode == EDDEVID_IMPL_NONE) {
		drvdata->edpcsr_present  = false;
		drvdata->edcidsr_present = false;
		drvdata->edvidsr_present = false;
	} else if (mode == EDDEVID_IMPL_EDPCSR) {
		drvdata->edpcsr_present  = true;
		drvdata->edcidsr_present = false;
		drvdata->edvidsr_present = false;
	} else if (mode == EDDEVID_IMPL_EDPCSR_EDCIDSR) {
		/*
		 * In ARM DDI 0487A.k, the EDDEVID1.PCSROffset is used to
		 * define if has the offset for PC sampling value; if read
		 * back EDDEVID1.PCSROffset == 0x2, then this means the debug
		 * module does not sample the instruction set state when
		 * armv8 CPU in AArch32 state.
		 */
		if (!IS_ENABLED(CONFIG_64BIT) &&
			(pcsr_offset == EDDEVID1_PCSR_NO_OFFSET_DIS_AARCH32))
			drvdata->edpcsr_present = false;
		else
			drvdata->edpcsr_present = true;

		drvdata->edcidsr_present = true;
		drvdata->edvidsr_present = false;
	} else if (mode == EDDEVID_IMPL_FULL) {
		drvdata->edpcsr_present  = true;
		drvdata->edcidsr_present = true;
		drvdata->edvidsr_present = true;
	}

	if (IS_ENABLED(CONFIG_64BIT))
		drvdata->pc_has_offset = false;
	else
		drvdata->pc_has_offset =
			(pcsr_offset == EDDEVID1_PCSR_OFFSET_INS_SET);

	return;
}

/*
 * Dump out information on panic.
 */
static int debug_notifier_call(struct notifier_block *self,
			       unsigned long v, void *p)
{
	int cpu;
	struct debug_drvdata *drvdata;

	pr_emerg("ARM external debug module:\n");

	for_each_possible_cpu(cpu) {
		drvdata = per_cpu(debug_drvdata, cpu);
		if (!drvdata)
			continue;

		pr_emerg("CPU[%d]:\n", drvdata->cpu);

		debug_read_regs(drvdata);
		debug_dump_regs(drvdata);
	}

	return 0;
}

static struct notifier_block debug_notifier = {
	.notifier_call = debug_notifier_call,
};

static int debug_enable_func(void)
{
	int ret;

	pm_qos_add_request(&debug_qos_req,
		PM_QOS_CPU_DMA_LATENCY, idle_constraint);

	ret = atomic_notifier_chain_register(&panic_notifier_list,
					     &debug_notifier);
	if (ret)
		goto err;

	return 0;

err:
	pm_qos_remove_request(&debug_qos_req);
	return ret;
}

static void debug_disable_func(void)
{
	atomic_notifier_chain_unregister(&panic_notifier_list,
					 &debug_notifier);
	pm_qos_remove_request(&debug_qos_req);
}

static ssize_t debug_func_knob_write(struct file *f,
		const char __user *buf, size_t count, loff_t *ppos)
{
	u8 on;
	int ret;

	ret = kstrtou8_from_user(buf, count, 2, &on);
	if (ret)
		return ret;

	mutex_lock(&debug_lock);

	if (!on ^ debug_enable)
		goto out;

	if (on) {
		ret = debug_enable_func();
		if (ret) {
			pr_err("%s: unable to disable debug function: %d\n",
			       __func__, ret);
			goto err;
		}
	} else
		debug_disable_func();

	debug_enable = on;

out:
	ret = count;
err:
	mutex_unlock(&debug_lock);
	return ret;
}

static ssize_t debug_func_knob_read(struct file *f,
		char __user *ubuf, size_t count, loff_t *ppos)
{
	char val[] = { '0' + debug_enable, '\n' };

	return simple_read_from_buffer(ubuf, count, ppos, val, sizeof(val));
}

static ssize_t debug_idle_constraint_write(struct file *f,
		const char __user *buf, size_t count, loff_t *ppos)
{
	int val;
	ssize_t ret;

	ret = kstrtoint_from_user(buf, count, 10, &val);
	if (ret)
		return ret;

	mutex_lock(&debug_lock);
	idle_constraint = val;

	if (debug_enable)
		pm_qos_update_request(&debug_qos_req, idle_constraint);

	mutex_unlock(&debug_lock);
	return count;
}

static ssize_t debug_idle_constraint_read(struct file *f,
		char __user *ubuf, size_t count, loff_t *ppos)
{
	char buf[32];
	int len;

	if (*ppos)
		return 0;

	len = sprintf(buf, "%d\n", idle_constraint);
	return simple_read_from_buffer(ubuf, count, ppos, buf, len);
}

static const struct file_operations debug_func_knob_fops = {
	.open	= simple_open,
	.read	= debug_func_knob_read,
	.write	= debug_func_knob_write,
};

static const struct file_operations debug_idle_constraint_fops = {
	.open	= simple_open,
	.read	= debug_idle_constraint_read,
	.write	= debug_idle_constraint_write,
};

static int debug_func_init(void)
{
	struct dentry *file;
	int ret;

	/* Create debugfs node */
	debug_debugfs_dir = debugfs_create_dir("coresight_cpu_debug", NULL);
	if (!debug_debugfs_dir) {
		pr_err("%s: unable to create debugfs directory\n", __func__);
		return -ENOMEM;
	}

	file = debugfs_create_file("enable", S_IRUGO | S_IWUSR,
			debug_debugfs_dir, NULL, &debug_func_knob_fops);
	if (!file) {
		pr_err("%s: unable to create enable knob file\n", __func__);
		ret = -ENOMEM;
		goto err;
	}

	file = debugfs_create_file("idle_constraint", S_IRUGO | S_IWUSR,
			debug_debugfs_dir, NULL, &debug_idle_constraint_fops);
	if (!file) {
		pr_err("%s: unable to create idle constraint file\n", __func__);
		ret = -ENOMEM;
		goto err;
	}

	/* Use sysfs node to enable functionality */
	if (!debug_enable)
		return 0;

	/* Enable debug module at boot time */
	ret = debug_enable_func();
	if (ret) {
		pr_err("%s: unable to disable debug function: %d\n",
		       __func__, ret);
		goto err;
	}

	return 0;

err:
	debugfs_remove_recursive(debug_debugfs_dir);
	return ret;
}

static void debug_func_exit(void)
{
	debugfs_remove_recursive(debug_debugfs_dir);

	/* Disable functionality if has enabled */
	if (debug_enable)
		debug_disable_func();
}

static int debug_probe(struct amba_device *adev, const struct amba_id *id)
{
	void __iomem *base;
	struct device *dev = &adev->dev;
	struct debug_drvdata *drvdata;
	struct resource *res = &adev->res;
	struct device_node *np = adev->dev.of_node;
	int ret;

	drvdata = devm_kzalloc(dev, sizeof(*drvdata), GFP_KERNEL);
	if (!drvdata)
		return -ENOMEM;

	drvdata->cpu = np ? of_coresight_get_cpu(np) : 0;
	if (per_cpu(debug_drvdata, drvdata->cpu)) {
		dev_err(dev, "CPU's drvdata has been initialized\n");
		return -EBUSY;
	}

	drvdata->dev = &adev->dev;
	amba_set_drvdata(adev, drvdata);

	/* Validity for the resource is already checked by the AMBA core */
	base = devm_ioremap_resource(dev, res);
	if (IS_ERR(base))
		return PTR_ERR(base);

	drvdata->base = base;

	get_online_cpus();
	per_cpu(debug_drvdata, drvdata->cpu) = drvdata;
	ret = smp_call_function_single(drvdata->cpu,
				debug_init_arch_data, drvdata, 1);
	put_online_cpus();

	if (ret) {
		dev_err(dev, "Debug arch init failed\n");
		goto err;
	}

	if (!drvdata->edpcsr_present) {
		ret = -ENXIO;
		dev_err(dev, "Sample-based profiling is not implemented\n");
		goto err;
	}

	if (!debug_count++) {
		ret = debug_func_init();
		if (ret)
			goto err_func_init;
	}

	dev_info(dev, "Coresight debug-CPU%d initialized\n", drvdata->cpu);
	return 0;

err_func_init:
	debug_count--;
err:
	per_cpu(debug_drvdata, drvdata->cpu) = NULL;
	return ret;
}

static int debug_remove(struct amba_device *adev)
{
	struct debug_drvdata *drvdata = amba_get_drvdata(adev);

	per_cpu(debug_drvdata, drvdata->cpu) = NULL;

	if (!--debug_count)
		debug_func_exit();

	return 0;
}

static struct amba_id debug_ids[] = {
	{       /* Debug for Cortex-A53 */
		.id	= 0x000bbd03,
		.mask	= 0x000fffff,
	},
	{       /* Debug for Cortex-A57 */
		.id	= 0x000bbd07,
		.mask	= 0x000fffff,
	},
	{       /* Debug for Cortex-A72 */
		.id	= 0x000bbd08,
		.mask	= 0x000fffff,
	},
	{ 0, 0 },
};

static struct amba_driver debug_driver = {
	.drv = {
		.name   = "coresight-cpu-debug",
		.suppress_bind_attrs = true,
	},
	.probe		= debug_probe,
	.remove		= debug_remove,
	.id_table	= debug_ids,
};

module_amba_driver(debug_driver);

MODULE_AUTHOR("Leo Yan <leo.yan@linaro.org>");
MODULE_DESCRIPTION("ARM Coresight CPU Debug Driver");
MODULE_LICENSE("GPL");
