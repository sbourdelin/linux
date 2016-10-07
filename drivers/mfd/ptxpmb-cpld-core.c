/*
 * Juniper PTX PMB CPLD multi-function core driver
 *
 * Copyright (C) 2012 Juniper Networks
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/delay.h>
#include <linux/sched.h>
#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/dmi.h>
#include <linux/mfd/core.h>
#include <linux/of_device.h>
#include <linux/mfd/ptxpmb_cpld.h>
#include <linux/jnx/jnx-subsys.h>
#include <linux/jnx/board_ids.h>

struct pmb_cpld_core {
	struct device		*dev;
	struct pmb_boot_cpld __iomem *cpld;
	spinlock_t		lock;
	int			irq;
	wait_queue_head_t	wqh;
};

static const struct of_device_id pmb_cpld_of_ids[] = {
	{ .compatible = "jnx,ptxpmb-cpld", .data = (void *)CPLD_TYPE_PTXPMB },
	{ .compatible = "jnx,ngpmb-bcpld", .data = (void *)CPLD_TYPE_NGPMB },
	{ }
};
MODULE_DEVICE_TABLE(of, pmb_cpld_of_ids);

static struct dmi_system_id gld_2t_dmi_data[] = {
	{
		.ident = "Juniper Networks Gladiator 2T FPC",
		.matches = {
			    DMI_MATCH(DMI_SYS_VENDOR, "Juniper Networks Inc."),
			    DMI_MATCH(DMI_PRODUCT_NAME, "0BF9"),
			},
	},
	{},
};
MODULE_DEVICE_TABLE(dmi, gld_2t_dmi_data);

static struct dmi_system_id gld_3t_dmi_data[] = {
	{
		.ident = "Juniper Networks Gladiator 3T FPC",
		.matches = {
			    DMI_MATCH(DMI_SYS_VENDOR, "Juniper Networks Inc."),
			    DMI_MATCH(DMI_PRODUCT_NAME, "0BFA"),
			},
	},
	{},
};
MODULE_DEVICE_TABLE(dmi, gld_3t_dmi_data);

static int ptxpmb_cpld_get_master(void *data)
{
	struct pmb_cpld_core *cpld = data;
	u8 s1;

	s1 = ioread8(&cpld->cpld->i2c_host_sel) & CPLD_I2C_HOST_MSTR_MASK;

	if ((s1 & CPLD_I2C_HOST0_MSTR) == CPLD_I2C_HOST0_MSTR)
		return 0;

	if ((s1 & CPLD_I2C_HOST1_MSTR) == CPLD_I2C_HOST1_MSTR)
		return 1;

	return -1;
}

static int ngpmb_cpld_get_master(void *data)
{
	struct pmb_cpld_core *cpld = data;

	if (ioread8(&cpld->cpld->baseboard_status1) & NGPMB_MASTER_SELECT)
		return 1;
	else
		return 0;
}

static irqreturn_t pmb_cpld_core_interrupt(int irq, void *dev_data)
{
	struct pmb_cpld_core *cpld = dev_data;

	pr_info("pmb_cpld_core_interrupt %d\n", irq);

	spin_lock(&cpld->wqh.lock);

	/* clear interrupt, wake up any handlers */
	wake_up_locked(&cpld->wqh);

	spin_unlock(&cpld->wqh.lock);

	return IRQ_HANDLED;
}

static struct resource pmb_cpld_resources[] = {
	{
		.start	= 0,
		.end	= sizeof(struct pmb_boot_cpld) - 1,
		.flags	= IORESOURCE_MEM,
	},
};

static struct mfd_cell pmb_cpld_cells[] = {
	{
		.name = "jnx-ptxpmb-wdt",
		.num_resources = ARRAY_SIZE(pmb_cpld_resources),
		.resources = pmb_cpld_resources,
		.of_compatible = "jnx,ptxpmb-wdt",
	},
	{
		.name = "i2c-mux-ptxpmb-cpld",
		.num_resources = ARRAY_SIZE(pmb_cpld_resources),
		.resources = pmb_cpld_resources,
		.of_compatible = "jnx,i2c-mux-ptxpmb-cpld",
	},
	{
		.name = "gpio-ptxpmb-cpld",
		.num_resources = ARRAY_SIZE(pmb_cpld_resources),
		.resources = pmb_cpld_resources,
		.of_compatible = "jnx,gpio-ptxpmb-cpld",
	},
};

static struct mfd_cell ngpmb_cpld_cells[] = {
	{
		.name = "jnx-ptxpmb-wdt",
		.num_resources = ARRAY_SIZE(pmb_cpld_resources),
		.resources = pmb_cpld_resources,
		.of_compatible = "jnx,ptxpmb-wdt",
	},
	{
		.name = "i2c-mux-ngpmb-bcpld",
		.num_resources = ARRAY_SIZE(pmb_cpld_resources),
		.resources = pmb_cpld_resources,
		.of_compatible = "jnx,i2c-mux-ngpmb-bcpld",
	},
	{
		.name = "gpio-ptxpmb-cpld",
		.num_resources = ARRAY_SIZE(pmb_cpld_resources),
		.resources = pmb_cpld_resources,
		.of_compatible = "jnx,gpio-ptxpmb-cpld",
	},
};

static void cpld_ngpmb_init(struct pmb_cpld_core *cpld,
			    struct jnx_chassis_info *chinfo,
			    struct jnx_card_info *cinfo)
{
	u8 s1, s2, val, chassis;

	s1 = ioread8(&cpld->cpld->baseboard_status1);
	s2 = ioread8(&cpld->cpld->baseboard_status2);
	chassis = (ioread8(&cpld->cpld->board.ngpmb.chassis_type)
		   & NGPMB_CHASSIS_TYPE_MASK) >> NGPMB_CHASSIS_TYPE_LSB;

	dev_info(cpld->dev, "Revision 0x%02X chassis type %s (0x%02X)\n",
		 ioread8(&cpld->cpld->cpld_rev),
		 chassis == NGPMB_CHASSIS_TYPE_POLARIS ? "PTX-1000" :
		 chassis == NGPMB_CHASSIS_TYPE_HENDRICKS ? "PTX-3000" :
		 "Unknown", chassis);

	/* Only the Gladiator 2t/3t FPC */
	if (dmi_check_system(gld_2t_dmi_data) ||
	    dmi_check_system(gld_3t_dmi_data)) {
		/* Take SAM FPGA out of reset */
		val = ioread8(&cpld->cpld->gpio_2);
		iowrite8(val | NGPMB_GPIO2_TO_BASEBRD_LSB, &cpld->cpld->gpio_2);
		mdelay(10);
	} else {
		/*
		 * Get the PAM FPGA out of reset,
		 * and wait for 100ms as per HW manual
		 */
		val = ioread8(&cpld->cpld->reset);
		iowrite8(val & ~NGPMB_PCIE_OTHER_RESET, &cpld->cpld->reset);
		mdelay(100);
	}

	/* No Card / Chassis info needed in stand alone mode */
	if (!(s1 & NGPMB_PMB_STANDALONE) || !(s1 & NGPMB_BASEBRD_STANDALONE))
		return;

	cinfo->type = JNX_BOARD_TYPE_FPC;
	cinfo->slot = (s1 & NGPMB_BASEBRD_SLOT_MASK) >> NGPMB_BASEBRD_SLOT_LSB;

	if (((s2 & NGPMB_BASEBRD_TYPE_MASK) >> NGPMB_BASEBRD_TYPE_LSB) !=
	    NGPMB_BASEBRD_TYPE_MX) {
		if (dmi_check_system(gld_2t_dmi_data))
			cinfo->assembly_id = JNX_ID_GLD_2T_FPC;
		else if (dmi_check_system(gld_3t_dmi_data))
			cinfo->assembly_id = JNX_ID_GLD_3T_FPC;
		else
			cinfo->assembly_id = JNX_ID_POLARIS_MLC;
	}

	/*
	 * Multi chassis configuration. These bits are not
	 * valid for Gladiator.
	 */
	if (!(dmi_check_system(gld_2t_dmi_data) ||
	      dmi_check_system(gld_3t_dmi_data))) {
		if (ioread8(&cpld->cpld->board.ngpmb.sys_config) &
		    NGPMB_SYS_CONFIG_MULTI_CHASSIS) {
			chinfo->multichassis = 1;
			chinfo->chassis_no =
			ioread8(&cpld->cpld->board.ngpmb.chassis_id);
		}
	}

	switch (chassis) {
	case NGPMB_CHASSIS_TYPE_POLARIS:
		chinfo->platform = JNX_PRODUCT_POLARIS;
		break;
	case NGPMB_CHASSIS_TYPE_HENDRICKS:
		chinfo->platform = JNX_PRODUCT_HENDRICKS;
		break;
	default:
		chinfo->platform = 0;
		break;
	};
	chinfo->get_master = ngpmb_cpld_get_master;
}

static void cpld_ptxpmb_init(struct pmb_cpld_core *cpld,
			     struct jnx_chassis_info *chinfo,
			     struct jnx_card_info *cinfo)
{
	u8 s1, s2;

	s1 = ioread8(&cpld->cpld->baseboard_status1);
	s2 = ioread8(&cpld->cpld->baseboard_status2);

	dev_info(cpld->dev, "Revision 0x%02x carrier type 0x%x [%s]\n",
		 ioread8(&cpld->cpld->cpld_rev), s2 & 0x1f,
		 (s1 & 0X3F) == 0X1F ? "standalone"
				     : (s2 & 0x10) ? "FPC" : "SPMB");

	if ((s1 & 0x3f) != 0x1f) {	/* not standalone */
		cinfo->slot = s1 & 0x0f;
		if (s2 & 0x10) {	/* fpc */
			cinfo->type = JNX_BOARD_TYPE_FPC;
			switch (s2 & 0x0f) {
			case 0x00:	/* Sangria */
				cinfo->assembly_id = JNX_ID_SNG_VDV_BASE_P2;
				chinfo->platform = JNX_PRODUCT_SANGRIA;
				break;
			case 0x01:	/* Tiny */
				chinfo->platform = JNX_PRODUCT_TINY;
				break;
			case 0x02:	/* Hercules */
				chinfo->platform = JNX_PRODUCT_HERCULES;
				break;
			case 0x03:      /* Hendricks */
				cinfo->assembly_id = JNX_ID_HENDRICKS_FPC_P2;
				chinfo->platform = JNX_PRODUCT_HENDRICKS;
				break;
			default:	/* unknown */
				break;
			}
		} else {		/* spmb */
			cinfo->type = JNX_BOARD_TYPE_SPMB;
			switch (s2 & 0x0f) {
			case 0x00:	/* Sangria */
				cinfo->assembly_id = JNX_ID_SNG_PMB;
				chinfo->platform = JNX_PRODUCT_SANGRIA;
				break;
			default:	/* unknown */
				break;
			}
		}
	}
	chinfo->get_master = ptxpmb_cpld_get_master;
}

static int pmb_cpld_core_probe(struct platform_device *pdev)
{
	static struct pmb_cpld_core *cpld;
	struct device *dev = &pdev->dev;
	struct resource *res;
	struct ptxpmb_mux_data *pdata = dev->platform_data;
	int i, error, mfd_size;
	int cpld_type = CPLD_TYPE_PTXPMB;
	const struct of_device_id *match;
	struct mfd_cell *mfd_cells;

	struct jnx_chassis_info chinfo = {
		.chassis_no = 0,
		.multichassis = 0,
		.master_data = NULL,
		.platform = -1,
		.get_master = NULL,
	};
	struct jnx_card_info cinfo = {
		.type = JNX_BOARD_TYPE_UNKNOWN,
		.slot = -1,
		.assembly_id = -1,
	};

	cpld = devm_kzalloc(dev, sizeof(*cpld), GFP_KERNEL);
	if (!cpld)
		return -ENOMEM;

	cpld->dev = dev;
	dev_set_drvdata(dev, cpld);

	if (pdata) {
		cpld_type = pdata->cpld_type;
	} else {
		match = of_match_device(pmb_cpld_of_ids, dev);
		if (match)
			cpld_type = (int)(unsigned long)match->data;
	}

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	cpld->cpld = devm_ioremap_resource(dev, res);
	if (IS_ERR(cpld->cpld))
		return PTR_ERR(cpld->cpld);

	chinfo.master_data = cpld;

	cpld->irq = platform_get_irq(pdev, 0);
	if (cpld->irq >= 0) {
		error = devm_request_threaded_irq(dev, cpld->irq, NULL,
						  pmb_cpld_core_interrupt,
						  IRQF_TRIGGER_RISING |
						  IRQF_ONESHOT,
						  dev_name(dev), cpld);
		if (error < 0)
			return error;
	}

	spin_lock_init(&cpld->lock);
	init_waitqueue_head(&cpld->wqh);

	mfd_cells = pmb_cpld_cells;
	mfd_size = ARRAY_SIZE(pmb_cpld_cells);

	switch (cpld_type) {
	case CPLD_TYPE_PTXPMB:
		cpld_ptxpmb_init(cpld, &chinfo, &cinfo);
		break;
	case CPLD_TYPE_NGPMB:
		cpld_ngpmb_init(cpld, &chinfo, &cinfo);
		mfd_cells = ngpmb_cpld_cells;
		mfd_size = ARRAY_SIZE(ngpmb_cpld_cells);
		break;
	}

	if (pdata) {
		for (i = 0; i < mfd_size; i++) {
			mfd_cells[i].platform_data = pdata;
			mfd_cells[i].pdata_size = sizeof(*pdata);
		}
	}

	error = mfd_add_devices(dev, pdev->id, mfd_cells,
				mfd_size, res, 0, NULL);
	if (error < 0)
		return error;

	jnx_register_chassis(&chinfo);
	jnx_register_local_card(&cinfo);

	return 0;
}

static int pmb_cpld_core_remove(struct platform_device *pdev)
{
	jnx_unregister_local_card();
	jnx_unregister_chassis();
	mfd_remove_devices(&pdev->dev);
	return 0;
}

static struct platform_driver pmb_cpld_core_driver = {
	.probe		= pmb_cpld_core_probe,
	.remove		= pmb_cpld_core_remove,
	.driver		= {
		.name	= "ptxpmb-cpld",
		.of_match_table = pmb_cpld_of_ids,
		.owner	= THIS_MODULE,
	}
};

module_platform_driver(pmb_cpld_core_driver);

MODULE_DESCRIPTION("Juniper PTX PMB CPLD Core Driver");
MODULE_AUTHOR("Guenter Roeck <groeck@juniper.net>");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:ptxpmb-cpld");
