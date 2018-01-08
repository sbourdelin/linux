/*
 * Xilinx Zynq MPSoC Power Management
 *
 *  Copyright (C) 2014-2017 Xilinx, Inc.
 *
 *  Davorin Mista <davorin.mista@aggios.com>
 *  Jolly Shah <jollys@xilinx.com>
 *  Rajan Vaja <rajanv@xilinx.com>
 *
 * SPDX-License-Identifier:	GPL-2.0+
 */

#include <linux/compiler.h>
#include <linux/arm-smccc.h>
#include <linux/of.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/interrupt.h>
#include <linux/uaccess.h>
#include <linux/platform_device.h>
#include <linux/debugfs.h>
#include <linux/reboot.h>
#include <linux/suspend.h>
#include <linux/firmware/xilinx/zynqmp/firmware.h>

#define DRIVER_NAME	"zynqmp_pm"

/**
 * struct zynqmp_pm_work_struct - Wrapper for struct work_struct
 * @callback_work:	Work structure
 * @args:		Callback arguments
 */
struct zynqmp_pm_work_struct {
	struct work_struct callback_work;
	u32 args[CB_ARG_CNT];
};

static struct zynqmp_pm_work_struct *zynqmp_pm_init_suspend_work;

enum pm_suspend_mode {
	PM_SUSPEND_MODE_STD,
	PM_SUSPEND_MODE_POWER_OFF,
};

#define PM_SUSPEND_MODE_FIRST	PM_SUSPEND_MODE_STD

static const char *const suspend_modes[] = {
	[PM_SUSPEND_MODE_STD] = "standard",
	[PM_SUSPEND_MODE_POWER_OFF] = "power-off",
};

static enum pm_suspend_mode suspend_mode = PM_SUSPEND_MODE_STD;

enum pm_api_cb_id {
	PM_INIT_SUSPEND_CB = 30,
	PM_ACKNOWLEDGE_CB,
	PM_NOTIFY_CB,
};

static irqreturn_t zynqmp_pm_isr(int irq, void *data)
{
	u32 payload[CB_PAYLOAD_SIZE];
	const struct zynqmp_eemi_ops *eemi_ops = get_eemi_ops();

	if (!eemi_ops || !eemi_ops->get_callback_data)
		return IRQ_NONE;

	eemi_ops->get_callback_data(payload);

	if (!payload[0])
		return IRQ_NONE;

	/* First element is callback API ID, others are callback arguments */
	if (payload[0] == PM_INIT_SUSPEND_CB) {
		if (work_pending(&zynqmp_pm_init_suspend_work->callback_work))
			goto done;

		/* Copy callback arguments into work's structure */
		memcpy(zynqmp_pm_init_suspend_work->args, &payload[1],
		       sizeof(zynqmp_pm_init_suspend_work->args));

		queue_work(system_unbound_wq,
			   &zynqmp_pm_init_suspend_work->callback_work);
	}

done:
	return IRQ_HANDLED;
}

static const struct of_device_id pm_of_match[] = {
	{ .compatible = "xlnx,zynqmp-pm", },
	{ /* end of table */ },
};

MODULE_DEVICE_TABLE(of, pm_of_match);

/**
 * zynqmp_pm_init_suspend_work_fn - Initialize suspend
 * @work:	Pointer to work_struct
 *
 * Bottom-half of PM callback IRQ handler.
 */
static void zynqmp_pm_init_suspend_work_fn(struct work_struct *work)
{
	struct zynqmp_pm_work_struct *pm_work =
		container_of(work, struct zynqmp_pm_work_struct, callback_work);

	if (pm_work->args[0] == ZYNQMP_PM_SUSPEND_REASON_SYSTEM_SHUTDOWN) {
		orderly_poweroff(true);
	} else if (pm_work->args[0] ==
		   ZYNQMP_PM_SUSPEND_REASON_POWER_UNIT_REQUEST) {
		pm_suspend(PM_SUSPEND_MEM);
	} else {
		pr_err("%s Unsupported InitSuspendCb reason code %d.\n"
		       , __func__, pm_work->args[0]);
	}
}

static ssize_t suspend_mode_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	char *s = buf;
	int md;

	for (md = PM_SUSPEND_MODE_FIRST; md < ARRAY_SIZE(suspend_modes); md++)
		if (suspend_modes[md]) {
			if (md == suspend_mode)
				s += sprintf(s, "[%s] ", suspend_modes[md]);
			else
				s += sprintf(s, "%s ", suspend_modes[md]);
		}

	/* Convert last space to newline */
	if (s != buf)
		*(s - 1) = '\n';
	return (s - buf);
}

static ssize_t suspend_mode_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t count)
{
	int md, ret = -EINVAL;
	const struct zynqmp_eemi_ops *eemi_ops = get_eemi_ops();

	if (!eemi_ops || !eemi_ops->set_suspend_mode)
		return ret;

	for (md = PM_SUSPEND_MODE_FIRST; md < ARRAY_SIZE(suspend_modes); md++)
		if (suspend_modes[md] &&
		    sysfs_streq(suspend_modes[md], buf)) {
			ret = 0;
			break;
		}

	if (!ret && md != suspend_mode) {
		ret = eemi_ops->set_suspend_mode(md);
		if (likely(!ret))
			suspend_mode = md;
	}

	return ret ? ret : count;
}

static DEVICE_ATTR_RW(suspend_mode);

/**
 * zynqmp_pm_sysfs_init - Initialize PM driver sysfs interface
 * @dev:	Pointer to device structure
 *
 * Return:	0 on success, negative error code otherwise
 */
static int zynqmp_pm_sysfs_init(struct device *dev)
{
	return sysfs_create_file(&dev->kobj, &dev_attr_suspend_mode.attr);
}

/**
 * zynqmp_pm_probe - Probe existence of the PMU Firmware
 *			and initialize debugfs interface
 *
 * @pdev:	Pointer to the platform_device structure
 *
 * Return:	Returns 0 on success
 *		Negative error code otherwise
 */
static int zynqmp_pm_probe(struct platform_device *pdev)
{
	int ret, irq;
	u32 pm_api_version;
	const struct zynqmp_eemi_ops *eemi_ops = get_eemi_ops();

	if (!eemi_ops || !eemi_ops->get_api_version)
		return -ENXIO;

	eemi_ops->get_api_version(&pm_api_version);

	/* Check PM API version number */
	if (pm_api_version != ZYNQMP_PM_VERSION)
		return -ENODEV;

	irq = platform_get_irq(pdev, 0);
	if (irq <= 0)
		return -ENXIO;

	ret = request_irq(irq, zynqmp_pm_isr, IRQF_SHARED, DRIVER_NAME, pdev);
	if (ret) {
		dev_err(&pdev->dev, "request_irq '%d' failed with %d\n",
			irq, ret);
		return ret;
	}

	zynqmp_pm_init_suspend_work =
		devm_kzalloc(&pdev->dev, sizeof(struct zynqmp_pm_work_struct),
			     GFP_KERNEL);
	if (!zynqmp_pm_init_suspend_work) {
		ret = -ENOMEM;
		goto error;
	}

	INIT_WORK(&zynqmp_pm_init_suspend_work->callback_work,
		  zynqmp_pm_init_suspend_work_fn);

	ret = zynqmp_pm_sysfs_init(&pdev->dev);
	if (ret) {
		dev_err(&pdev->dev, "unable to initialize sysfs interface\n");
		goto error;
	}

	dev_info(&pdev->dev, "Power management API v%d.%d\n",
		 ZYNQMP_PM_VERSION_MAJOR, ZYNQMP_PM_VERSION_MINOR);

	return 0;

error:
	free_irq(irq, 0);
	return ret;
}

static struct platform_driver zynqmp_pm_platform_driver = {
	.probe   = zynqmp_pm_probe,
	.driver  = {
			.name             = DRIVER_NAME,
			.of_match_table   = pm_of_match,
		   },
};
builtin_platform_driver(zynqmp_pm_platform_driver);

/**
 * zynqmp_pm_init - Notify PM firmware that initialization is completed
 *
 * Return:	Status returned from the PM firmware
 */
static int __init zynqmp_pm_init(void)
{
	const struct zynqmp_eemi_ops *eemi_ops = get_eemi_ops();

	if (!eemi_ops || !eemi_ops->init_finalize)
		return -ENXIO;

	return eemi_ops->init_finalize();
}

late_initcall_sync(zynqmp_pm_init);
