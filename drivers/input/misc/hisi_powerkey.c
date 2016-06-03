/*
 * Hisilicon PMIC powerkey driver
 *
 * Copyright (C) 2013 Hisilicon Ltd.
 * Copyright (C) 2015, 2016 Linaro Ltd.
 *
 * This file is subject to the terms and conditions of the GNU General
 * Public License. See the file "COPYING" in the main directory of this
 * archive for more details.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/platform_device.h>
#include <linux/interrupt.h>
#include <linux/reboot.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of_irq.h>
#include <linux/input.h>
#include <linux/slab.h>

/* the held interrupt will trigger after 4 seconds */
#define MAX_HELD_TIME	(4 * MSEC_PER_SEC)


enum id_action { ID_PRESSED, ID_RELEASED, ID_HELD, ID_LAST };
const char *const irq_names[ID_LAST] = {"down", "up", "hold 4s"};

struct hi65xx_priv {
	struct input_dev *input;
};

static irqreturn_t hi65xx_power_press_isr(int irq, void *q)
{
	struct hi65xx_priv *p = q;

	pm_wakeup_event(p->input->dev.parent, MAX_HELD_TIME);
	input_report_key(p->input, KEY_POWER, 1);
	input_sync(p->input);

	return IRQ_HANDLED;
}

static irqreturn_t hi65xx_power_release_isr(int irq, void *q)
{
	struct hi65xx_priv *p = q;

	pm_wakeup_event(p->input->dev.parent, MAX_HELD_TIME);
	input_report_key(p->input, KEY_POWER, 0);
	input_sync(p->input);

	return IRQ_HANDLED;
}

static irqreturn_t hi65xx_restart_toggle_isr(int irq, void *q)
{
	struct hi65xx_priv *p = q;
	int value = test_bit(KEY_RESTART, p->input->key);

	pm_wakeup_event(p->input->dev.parent, MAX_HELD_TIME);
	input_report_key(p->input, KEY_RESTART, !value);
	input_sync(p->input);

	return IRQ_HANDLED;
}

irqreturn_t (*irq_handlers[ID_LAST])(int irq, void *q) = {
	hi65xx_power_press_isr,
	hi65xx_power_release_isr,
	hi65xx_restart_toggle_isr,
};

static int hi65xx_powerkey_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct hi65xx_priv *priv;
	int irq, i, ret;

	priv = devm_kzalloc(dev, sizeof(struct hi65xx_priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->input = devm_input_allocate_device(&pdev->dev);
	if (!priv->input) {
		dev_err(&pdev->dev, "failed to allocate input device\n");
		return -ENOMEM;
	}

	priv->input->phys = "hisi_on/input0";
	priv->input->name = "HISI 65xx PowerOn Key";

	input_set_capability(priv->input, EV_KEY, KEY_POWER);
	input_set_capability(priv->input, EV_KEY, KEY_RESTART);

	for (i = 0; i < ID_LAST; i++) {

		irq = platform_get_irq_byname(pdev, irq_names[i]);
		if (irq < 0) {
			dev_err(dev, "couldn't get irq %s\n", irq_names[i]);
			return irq;
		}

		ret = devm_request_any_context_irq(dev, irq,
					irq_handlers[i], IRQF_ONESHOT,
					irq_names[i], priv);
		if (ret < 0) {
			dev_err(dev, "couldn't get irq %s\n", irq_names[i]);
			return ret;
		}
	}

	ret = input_register_device(priv->input);
	if (ret) {
		dev_err(&pdev->dev, "failed to register input device: %d\n",
			ret);
		return ret;
	}

	platform_set_drvdata(pdev, priv);
	device_init_wakeup(&pdev->dev, 1);

	return 0;
}

static int hi65xx_powerkey_remove(struct platform_device *pdev)
{
	device_init_wakeup(&pdev->dev, 0);
	return 0;
}

static const struct of_device_id hi65xx_powerkey_of_match[] = {
	{ .compatible = "hisilicon,hi6552-powerkey", },
	{},
};

MODULE_DEVICE_TABLE(of, hi65xx_powerkey_of_match);

static struct platform_driver hi65xx_powerkey_driver = {
	.driver = {
		.name = "hi65xx-powerkey",
		.of_match_table = of_match_ptr(hi65xx_powerkey_of_match),
	},
	.probe = hi65xx_powerkey_probe,
	.remove  = hi65xx_powerkey_remove,
};

module_platform_driver(hi65xx_powerkey_driver);

MODULE_AUTHOR("Zhiliang Xue <xuezhiliang@huawei.com");
MODULE_DESCRIPTION("Hisi PMIC Power key driver");
MODULE_LICENSE("GPL v2");
