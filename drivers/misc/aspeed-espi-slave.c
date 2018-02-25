// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2012-2015, ASPEED Technology Inc.
 * Copyright (c) 2015-2018, Intel Corporation.
 */

#include <linux/atomic.h>
#include <linux/errno.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/poll.h>
#include <linux/regmap.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/timer.h>

#define DEVICE_NAME     "aspeed-espi-slave"

#define ESPI_CTRL                 0x00
#define      ESPI_CTRL_SW_RESET             GENMASK(31, 24)
#define      ESPI_CTRL_OOB_CHRDY            BIT(4)
#define ESPI_ISR                  0x08
#define      ESPI_ISR_HW_RESET              BIT(31)
#define      ESPI_ISR_VW_SYS_EVT1           BIT(22)
#define      ESPI_ISR_VW_SYS_EVT            BIT(8)
#define ESPI_IER                  0x0C
#define ESPI_SYS_IER              0x94
#define ESPI_SYS_EVENT            0x98
#define ESPI_SYS_INT_T0           0x110
#define ESPI_SYS_INT_T1           0x114
#define ESPI_SYS_INT_T2           0x118
#define ESPI_SYS_ISR              0x11C
#define      ESPI_SYSEVT_HOST_RST_ACK       BIT(27)
#define      ESPI_SYSEVT_SLAVE_BOOT_STATUS  BIT(23)
#define      ESPI_SYSEVT_SLAVE_BOOT_DONE    BIT(20)
#define      ESPI_SYSEVT_OOB_RST_ACK        BIT(16)
#define      ESPI_SYSEVT_HOST_RST_WARN      BIT(8)
#define      ESPI_SYSEVT_OOB_RST_WARN       BIT(6)
#define      ESPI_SYSEVT_PLT_RST_N          BIT(5)
#define ESPI_SYS1_IER             0x100
#define ESPI_SYS1_EVENT           0x104
#define ESPI_SYS1_INT_T0          0x120
#define ESPI_SYS1_INT_T1          0x124
#define ESPI_SYS1_INT_T2          0x128
#define ESPI_SYS1_ISR             0x12C
#define      ESPI_SYSEVT1_SUS_ACK           BIT(20)
#define      ESPI_SYSEVT1_SUS_WARN          BIT(0)

struct aspeed_espi_slave_data {
	struct regmap *map;
};

static void aspeed_espi_slave_sys_event(struct aspeed_espi_slave_data *priv)
{
	u32 sts, evt;

	if (regmap_read(priv->map, ESPI_SYS_ISR, &sts) != 0 ||
	    regmap_read(priv->map, ESPI_SYS_EVENT, &evt) != 0)
		return;

	if (sts & ESPI_SYSEVT_HOST_RST_WARN)
		regmap_update_bits_base(priv->map, ESPI_SYS_EVENT,
			ESPI_SYSEVT_HOST_RST_ACK,
			evt & ESPI_SYSEVT_HOST_RST_WARN ?
				ESPI_SYSEVT_HOST_RST_ACK : 0,
			NULL, false, true);

	if (sts & ESPI_SYSEVT_OOB_RST_WARN)
		regmap_update_bits_base(priv->map, ESPI_SYS_EVENT,
			ESPI_SYSEVT_OOB_RST_ACK,
			evt & ESPI_SYSEVT_OOB_RST_WARN ?
				ESPI_SYSEVT_OOB_RST_ACK : 0,
			NULL, false, true);

	regmap_write(priv->map, ESPI_SYS_ISR, sts);
}

static void aspeed_espi_slave_sys1_event(struct aspeed_espi_slave_data *priv)
{
	u32 sts;

	if (regmap_read(priv->map, ESPI_SYS1_ISR, &sts) != 0)
		return;

	if (sts & ESPI_SYSEVT1_SUS_WARN)
		regmap_update_bits_base(priv->map, ESPI_SYS1_EVENT,
			ESPI_SYSEVT1_SUS_ACK, ESPI_SYSEVT1_SUS_ACK,
			NULL, false, true);

	regmap_write(priv->map, ESPI_SYS1_ISR, sts);
}

static irqreturn_t aspeed_espi_slave_irq(int irq, void *arg)
{
	struct aspeed_espi_slave_data *priv = arg;
	u32 sts;

	if (regmap_read(priv->map, ESPI_ISR, &sts) != 0)
		return IRQ_NONE;

	if (sts & ESPI_ISR_HW_RESET) {
		regmap_update_bits_base(priv->map, ESPI_CTRL,
				ESPI_CTRL_SW_RESET, 0,
				NULL, false, true);
		regmap_update_bits_base(priv->map, ESPI_CTRL,
				ESPI_CTRL_SW_RESET, ESPI_CTRL_SW_RESET,
				NULL, false, true);

		regmap_update_bits_base(priv->map, ESPI_SYS_EVENT,
				ESPI_SYSEVT_SLAVE_BOOT_STATUS |
					ESPI_SYSEVT_SLAVE_BOOT_DONE,
				ESPI_SYSEVT_SLAVE_BOOT_STATUS |
					ESPI_SYSEVT_SLAVE_BOOT_DONE,
				NULL, false, true);
	}

	if (sts & ESPI_ISR_VW_SYS_EVT)
		aspeed_espi_slave_sys_event(priv);

	if (sts & ESPI_ISR_VW_SYS_EVT1)
		aspeed_espi_slave_sys1_event(priv);

	regmap_write(priv->map, ESPI_ISR, sts);

	return IRQ_HANDLED;
}

/* Setup Interrupt Type/Enable of System Event from Master
 *				   T2 T1 T0
 *  1). HOST_RST_WARN : Dual Edge   1  0  0
 *  2). OOB_RST_WARN  : Dual Edge   1  0  0
 *  3). PLTRST_N      : Dual Edge   1  0  0
 */
#define ESPI_SYS_INT_T0_SET        0x00000000
#define ESPI_SYS_INT_T1_SET        0x00000000
#define ESPI_SYS_INT_T2_SET \
(ESPI_SYSEVT_HOST_RST_WARN | ESPI_SYSEVT_OOB_RST_WARN | ESPI_SYSEVT_PLT_RST_N)
#define ESPI_SYS_INT_SET \
(ESPI_SYSEVT_HOST_RST_WARN | ESPI_SYSEVT_OOB_RST_WARN | ESPI_SYSEVT_PLT_RST_N)

/* Setup Interrupt Type/Enable of System Event 1 from Master
 *				   T2 T1 T0
 *  1). SUS_WARN    : Rising Edge   0  0  1
 */
#define ESPI_SYS1_INT_T0_SET        ESPI_SYSEVT1_SUS_WARN
#define ESPI_SYS1_INT_T1_SET        0x00000000
#define ESPI_SYS1_INT_T2_SET        0x00000000
#define ESPI_SYS1_INT_SET           ESPI_SYSEVT1_SUS_WARN

static int aspeed_espi_slave_config_irq(struct aspeed_espi_slave_data *priv,
			struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	int irq;
	int rc;

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;

	rc = devm_request_irq(dev, irq, aspeed_espi_slave_irq, IRQF_SHARED,
			dev_name(dev), priv);
	if (rc < 0)
		return rc;

	regmap_update_bits(priv->map, ESPI_CTRL, ESPI_CTRL_OOB_CHRDY,
			ESPI_CTRL_OOB_CHRDY);

	regmap_write(priv->map, ESPI_SYS_INT_T0, ESPI_SYS_INT_T0_SET);
	regmap_write(priv->map, ESPI_SYS_INT_T1, ESPI_SYS_INT_T1_SET);
	regmap_write(priv->map, ESPI_SYS_INT_T2, ESPI_SYS_INT_T2_SET);
	regmap_write(priv->map, ESPI_SYS_IER, ESPI_SYS_INT_SET);

	regmap_write(priv->map, ESPI_SYS1_INT_T0, ESPI_SYS1_INT_T0_SET);
	regmap_write(priv->map, ESPI_SYS1_INT_T1, ESPI_SYS1_INT_T1_SET);
	regmap_write(priv->map, ESPI_SYS1_INT_T2, ESPI_SYS1_INT_T2_SET);
	regmap_write(priv->map, ESPI_SYS1_IER, ESPI_SYS1_INT_SET);

	regmap_write(priv->map, ESPI_IER, 0xFFFFFFFF);

	return 0;
}

static void aspeed_espi_slave_boot_ack(struct aspeed_espi_slave_data *priv)
{
	u32 evt;

	if (regmap_read(priv->map, ESPI_SYS_EVENT, &evt) == 0 &&
	    (evt & ESPI_SYSEVT_SLAVE_BOOT_STATUS) == 0)
		regmap_write(priv->map, ESPI_SYS_EVENT, evt |
			ESPI_SYSEVT_SLAVE_BOOT_STATUS |
			ESPI_SYSEVT_SLAVE_BOOT_DONE);

	if (regmap_read(priv->map, ESPI_SYS1_EVENT, &evt) == 0 &&
	    (evt & ESPI_SYSEVT1_SUS_WARN) != 0)
		regmap_write(priv->map, ESPI_SYS1_EVENT, evt |
			ESPI_SYSEVT1_SUS_ACK);
}

static const struct regmap_config espi_slave_regmap_cfg = {
	.reg_bits     = 32,
	.reg_stride   = 4,
	.val_bits     = 32,
	.max_register = ESPI_SYS1_ISR,
};

static int aspeed_espi_slave_probe(struct platform_device *pdev)
{
	struct aspeed_espi_slave_data *priv;
	struct device *dev = &pdev->dev;
	struct resource *res;
	void __iomem *regs;
	int rc;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	regs = devm_ioremap_resource(dev, res);
	if (IS_ERR(regs))
		return PTR_ERR(regs);

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->map = devm_regmap_init_mmio(dev, regs, &espi_slave_regmap_cfg);
	if (IS_ERR(priv->map))
		return PTR_ERR(priv->map);

	dev_set_name(dev, DEVICE_NAME);

	rc = aspeed_espi_slave_config_irq(priv, pdev);
	if (rc)
		return rc;

	aspeed_espi_slave_boot_ack(priv);

	platform_set_drvdata(pdev, priv);

	return 0;
}

static const struct of_device_id of_espi_slave_match_table[] = {
	{ .compatible = "aspeed,ast2500-espi-slave" },
	{ }
};
MODULE_DEVICE_TABLE(of, of_espi_slave_match_table);

static struct platform_driver aspeed_espi_slave_driver = {
	.driver = {
		.name           = DEVICE_NAME,
		.of_match_table = of_espi_slave_match_table,
	},
	.probe = aspeed_espi_slave_probe,
};
module_platform_driver(aspeed_espi_slave_driver);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Haiyue Wang <haiyue.wang@linux.intel.com>");
MODULE_DESCRIPTION("Linux device interface to the eSPI slave");
