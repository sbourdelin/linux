/*
 * Battery charger driver for TI's tps65217
 *
 * Copyright (c) 2015, Collabora Ltd.

 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.

 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.

 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * Battery charger driver for TI's tps65217
 */
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/slab.h>
#include <linux/err.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/power_supply.h>

#include <linux/mfd/core.h>
#include <linux/mfd/tps65217.h>

#define CHARGER_STATUS_PRESENT	(TPS65217_STATUS_ACPWR | TPS65217_STATUS_USBPWR)
#define NUM_CHARGER_IRQS	2
#define POLL_INTERVAL		(HZ * 2)

struct tps65217_charger_platform_data {
	u32	charge_current_uamp;
	u32	charge_voltage_uvolt;
	int	ntc_type;
};

struct tps65217_charger {
	struct tps65217 *tps;
	struct device *dev;
	struct power_supply *psy;

	int	online;
	int	prev_online;

	struct task_struct	*poll_task;
	struct tps65217_charger_platform_data *pdata;
};

static enum power_supply_property tps65217_charger_props[] = {
	POWER_SUPPLY_PROP_ONLINE,
};

static int tps65217_set_charge_current(struct tps65217_charger *charger,
				       unsigned int uamp)
{
	int ret, val;

	dev_dbg(charger->dev, "setting charge current to %d uA\n", uamp);

	if (uamp == 300000)
		val = 0x00;
	else if (uamp == 400000)
		val = 0x01;
	else if (uamp == 500000)
		val = 0x02;
	else if (uamp == 700000)
		val = 0x03;
	else
		return -EINVAL;

	ret = tps65217_set_bits(charger->tps, TPS65217_REG_CHGCONFIG3,
				TPS65217_CHGCONFIG3_ICHRG_MASK,
				val << TPS65217_CHGCONFIG3_ICHRG_SHIFT,
				TPS65217_PROTECT_NONE);
	if (ret) {
		dev_err(charger->dev,
			"failed to set ICHRG setting to 0x%02x (err: %d)\n",
			val, ret);
		return ret;
	}

	return 0;
}

static int tps65217_set_charge_voltage(struct tps65217_charger *charger,
				       unsigned int uvolt)
{
	int ret, val;

	dev_dbg(charger->dev, "setting charge voltage to %d uV\n", uvolt);

	if (uvolt != 4100000 && uvolt != 4150000 &&
	    uvolt != 4200000 && uvolt != 4250000)
		return -EINVAL;

	val = (uvolt - 4100000) / 50000;

	ret = tps65217_set_bits(charger->tps, TPS65217_REG_CHGCONFIG2,
				TPS65217_CHGCONFIG2_VOREG_MASK,
				val << TPS65217_CHGCONFIG2_VOREG_SHIFT,
				TPS65217_PROTECT_NONE);
	if (ret) {
		dev_err(charger->dev,
			"failed to set VOCHG setting to 0x%02x (err: %d)\n",
			val, ret);
		return ret;
	}

	return 0;
}

static int tps65217_set_ntc_type(struct tps65217_charger *charger,
				 unsigned int ntc)
{
	int ret;

	dev_dbg(charger->dev, "setting NTC type to %d\n", ntc);

	if (ntc != 0 && ntc != 1)
		return -EINVAL;

	/*
	 * tps65217 rev. G, p. 31 (see p. 32 for NTC schematic)
	 *
	 * The device can be configured to support a 100k NTC (B = 3960) by
	 * setting the the NTC_TYPE bit in register CHGCONFIG1 to 1. However it
	 * is not recommended to do so. In sleep mode, the charger continues
	 * charging the battery, but all register values are reset to default
	 * values. Therefore, the charger would get the wrong temperature
	 * information. If 100k NTC setting is required, please contact the
	 * factory.
	 *
	 * ATTENTION, conflicting information, from p. 46
	 *
	 * NTC TYPE (for battery temperature measurement)
	 *   0 – 100k (curve 1, B = 3960)
	 *   1 – 10k  (curve 2, B = 3480) (default on reset)
	 */
	if (ntc) {
		ret = tps65217_set_bits(charger->tps, TPS65217_REG_CHGCONFIG1,
					TPS65217_CHGCONFIG1_NTC_TYPE,
					TPS65217_CHGCONFIG1_NTC_TYPE,
					TPS65217_PROTECT_NONE);
		if (ret) {
			dev_err(charger->dev,
				"failed to set NTC type to 10K: %d\n", ret);
			return ret;
		}
	} else {
		ret = tps65217_clear_bits(charger->tps, TPS65217_REG_CHGCONFIG1,
					  TPS65217_CHGCONFIG1_NTC_TYPE,
					  TPS65217_PROTECT_NONE);
		if (ret) {
			dev_err(charger->dev,
				"failed to set NTC type to 100K: %d\n", ret);
			return ret;
		}
	}

	return 0;
}

static int tps65217_config_charger(struct tps65217_charger *charger)
{
	int ret;
	struct tps65217_charger_platform_data *pdata = charger->pdata;

	if (!charger->pdata)
		return -EINVAL;

	ret = tps65217_set_charge_voltage(charger, pdata->charge_voltage_uvolt);
	if (ret) {
		dev_err(charger->dev,
			"failed to set charge voltage setting: %d\n", ret);
		return ret;
	}

	ret = tps65217_set_charge_current(charger, pdata->charge_current_uamp);
	if (ret) {
		dev_err(charger->dev,
			"failed to set charge current setting: %d\n", ret);
		return ret;
	}

	ret = tps65217_set_ntc_type(charger, pdata->ntc_type);
	if (ret) {
		dev_err(charger->dev,
			"failed to set NTC type setting: %d\n", ret);
		return ret;
	}

	return 0;
}

static int tps65217_enable_charging(struct tps65217_charger *charger)
{
	int ret;

	/* charger already enabled */
	if (charger->online)
		return 0;

	dev_dbg(charger->dev, "%s: enable charging\n", __func__);
	ret = tps65217_set_bits(charger->tps, TPS65217_REG_CHGCONFIG1,
				TPS65217_CHGCONFIG1_CHG_EN,
				TPS65217_CHGCONFIG1_CHG_EN,
				TPS65217_PROTECT_NONE);
	if (ret) {
		dev_err(charger->dev,
			"%s: Error in writing CHG_EN in reg 0x%x: %d\n",
			__func__, TPS65217_REG_CHGCONFIG1, ret);
		return ret;
	}

	charger->online = 1;

	return 0;
}

static int tps65217_charger_get_property(struct power_supply *psy,
					 enum power_supply_property psp,
					 union power_supply_propval *val)
{
	struct tps65217_charger *charger = power_supply_get_drvdata(psy);

	if (psp == POWER_SUPPLY_PROP_ONLINE) {
		val->intval = charger->online;
		return 0;
	}
	return -EINVAL;
}

static irqreturn_t tps65217_charger_irq(int irq, void *dev)
{
	int ret, val;
	struct tps65217_charger *charger = dev;

	charger->prev_online = charger->online;

	ret = tps65217_reg_read(charger->tps, TPS65217_REG_STATUS, &val);
	if (ret < 0) {
		dev_err(charger->dev, "%s: Error in reading reg 0x%x\n",
			__func__, TPS65217_REG_STATUS);
		return IRQ_HANDLED;
	}

	dev_dbg(charger->dev, "%s: 0x%x\n", __func__, val);

	/* check for charger status bit */
	if (val & CHARGER_STATUS_PRESENT) {
		ret = tps65217_enable_charging(charger);
		if (ret) {
			dev_err(charger->dev,
				"failed to enable charger: %d\n", ret);
			return IRQ_HANDLED;
		}
	} else {
		charger->online = 0;
	}

	if (charger->prev_online != charger->online)
		power_supply_changed(charger->psy);

	ret = tps65217_reg_read(charger->tps, TPS65217_REG_CHGCONFIG0, &val);
	if (ret < 0) {
		dev_err(charger->dev, "%s: Error in reading reg 0x%x\n",
			__func__, TPS65217_REG_CHGCONFIG0);
		return IRQ_HANDLED;
	}

	if (val & TPS65217_CHGCONFIG0_ACTIVE)
		dev_dbg(charger->dev, "%s: charger is charging\n", __func__);
	else
		dev_dbg(charger->dev,
			"%s: charger is NOT charging\n", __func__);

	return IRQ_HANDLED;
}

static int tps65217_charger_poll_task(void *data)
{
	set_freezable();

	while (!kthread_should_stop()) {
		schedule_timeout_interruptible(POLL_INTERVAL);
		try_to_freeze();
		tps65217_charger_irq(-1, data);
	}
	return 0;
}

#ifdef CONFIG_OF
static struct tps65217_charger_platform_data *tps65217_charger_pdata_init(
		struct platform_device *pdev)
{
	struct tps65217_charger_platform_data *pdata;
	struct device_node *np = pdev->dev.of_node;
	int ret;

	if (!np) {
		dev_err(&pdev->dev, "No charger OF node\n");
		return ERR_PTR(-EINVAL);
	}

	pdata = devm_kzalloc(&pdev->dev, sizeof(*pdata), GFP_KERNEL);
	if (!pdata)
		return ERR_PTR(-ENOMEM);

	ret = of_property_read_u32(np, "charge-voltage-microvolt",
				   &pdata->charge_voltage_uvolt);
	if (ret)
		pdata->charge_voltage_uvolt = 4100000;

	ret = of_property_read_u32(np, "charge-current-microamp",
				   &pdata->charge_current_uamp);
	if (ret)
		pdata->charge_current_uamp = 500000;

	ret = of_property_read_u32(np, "ti,ntc-type",
				   &pdata->ntc_type);
	if (ret)
		pdata->ntc_type = 1;	/* 10k  (curve 2, B = 3480) */

	return pdata;
}
#else /* CONFIG_OF */
static struct tps65217_charger_platform_data *tps65217_charger_pdata_init(
		struct platform_device *pdev)
{
	return NULL;
}
#endif /* CONFIG_OF */

static const struct power_supply_desc tps65217_charger_desc = {
	.name			= "tps65217-charger",
	.type			= POWER_SUPPLY_TYPE_MAINS,
	.get_property		= tps65217_charger_get_property,
	.properties		= tps65217_charger_props,
	.num_properties		= ARRAY_SIZE(tps65217_charger_props),
};

static int tps65217_charger_probe(struct platform_device *pdev)
{
	struct tps65217 *tps = dev_get_drvdata(pdev->dev.parent);
	struct tps65217_charger *charger;
	struct power_supply_config cfg = {};
	struct task_struct *poll_task;
	int irq[NUM_CHARGER_IRQS];
	int ret;
	int i;

	charger = devm_kzalloc(&pdev->dev, sizeof(*charger), GFP_KERNEL);
	if (!charger)
		return -ENOMEM;

	platform_set_drvdata(pdev, charger);
	charger->tps = tps;
	charger->dev = &pdev->dev;

	cfg.of_node = pdev->dev.of_node;
	cfg.drv_data = charger;

	charger->pdata = tps65217_charger_pdata_init(pdev);
	if (IS_ERR(charger->pdata)) {
		dev_err(charger->dev, "failed: getting platform data\n");
		return PTR_ERR(charger->pdata);
	}

	ret = tps65217_config_charger(charger);
	if (ret < 0) {
		dev_err(charger->dev, "charger config failed, err %d\n", ret);
		return ret;
	}

	charger->psy = devm_power_supply_register(&pdev->dev,
						  &tps65217_charger_desc,
						  &cfg);
	if (IS_ERR(charger->psy)) {
		dev_err(&pdev->dev, "failed: power supply register\n");
		return PTR_ERR(charger->psy);
	}

	irq[0] = platform_get_irq_byname(pdev, "USB");
	irq[1] = platform_get_irq_byname(pdev, "AC");

	/* Create a polling thread if an interrupt is invalid */
	if (irq[0] < 0 || irq[1] < 0) {
		poll_task = kthread_run(tps65217_charger_poll_task,
					charger, "ktps65217charger");
		if (IS_ERR(poll_task)) {
			ret = PTR_ERR(poll_task);
			dev_err(charger->dev,
				"Unable to run kthread err %d\n", ret);
			return ret;
		}

		charger->poll_task = poll_task;
		return 0;
	}

	/* Create IRQ threads for charger interrupts */
	for (i = 0; i < NUM_CHARGER_IRQS; i++) {
		ret = devm_request_threaded_irq(&pdev->dev, irq[i], NULL,
						tps65217_charger_irq,
						0, "tps65217-charger",
						charger);
		if (ret) {
			dev_err(charger->dev,
				"Unable to register irq %d err %d\n", irq[i],
				ret);
			return ret;
		}

		/* Check current state */
		tps65217_charger_irq(-1, charger);
	}

	return 0;
}

static int tps65217_charger_remove(struct platform_device *pdev)
{
	struct tps65217_charger *charger = platform_get_drvdata(pdev);

	if (charger->poll_task)
		kthread_stop(charger->poll_task);

	return 0;
}

static const struct of_device_id tps65217_charger_match_table[] = {
	{ .compatible = "ti,tps65217-charger", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, tps65217_charger_match_table);

static struct platform_driver tps65217_charger_driver = {
	.probe	= tps65217_charger_probe,
	.remove = tps65217_charger_remove,
	.driver	= {
		.name	= "tps65217-charger",
		.of_match_table = of_match_ptr(tps65217_charger_match_table),
	},

};
module_platform_driver(tps65217_charger_driver);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Enric Balletbo Serra <enric.balletbo@collabora.com>");
MODULE_DESCRIPTION("TPS65217 battery charger driver");
