// SPDX-License-Identifier: GPL-2.0-or-later
//
// Copyright (C) 2018 ROHM Semiconductors
//
// ROHM BD70528 PMIC driver

#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/mfd/core.h>
#include <linux/mfd/rohm-bd70528.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/regmap.h>
#include <linux/types.h>

#define BD70528_INT_RES(_reg, _name)		\
	{					\
		.start = (_reg),		\
		.end = (_reg),			\
		.name = (_name),		\
		.flags = IORESOURCE_IRQ,	\
	}

static const struct resource rtc_irqs[] = {
	BD70528_INT_RES(BD70528_INT_RTC_ALARM, "bd70528-rtc-alm"),
	BD70528_INT_RES(BD70528_INT_ELPS_TIM, "bd70528-elapsed-timer"),
};

static const struct resource charger_irqs[] = {
	BD70528_INT_RES(BD70528_INT_BAT_OV_RES, "bd70528-bat-ov-res"),
	BD70528_INT_RES(BD70528_INT_BAT_OV_DET, "bd70528-bat-ov-det"),
	BD70528_INT_RES(BD70528_INT_DBAT_DET, "bd70528-bat-dead"),
	BD70528_INT_RES(BD70528_INT_BATTSD_COLD_RES, "bd70528-bat-warmed"),
	BD70528_INT_RES(BD70528_INT_BATTSD_COLD_DET, "bd70528-bat-cold"),
	BD70528_INT_RES(BD70528_INT_BATTSD_HOT_RES, "bd70528-bat-cooled"),
	BD70528_INT_RES(BD70528_INT_BATTSD_HOT_DET, "bd70528-bat-hot"),
	BD70528_INT_RES(BD70528_INT_CHG_TSD, "bd70528-chg-tshd"),
	BD70528_INT_RES(BD70528_INT_BAT_RMV, "bd70528-bat-removed"),
	BD70528_INT_RES(BD70528_INT_BAT_DET, "bd70528-bat-detected"),
	BD70528_INT_RES(BD70528_INT_DCIN2_OV_RES, "bd70528-dcin2-ov-res"),
	BD70528_INT_RES(BD70528_INT_DCIN2_OV_DET, "bd70528-dcin2-ov-det"),
	BD70528_INT_RES(BD70528_INT_DCIN2_RMV, "bd70528-dcin2-removed"),
	BD70528_INT_RES(BD70528_INT_DCIN2_DET, "bd70528-dcin2-detected"),
	BD70528_INT_RES(BD70528_INT_DCIN1_RMV, "bd70528-dcin1-removed"),
	BD70528_INT_RES(BD70528_INT_DCIN1_DET, "bd70528-dcin1-detected"),
};

static struct mfd_cell bd70528_mfd_cells[] = {
	{ .name = "bd70528-pmic", },
	{ .name = "bd70528-gpio", },
	/*
	 * We use BD71837 driver to drive the clk block. Only differences to
	 * BD70528 clock gate are the register address and mask.
	 */
	{ .name = "bd718xx-clk", },
	{ .name = "bd70528-wdt", },
	{
		.name = "bd70528-power",
		.resources = &charger_irqs[0],
		.num_resources = ARRAY_SIZE(charger_irqs),
	},
	{
		.name = "bd70528-rtc",
		.resources = &rtc_irqs[0],
		.num_resources = ARRAY_SIZE(rtc_irqs),
	},
};

static const struct regmap_range volatile_ranges[] = {
	/* IRQ regs */
	{
		.range_min = BD70528_REG_INT_MAIN,
		.range_max = BD70528_REG_INT_OP_FAIL,
	},
	/* RTC regs */
	{
		.range_min = BD70528_REG_RTC_COUNT_H,
		.range_max = BD70528_REG_RTC_ALM_REPEAT,
	},
	/*
	 * WDT control reg is special. Magic values must be
	 * written to it in order to change the control. Should
	 * not be cached.
	 */
	{
		.range_min = BD70528_REG_WDT_CTRL,
		.range_max = BD70528_REG_WDT_CTRL,
	},
	/*
	 * bd70528 contains also few other registers which require
	 * magic sequence to be written in order to update the value.
	 * At least SHIPMODE, HWRESET, WARMRESET,and STANDBY
	 */
	{
		.range_min = BD70528_REG_SHIPMODE,
		.range_max = BD70528_REG_STANDBY,
	},
};

static const struct regmap_access_table volatile_regs = {
	.yes_ranges = &volatile_ranges[0],
	.n_yes_ranges = ARRAY_SIZE(volatile_ranges),
};

static struct regmap_config bd70528_regmap = {
	.reg_bits = 8,
	.val_bits = 8,
	.volatile_table = &volatile_regs,
	.max_register = BD70528_MAX_REGISTER,
	.cache_type = REGCACHE_RBTREE,
};
/* bit [0] - Shutdown register */
unsigned int bit0_offsets[] = {0};
/* bit [1] - Power failure register */
unsigned int bit1_offsets[] = {1};
/* bit [2] - VR FAULT register */
unsigned int bit2_offsets[] = {2};
/* bit [3] - PMU register interrupts */
unsigned int bit3_offsets[] = {3};
/* bit [4] - Charger 1 and Charger 2 registers */
unsigned int bit4_offsets[] = {4, 5};
/* bit [5] - RTC register */
unsigned int bit5_offsets[] = {6};
/* bit [6] - GPIO register */
unsigned int bit6_offsets[] = {7};
/* bit [7] - Invalid operation register */
unsigned int bit7_offsets[] = {8};

static struct regmap_irq_sub_irq_map bd70528_sub_irq_offsets[] = {
	REGMAP_IRQ_MAIN_REG_OFFSET(bit0_offsets),
	REGMAP_IRQ_MAIN_REG_OFFSET(bit1_offsets),
	REGMAP_IRQ_MAIN_REG_OFFSET(bit2_offsets),
	REGMAP_IRQ_MAIN_REG_OFFSET(bit3_offsets),
	REGMAP_IRQ_MAIN_REG_OFFSET(bit4_offsets),
	REGMAP_IRQ_MAIN_REG_OFFSET(bit5_offsets),
	REGMAP_IRQ_MAIN_REG_OFFSET(bit6_offsets),
	REGMAP_IRQ_MAIN_REG_OFFSET(bit7_offsets),
};

static struct regmap_irq irqs[] = {
	REGMAP_IRQ_REG(BD70528_INT_LONGPUSH, 0, BD70528_INT_LONGPUSH_MASK),
	REGMAP_IRQ_REG(BD70528_INT_WDT, 0, BD70528_INT_WDT_MASK),
	REGMAP_IRQ_REG(BD70528_INT_HWRESET, 0, BD70528_INT_HWRESET_MASK),
	REGMAP_IRQ_REG(BD70528_INT_RSTB_FAULT, 0, BD70528_INT_RSTB_FAULT_MASK),
	REGMAP_IRQ_REG(BD70528_INT_VBAT_UVLO, 0, BD70528_INT_VBAT_UVLO_MASK),
	REGMAP_IRQ_REG(BD70528_INT_TSD, 0, BD70528_INT_TSD_MASK),
	REGMAP_IRQ_REG(BD70528_INT_RSTIN, 0, BD70528_INT_RSTIN_MASK),
	REGMAP_IRQ_REG(BD70528_INT_BUCK1_FAULT, 1,
		       BD70528_INT_BUCK1_FAULT_MASK),
	REGMAP_IRQ_REG(BD70528_INT_BUCK2_FAULT, 1,
		       BD70528_INT_BUCK2_FAULT_MASK),
	REGMAP_IRQ_REG(BD70528_INT_BUCK3_FAULT, 1,
		       BD70528_INT_BUCK3_FAULT_MASK),
	REGMAP_IRQ_REG(BD70528_INT_LDO1_FAULT, 1, BD70528_INT_LDO1_FAULT_MASK),
	REGMAP_IRQ_REG(BD70528_INT_LDO2_FAULT, 1, BD70528_INT_LDO2_FAULT_MASK),
	REGMAP_IRQ_REG(BD70528_INT_LDO3_FAULT, 1, BD70528_INT_LDO3_FAULT_MASK),
	REGMAP_IRQ_REG(BD70528_INT_LED1_FAULT, 1, BD70528_INT_LED1_FAULT_MASK),
	REGMAP_IRQ_REG(BD70528_INT_LED2_FAULT, 1, BD70528_INT_LED2_FAULT_MASK),
	REGMAP_IRQ_REG(BD70528_INT_BUCK1_OCP, 2, BD70528_INT_BUCK1_OCP_MASK),
	REGMAP_IRQ_REG(BD70528_INT_BUCK2_OCP, 2, BD70528_INT_BUCK2_OCP_MASK),
	REGMAP_IRQ_REG(BD70528_INT_BUCK3_OCP, 2, BD70528_INT_BUCK3_OCP_MASK),
	REGMAP_IRQ_REG(BD70528_INT_LED1_OCP, 2, BD70528_INT_LED1_OCP_MASK),
	REGMAP_IRQ_REG(BD70528_INT_LED2_OCP, 2, BD70528_INT_LED2_OCP_MASK),
	REGMAP_IRQ_REG(BD70528_INT_BUCK1_FULLON, 2,
		       BD70528_INT_BUCK1_FULLON_MASK),
	REGMAP_IRQ_REG(BD70528_INT_BUCK2_FULLON, 2,
		       BD70528_INT_BUCK2_FULLON_MASK),
	REGMAP_IRQ_REG(BD70528_INT_SHORTPUSH, 3, BD70528_INT_SHORTPUSH_MASK),
	REGMAP_IRQ_REG(BD70528_INT_AUTO_WAKEUP, 3,
		       BD70528_INT_AUTO_WAKEUP_MASK),
	REGMAP_IRQ_REG(BD70528_INT_STATE_CHANGE, 3,
		       BD70528_INT_STATE_CHANGE_MASK),
	REGMAP_IRQ_REG(BD70528_INT_BAT_OV_RES, 4, BD70528_INT_BAT_OV_RES_MASK),
	REGMAP_IRQ_REG(BD70528_INT_BAT_OV_DET, 4, BD70528_INT_BAT_OV_DET_MASK),
	REGMAP_IRQ_REG(BD70528_INT_DBAT_DET, 4, BD70528_INT_DBAT_DET_MASK),
	REGMAP_IRQ_REG(BD70528_INT_BATTSD_COLD_RES, 4,
		       BD70528_INT_BATTSD_COLD_RES_MASK),
	REGMAP_IRQ_REG(BD70528_INT_BATTSD_COLD_DET, 4,
		       BD70528_INT_BATTSD_COLD_DET_MASK),
	REGMAP_IRQ_REG(BD70528_INT_BATTSD_HOT_RES, 4,
		       BD70528_INT_BATTSD_HOT_RES_MASK),
	REGMAP_IRQ_REG(BD70528_INT_BATTSD_HOT_DET, 4,
		       BD70528_INT_BATTSD_HOT_DET_MASK),
	REGMAP_IRQ_REG(BD70528_INT_CHG_TSD, 4, BD70528_INT_CHG_TSD_MASK),
	REGMAP_IRQ_REG(BD70528_INT_BAT_RMV, 5, BD70528_INT_BAT_RMV_MASK),
	REGMAP_IRQ_REG(BD70528_INT_BAT_DET, 5, BD70528_INT_BAT_DET_MASK),
	REGMAP_IRQ_REG(BD70528_INT_DCIN2_OV_RES, 5,
		       BD70528_INT_DCIN2_OV_RES_MASK),
	REGMAP_IRQ_REG(BD70528_INT_DCIN2_OV_DET, 5,
		       BD70528_INT_DCIN2_OV_DET_MASK),
	REGMAP_IRQ_REG(BD70528_INT_DCIN2_RMV, 5, BD70528_INT_DCIN2_RMV_MASK),
	REGMAP_IRQ_REG(BD70528_INT_DCIN2_DET, 5, BD70528_INT_DCIN2_DET_MASK),
	REGMAP_IRQ_REG(BD70528_INT_DCIN1_RMV, 5, BD70528_INT_DCIN1_RMV_MASK),
	REGMAP_IRQ_REG(BD70528_INT_DCIN1_DET, 5, BD70528_INT_DCIN1_DET_MASK),
	REGMAP_IRQ_REG(BD70528_INT_RTC_ALARM, 6, BD70528_INT_RTC_ALARM_MASK),
	REGMAP_IRQ_REG(BD70528_INT_ELPS_TIM, 6, BD70528_INT_ELPS_TIM_MASK),
	REGMAP_IRQ_REG(BD70528_INT_GPIO0, 7, BD70528_INT_GPIO0_MASK),
	REGMAP_IRQ_REG(BD70528_INT_GPIO1, 7, BD70528_INT_GPIO1_MASK),
	REGMAP_IRQ_REG(BD70528_INT_GPIO2, 7, BD70528_INT_GPIO2_MASK),
	REGMAP_IRQ_REG(BD70528_INT_GPIO3, 7, BD70528_INT_GPIO3_MASK),
	REGMAP_IRQ_REG(BD70528_INT_BUCK1_DVS_OPFAIL, 8,
		       BD70528_INT_BUCK1_DVS_OPFAIL_MASK),
	REGMAP_IRQ_REG(BD70528_INT_BUCK2_DVS_OPFAIL, 8,
		       BD70528_INT_BUCK2_DVS_OPFAIL_MASK),
	REGMAP_IRQ_REG(BD70528_INT_BUCK3_DVS_OPFAIL, 8,
		       BD70528_INT_BUCK3_DVS_OPFAIL_MASK),
	REGMAP_IRQ_REG(BD70528_INT_LED1_VOLT_OPFAIL, 8,
		       BD70528_INT_LED1_VOLT_OPFAIL_MASK),
	REGMAP_IRQ_REG(BD70528_INT_LED2_VOLT_OPFAIL, 8,
		       BD70528_INT_LED2_VOLT_OPFAIL_MASK),
};

static struct regmap_irq_chip bd70528_irq_chip = {
	.name = "bd70528_irq",
	.main_status = BD70528_REG_INT_MAIN,
	.irqs = &irqs[0],
	.num_irqs = ARRAY_SIZE(irqs),
	.status_base = BD70528_REG_INT_SHDN,
	.mask_base = BD70528_REG_INT_SHDN_MASK,
	.ack_base = BD70528_REG_INT_SHDN,
	.type_base = BD70528_REG_GPIO1_IN,
	.init_ack_masked = true,
	.num_regs = 9,
	.num_main_regs = 1,
	.num_type_reg = 4,
	.sub_reg_offsets = &bd70528_sub_irq_offsets[0],
	.num_main_status_bits = 8,
	.irq_reg_stride = 1,
};

#define WD_CTRL_MAGIC1 0x55
#define WD_CTRL_MAGIC2 0xAA

static int bd70528_wdt_set(struct bd70528 *bd70528, int enable, int *old_state)
{
	int ret, i;
	unsigned int tmp;
	u8 wd_ctrl_arr[3] = { WD_CTRL_MAGIC1, WD_CTRL_MAGIC2, 0 };
	u8 *wd_ctrl = &wd_ctrl_arr[2];

	ret = regmap_read(bd70528->chip.regmap, BD70528_REG_WDT_CTRL, &tmp);
	if (ret)
		return ret;

	*wd_ctrl = (u8)tmp;

	if (old_state) {
		if (*wd_ctrl & BD70528_MASK_WDT_EN)
			*old_state |= BD70528_WDT_STATE_BIT;
		else
			*old_state &= ~BD70528_WDT_STATE_BIT;
		if ((!enable) == (!(*old_state & BD70528_WDT_STATE_BIT)))
			return 0;
	}

	if (enable) {
		if (*wd_ctrl & BD70528_MASK_WDT_EN)
			return 0;
		*wd_ctrl |= BD70528_MASK_WDT_EN;
	} else {
		if (*wd_ctrl & BD70528_MASK_WDT_EN)
			*wd_ctrl &= ~BD70528_MASK_WDT_EN;
		else
			return 0;
	}

	for (i = 0; i < 3; i++) {
		ret = regmap_write(bd70528->chip.regmap, BD70528_REG_WDT_CTRL,
				   wd_ctrl_arr[i]);
		if (ret)
			return ret;
	}

	ret = regmap_read(bd70528->chip.regmap, BD70528_REG_WDT_CTRL, &tmp);
	if ((tmp & BD70528_MASK_WDT_EN) != (*wd_ctrl & BD70528_MASK_WDT_EN)) {
		dev_err(bd70528->chip.dev,
			"Watchdog ctrl mismatch (hw) 0x%x (set) 0x%x\n",
			tmp, *wd_ctrl);
		ret = -EIO;
	}

	return ret;
}

static int bd70528_i2c_probe(struct i2c_client *i2c,
			    const struct i2c_device_id *id)
{
	struct bd70528 *bd70528;
	struct regmap_irq_chip_data *irq_data;
	int ret, i;
	struct mutex *rtc_mutex;

	if (!i2c->irq) {
		dev_err(&i2c->dev, "No IRQ configured\n");
		return -EINVAL;
	}
	bd70528 = devm_kzalloc(&i2c->dev, sizeof(*bd70528), GFP_KERNEL);

	if (!bd70528)
		return -ENOMEM;

	mutex_init(&bd70528->rtc_timer_lock);

	dev_set_drvdata(&i2c->dev, bd70528);
	bd70528->chip.chip_type = ROHM_CHIP_TYPE_BD70528;
	bd70528->wdt_set = bd70528_wdt_set;
	bd70528->chip.regmap = devm_regmap_init_i2c(i2c, &bd70528_regmap);
	if (IS_ERR(bd70528->chip.regmap)) {
		dev_err(&i2c->dev, "regmap initialization failed\n");
		return PTR_ERR(bd70528->chip.regmap);
	}

	/*
	 * Disallow type setting for all IRQs by default as
	 *  most of them do not support setting type.
	 */
	for (i = 0; i < ARRAY_SIZE(irqs); i++)
		irqs[i].type.types_supported = 0;

	irqs[BD70528_INT_GPIO0].type.type_reg_offset = 0;
	irqs[BD70528_INT_GPIO0].type.type_rising_val = 0x20;
	irqs[BD70528_INT_GPIO0].type.type_falling_val = 0x10;
	irqs[BD70528_INT_GPIO0].type.type_level_high_val = 0x40;
	irqs[BD70528_INT_GPIO0].type.type_level_low_val = 0x50;
	irqs[BD70528_INT_GPIO0].type.types_supported = (IRQ_TYPE_EDGE_BOTH |
				IRQ_TYPE_LEVEL_HIGH | IRQ_TYPE_LEVEL_LOW);
	irqs[BD70528_INT_GPIO1].type.type_reg_offset = 2;
	irqs[BD70528_INT_GPIO1].type.type_rising_val = 0x20;
	irqs[BD70528_INT_GPIO1].type.type_falling_val = 0x10;
	irqs[BD70528_INT_GPIO1].type.type_level_high_val = 0x40;
	irqs[BD70528_INT_GPIO1].type.type_level_low_val = 0x50;
	irqs[BD70528_INT_GPIO1].type.types_supported = (IRQ_TYPE_EDGE_BOTH |
				IRQ_TYPE_LEVEL_HIGH | IRQ_TYPE_LEVEL_LOW);
	irqs[BD70528_INT_GPIO2].type.type_reg_offset = 4;
	irqs[BD70528_INT_GPIO2].type.type_rising_val = 0x20;
	irqs[BD70528_INT_GPIO2].type.type_falling_val = 0x10;
	irqs[BD70528_INT_GPIO2].type.type_level_high_val = 0x40;
	irqs[BD70528_INT_GPIO2].type.type_level_low_val = 0x50;
	irqs[BD70528_INT_GPIO2].type.types_supported = (IRQ_TYPE_EDGE_BOTH |
				IRQ_TYPE_LEVEL_HIGH | IRQ_TYPE_LEVEL_LOW);
	irqs[BD70528_INT_GPIO3].type.type_reg_offset = 6;
	irqs[BD70528_INT_GPIO3].type.type_rising_val = 0x20;
	irqs[BD70528_INT_GPIO3].type.type_falling_val = 0x10;
	irqs[BD70528_INT_GPIO3].type.type_level_high_val = 0x40;
	irqs[BD70528_INT_GPIO3].type.type_level_low_val = 0x50;
	irqs[BD70528_INT_GPIO3].type.types_supported = (IRQ_TYPE_EDGE_BOTH |
				IRQ_TYPE_LEVEL_HIGH | IRQ_TYPE_LEVEL_LOW);

	ret = devm_regmap_add_irq_chip(&i2c->dev, bd70528->chip.regmap,
				       i2c->irq, IRQF_ONESHOT, 0,
				       &bd70528_irq_chip, &irq_data);
	if (ret) {
		dev_err(&i2c->dev, "Failed to add irq_chip\n");
		return ret;
	}
	dev_dbg(&i2c->dev, "Registered %d irqs for chip\n",
			bd70528_irq_chip.num_irqs);

	/*
	 * BD70528 irq controller is not touching the main mask register.
	 * So enable the GPIO block interrupts at main level. We can just
	 * leave them enabled as irq-controller should disable irqs
	 * from sub-registers when IRQ is disabled or freed.
	 */
	ret = regmap_update_bits(bd70528->chip.regmap,
				 BD70528_REG_INT_MAIN_MASK,
				 BD70528_INT_GPIO_MASK, 0);

	ret = devm_mfd_add_devices(&i2c->dev, PLATFORM_DEVID_AUTO,
				   bd70528_mfd_cells,
				   ARRAY_SIZE(bd70528_mfd_cells), NULL, 0,
				   regmap_irq_get_domain(irq_data));
	if (ret)
		dev_err(&i2c->dev, "Failed to create subdevices\n");

	return ret;
}

static const struct of_device_id bd70528_of_match[] = {
	{
		.compatible = "rohm,bd70528",
	},
	{ },
};
MODULE_DEVICE_TABLE(of, bd70528_of_match);

static struct i2c_driver bd70528_drv = {
	.driver = {
		.name = "rohm-bd70528",
		.of_match_table = bd70528_of_match,
	},
	.probe = &bd70528_i2c_probe,
};

static int __init bd70528_init(void)
{
	return i2c_add_driver(&bd70528_drv);
}
subsys_initcall(bd70528_init);

static void __exit bd70528_exit(void)
{
	i2c_del_driver(&bd70528_drv);
}
module_exit(bd70528_exit);

MODULE_AUTHOR("Matti Vaittinen <matti.vaittinen@fi.rohmeurope.com>");
MODULE_DESCRIPTION("ROHM BD70528 Power Management IC driver");
MODULE_LICENSE("GPL");
