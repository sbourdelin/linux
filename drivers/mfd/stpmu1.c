// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) STMicroelectronics 2018 - All Rights Reserved
 * Author: Philippe Peurichard <philippe.peurichard@st.com>,
 * Pascal Paillet <p.paillet@st.com> for STMicroelectronics.
 */

#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/mfd/core.h>
#include <linux/mfd/stpmu1.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/pm_wakeirq.h>
#include <linux/regmap.h>
#include <dt-bindings/mfd/st,stpmu1.h>

static bool stpmu1_reg_readable(struct device *dev, unsigned int reg);
static bool stpmu1_reg_writeable(struct device *dev, unsigned int reg);
static bool stpmu1_reg_volatile(struct device *dev, unsigned int reg);

const struct regmap_config stpmu1_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.cache_type = REGCACHE_RBTREE,
	.max_register = PMIC_MAX_REGISTER_ADDRESS,
	.readable_reg = stpmu1_reg_readable,
	.writeable_reg = stpmu1_reg_writeable,
	.volatile_reg = stpmu1_reg_volatile,
};

#define FILL_IRQS(_index) \
	[(_index)] = { \
		.reg_offset = ((_index) >> 3), \
		.mask = (1 << (_index % 8)), \
	}

static const struct regmap_irq stpmu1_irqs[] = {
	FILL_IRQS(IT_PONKEY_F),
	FILL_IRQS(IT_PONKEY_R),
	FILL_IRQS(IT_WAKEUP_F),
	FILL_IRQS(IT_WAKEUP_R),
	FILL_IRQS(IT_VBUS_OTG_F),
	FILL_IRQS(IT_VBUS_OTG_R),
	FILL_IRQS(IT_SWOUT_F),
	FILL_IRQS(IT_SWOUT_R),

	FILL_IRQS(IT_CURLIM_BUCK1),
	FILL_IRQS(IT_CURLIM_BUCK2),
	FILL_IRQS(IT_CURLIM_BUCK3),
	FILL_IRQS(IT_CURLIM_BUCK4),
	FILL_IRQS(IT_OCP_OTG),
	FILL_IRQS(IT_OCP_SWOUT),
	FILL_IRQS(IT_OCP_BOOST),
	FILL_IRQS(IT_OVP_BOOST),

	FILL_IRQS(IT_CURLIM_LDO1),
	FILL_IRQS(IT_CURLIM_LDO2),
	FILL_IRQS(IT_CURLIM_LDO3),
	FILL_IRQS(IT_CURLIM_LDO4),
	FILL_IRQS(IT_CURLIM_LDO5),
	FILL_IRQS(IT_CURLIM_LDO6),
	FILL_IRQS(IT_SHORT_SWOTG),
	FILL_IRQS(IT_SHORT_SWOUT),

	FILL_IRQS(IT_TWARN_F),
	FILL_IRQS(IT_TWARN_R),
	FILL_IRQS(IT_VINLOW_F),
	FILL_IRQS(IT_VINLOW_R),
	FILL_IRQS(IT_SWIN_F),
	FILL_IRQS(IT_SWIN_R),
};

static const struct regmap_irq_chip stpmu1_regmap_irq_chip = {
	.name = "pmic_irq",
	.status_base = INT_PENDING_R1,
	.mask_base = INT_CLEAR_MASK_R1,
	.unmask_base = INT_SET_MASK_R1,
	.ack_base = INT_CLEAR_R1,
	.num_regs = STPMU1_PMIC_NUM_IRQ_REGS,
	.irqs = stpmu1_irqs,
	.num_irqs = ARRAY_SIZE(stpmu1_irqs),
};

static bool stpmu1_reg_readable(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case TURN_ON_SR:
	case TURN_OFF_SR:
	case ICC_LDO_TURN_OFF_SR:
	case ICC_BUCK_TURN_OFF_SR:
	case RREQ_STATE_SR:
	case VERSION_SR:
	case SWOFF_PWRCTRL_CR:
	case PADS_PULL_CR:
	case BUCKS_PD_CR:
	case LDO14_PD_CR:
	case LDO56_VREF_PD_CR:
	case VBUS_DET_VIN_CR:
	case PKEY_TURNOFF_CR:
	case BUCKS_MASK_RANK_CR:
	case BUCKS_MASK_RESET_CR:
	case LDOS_MASK_RANK_CR:
	case LDOS_MASK_RESET_CR:
	case WCHDG_CR:
	case WCHDG_TIMER_CR:
	case BUCKS_ICCTO_CR:
	case LDOS_ICCTO_CR:
	case BUCK1_ACTIVE_CR:
	case BUCK2_ACTIVE_CR:
	case BUCK3_ACTIVE_CR:
	case BUCK4_ACTIVE_CR:
	case VREF_DDR_ACTIVE_CR:
	case LDO1_ACTIVE_CR:
	case LDO2_ACTIVE_CR:
	case LDO3_ACTIVE_CR:
	case LDO4_ACTIVE_CR:
	case LDO5_ACTIVE_CR:
	case LDO6_ACTIVE_CR:
	case BUCK1_STDBY_CR:
	case BUCK2_STDBY_CR:
	case BUCK3_STDBY_CR:
	case BUCK4_STDBY_CR:
	case VREF_DDR_STDBY_CR:
	case LDO1_STDBY_CR:
	case LDO2_STDBY_CR:
	case LDO3_STDBY_CR:
	case LDO4_STDBY_CR:
	case LDO5_STDBY_CR:
	case LDO6_STDBY_CR:
	case BST_SW_CR:
	case INT_PENDING_R1:
	case INT_PENDING_R2:
	case INT_PENDING_R3:
	case INT_PENDING_R4:
	case INT_DBG_LATCH_R1:
	case INT_DBG_LATCH_R2:
	case INT_DBG_LATCH_R3:
	case INT_DBG_LATCH_R4:
	case INT_CLEAR_R1:
	case INT_CLEAR_R2:
	case INT_CLEAR_R3:
	case INT_CLEAR_R4:
	case INT_MASK_R1:
	case INT_MASK_R2:
	case INT_MASK_R3:
	case INT_MASK_R4:
	case INT_SET_MASK_R1:
	case INT_SET_MASK_R2:
	case INT_SET_MASK_R3:
	case INT_SET_MASK_R4:
	case INT_CLEAR_MASK_R1:
	case INT_CLEAR_MASK_R2:
	case INT_CLEAR_MASK_R3:
	case INT_CLEAR_MASK_R4:
	case INT_SRC_R1:
	case INT_SRC_R2:
	case INT_SRC_R3:
	case INT_SRC_R4:
		return true;
	default:
		return false;
	}
}

static bool stpmu1_reg_writeable(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case SWOFF_PWRCTRL_CR:
	case PADS_PULL_CR:
	case BUCKS_PD_CR:
	case LDO14_PD_CR:
	case LDO56_VREF_PD_CR:
	case VBUS_DET_VIN_CR:
	case PKEY_TURNOFF_CR:
	case BUCKS_MASK_RANK_CR:
	case BUCKS_MASK_RESET_CR:
	case LDOS_MASK_RANK_CR:
	case LDOS_MASK_RESET_CR:
	case WCHDG_CR:
	case WCHDG_TIMER_CR:
	case BUCKS_ICCTO_CR:
	case LDOS_ICCTO_CR:
	case BUCK1_ACTIVE_CR:
	case BUCK2_ACTIVE_CR:
	case BUCK3_ACTIVE_CR:
	case BUCK4_ACTIVE_CR:
	case VREF_DDR_ACTIVE_CR:
	case LDO1_ACTIVE_CR:
	case LDO2_ACTIVE_CR:
	case LDO3_ACTIVE_CR:
	case LDO4_ACTIVE_CR:
	case LDO5_ACTIVE_CR:
	case LDO6_ACTIVE_CR:
	case BUCK1_STDBY_CR:
	case BUCK2_STDBY_CR:
	case BUCK3_STDBY_CR:
	case BUCK4_STDBY_CR:
	case VREF_DDR_STDBY_CR:
	case LDO1_STDBY_CR:
	case LDO2_STDBY_CR:
	case LDO3_STDBY_CR:
	case LDO4_STDBY_CR:
	case LDO5_STDBY_CR:
	case LDO6_STDBY_CR:
	case BST_SW_CR:
	case INT_DBG_LATCH_R1:
	case INT_DBG_LATCH_R2:
	case INT_DBG_LATCH_R3:
	case INT_DBG_LATCH_R4:
	case INT_CLEAR_R1:
	case INT_CLEAR_R2:
	case INT_CLEAR_R3:
	case INT_CLEAR_R4:
	case INT_SET_MASK_R1:
	case INT_SET_MASK_R2:
	case INT_SET_MASK_R3:
	case INT_SET_MASK_R4:
	case INT_CLEAR_MASK_R1:
	case INT_CLEAR_MASK_R2:
	case INT_CLEAR_MASK_R3:
	case INT_CLEAR_MASK_R4:
		return true;
	default:
		return false;
	}
}

static bool stpmu1_reg_volatile(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case TURN_ON_SR:
	case TURN_OFF_SR:
	case ICC_LDO_TURN_OFF_SR:
	case ICC_BUCK_TURN_OFF_SR:
	case RREQ_STATE_SR:
	case INT_PENDING_R1:
	case INT_PENDING_R2:
	case INT_PENDING_R3:
	case INT_PENDING_R4:
	case INT_SRC_R1:
	case INT_SRC_R2:
	case INT_SRC_R3:
	case INT_SRC_R4:
	case WCHDG_CR:
		return true;
	default:
		return false;
	}
}

static int stpmu1_configure_from_dt(struct stpmu1_dev *pmic_dev)
{
	struct device_node *np = pmic_dev->np;
	u32 reg = 0;
	int ret = 0;
	int irq;

	irq = of_irq_get(np, 0);
	if (irq <= 0) {
		dev_err(pmic_dev->dev,
			"Failed to get irq config: %d\n", irq);
		return irq ? irq : -ENODEV;
	}
	pmic_dev->irq = irq;

	irq = of_irq_get(np, 1);
	if (irq <= 0) {
		dev_err(pmic_dev->dev,
			"Failed to get irq_wake config: %d\n", irq);
		return irq ? irq : -ENODEV;
	}
	pmic_dev->irq_wake = irq;

	device_init_wakeup(pmic_dev->dev, true);
	ret = dev_pm_set_dedicated_wake_irq(pmic_dev->dev, pmic_dev->irq_wake);
	if (ret)
		dev_warn(pmic_dev->dev, "failed to set up wakeup irq");

	if (!of_property_read_u32(np, "st,main_control_register", &reg)) {
		ret = regmap_update_bits(pmic_dev->regmap,
					 SWOFF_PWRCTRL_CR,
					 PWRCTRL_POLARITY_HIGH |
					 PWRCTRL_PIN_VALID |
					 RESTART_REQUEST_ENABLED,
					 reg);
		if (ret) {
			dev_err(pmic_dev->dev,
				"Failed to update main control register: %d\n",
				ret);
			return ret;
		}
	}

	if (!of_property_read_u32(np, "st,pads_pull_register", &reg)) {
		ret = regmap_update_bits(pmic_dev->regmap,
					 PADS_PULL_CR,
					 WAKEUP_DETECTOR_DISABLED |
					 PWRCTRL_PD_ACTIVE |
					 PWRCTRL_PU_ACTIVE |
					 WAKEUP_PD_ACTIVE,
					 reg);
		if (ret) {
			dev_err(pmic_dev->dev,
				"Failed to update pads control register: %d\n",
				ret);
			return ret;
		}
	}

	if (!of_property_read_u32(np, "st,vin_control_register", &reg)) {
		ret = regmap_update_bits(pmic_dev->regmap,
					 VBUS_DET_VIN_CR,
					 VINLOW_CTRL_REG_MASK,
					 reg);
		if (ret) {
			dev_err(pmic_dev->dev,
				"Failed to update vin control register: %d\n",
				ret);
			return ret;
		}
	}

	if (!of_property_read_u32(np, "st,usb_control_register", &reg)) {
		ret = regmap_update_bits(pmic_dev->regmap, BST_SW_CR,
					 BOOST_OVP_DISABLED |
					 VBUS_OTG_DETECTION_DISABLED |
					 SW_OUT_DISCHARGE |
					 VBUS_OTG_DISCHARGE |
					 OCP_LIMIT_HIGH,
					 reg);
		if (ret) {
			dev_err(pmic_dev->dev,
				"Failed to update usb control register: %d\n",
				ret);
			return ret;
		}
	}

	return 0;
}

int stpmu1_device_init(struct stpmu1_dev *pmic_dev)
{
	int ret;
	unsigned int val;

	pmic_dev->regmap =
	    devm_regmap_init_i2c(pmic_dev->i2c, &stpmu1_regmap_config);

	if (IS_ERR(pmic_dev->regmap)) {
		ret = PTR_ERR(pmic_dev->regmap);
		dev_err(pmic_dev->dev, "Failed to allocate register map: %d\n",
			ret);
		return ret;
	}

	ret = stpmu1_configure_from_dt(pmic_dev);
	if (ret < 0) {
		dev_err(pmic_dev->dev,
			"Unable to configure PMIC from Device Tree: %d\n", ret);
		return ret;
	}

	/* Read Version ID */
	ret = regmap_read(pmic_dev->regmap, VERSION_SR, &val);
	if (ret < 0) {
		dev_err(pmic_dev->dev, "Unable to read pmic version\n");
		return ret;
	}
	dev_dbg(pmic_dev->dev, "PMIC Chip Version: 0x%x\n", val);

	/* Initialize PMIC IRQ Chip & IRQ domains associated */
	ret = devm_regmap_add_irq_chip(pmic_dev->dev, pmic_dev->regmap,
				       pmic_dev->irq,
				       IRQF_ONESHOT | IRQF_SHARED,
				       0, &stpmu1_regmap_irq_chip,
				       &pmic_dev->irq_data);
	if (ret < 0) {
		dev_err(pmic_dev->dev, "IRQ Chip registration failed: %d\n",
			ret);
		return ret;
	}

	return 0;
}

static const struct of_device_id stpmu1_dt_match[] = {
	{.compatible = "st,stpmu1"},
	{},
};

MODULE_DEVICE_TABLE(of, stpmu1_dt_match);

static int stpmu1_remove(struct i2c_client *i2c)
{
	struct stpmu1_dev *pmic_dev = i2c_get_clientdata(i2c);

	of_platform_depopulate(pmic_dev->dev);

	return 0;
}

static int stpmu1_probe(struct i2c_client *i2c,
			const struct i2c_device_id *id)
{
	struct stpmu1_dev *pmic;
	struct device *dev = &i2c->dev;
	int ret = 0;

	pmic = devm_kzalloc(dev, sizeof(struct stpmu1_dev), GFP_KERNEL);
	if (!pmic)
		return -ENOMEM;

	pmic->np = dev->of_node;

	dev_set_drvdata(dev, pmic);
	pmic->dev = dev;
	pmic->i2c = i2c;

	ret = stpmu1_device_init(pmic);
	if (ret < 0)
		goto err;

	ret = of_platform_populate(pmic->np, NULL, NULL, pmic->dev);

	dev_dbg(dev, "stpmu1 driver probed\n");
err:
	return ret;
}

static const struct i2c_device_id stpmu1_id[] = {
	{"stpmu1", 0},
	{}
};

MODULE_DEVICE_TABLE(i2c, stpmu1_id);

#ifdef CONFIG_PM_SLEEP
static int stpmu1_suspend(struct device *dev)
{
	struct i2c_client *i2c = container_of(dev, struct i2c_client, dev);
	struct stpmu1_dev *pmic_dev = i2c_get_clientdata(i2c);

	if (device_may_wakeup(dev))
		enable_irq_wake(pmic_dev->irq_wake);

	disable_irq(pmic_dev->irq);
	return 0;
}

static int stpmu1_resume(struct device *dev)
{
	struct i2c_client *i2c = container_of(dev, struct i2c_client, dev);
	struct stpmu1_dev *pmic_dev = i2c_get_clientdata(i2c);

	regcache_sync(pmic_dev->regmap);

	if (device_may_wakeup(dev))
		disable_irq_wake(pmic_dev->irq_wake);

	enable_irq(pmic_dev->irq);
	return 0;
}
#endif

static SIMPLE_DEV_PM_OPS(stpmu1_pm, stpmu1_suspend, stpmu1_resume);

static struct i2c_driver stpmu1_driver = {
	.driver = {
		   .name = "stpmu1",
		   .owner = THIS_MODULE,
		   .pm = &stpmu1_pm,
		   .of_match_table = of_match_ptr(stpmu1_dt_match),
		   },
	.probe = stpmu1_probe,
	.remove = stpmu1_remove,
	.id_table = stpmu1_id,
};

module_i2c_driver(stpmu1_driver);

MODULE_DESCRIPTION("STPMU1 PMIC I2C Client");
MODULE_AUTHOR("<philippe.peurichard@st.com>");
MODULE_LICENSE("GPL");
