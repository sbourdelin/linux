/*
 * hisi_powerkey.c - Hisilicon MIC powerkey driver
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

/* the above held interrupt will trigger after 4 seconds */
#define MAX_HELD_TIME	(4 * MSEC_PER_SEC)


typedef irqreturn_t (*hi65xx_irq_handler) (int irq, void *data);
enum { id_pressed, id_released, id_held, id_last };
static irqreturn_t hi65xx_pkey_irq_handler(int irq, void *q);

/*
 * power key irq information
 */
static struct hi65xx_pkey_irq_info {
	hi65xx_irq_handler handler;
	const char *const name;
	int irq;
} irq_info[id_last] = {
	[id_pressed] = {
		.handler = hi65xx_pkey_irq_handler,
		.name = "down",
		.irq = -1,
	},
	[id_released] = {
		.handler = hi65xx_pkey_irq_handler,
		.name = "up",
		.irq = -1,
	},
	[id_held] = {
		.handler = hi65xx_pkey_irq_handler,
		.name = "hold 4s",
		.irq = -1,
	},
};

/*
 * power key events
 */
static struct key_report_pairs {
	int code;
	int value;
} pkey_report[id_last] = {
	[id_released] = {
		.code = KEY_POWER,
		.value = 0
	},
	[id_pressed] = {
		.code = KEY_POWER,
		.value = 1
	},
	[id_held] = {
		.code = KEY_RESTART,
		.value = 0
	},
};

struct hi65xx_priv {
	struct input_dev *input;
	struct wakeup_source wlock;
};

static inline void report_key(struct input_dev *dev, int id_action)
{
	/*
	 * track the state of the key held event since only ON/OFF values are
	 * allowed on EV_KEY types: KEY_RESTART will always toggle its value to
	 * guarantee that the event is passed to handlers (dispossition update).
	 */
	if (id_action == id_held)
		pkey_report[id_held].value ^= 1;

	dev_dbg(dev->dev.parent, "received - code %d, value %d\n",
		pkey_report[id_action].code,
		pkey_report[id_action].value);

	input_report_key(dev, pkey_report[id_action].code,
		pkey_report[id_action].value);
}

static irqreturn_t hi65xx_pkey_irq_handler(int irq, void *q)
{
	struct hi65xx_priv *p = q;
	int i, action = -1;

	for (i = 0; i < id_last; i++)
		if (irq == irq_info[i].irq)
			action = i;

	if (action == -1)
		return IRQ_NONE;

	__pm_wakeup_event(&p->wlock, MAX_HELD_TIME);

	report_key(p->input, action);
	input_sync(p->input);

	return IRQ_HANDLED;
}

static int hi65xx_powerkey_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct hi65xx_priv *priv;
	int irq, i, ret;

	if (pdev == NULL) {
		dev_err(dev, "parameter error\n");
		return -EINVAL;
	}

	priv = devm_kzalloc(dev, sizeof(struct hi65xx_priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->input = input_allocate_device();
	if (!priv->input) {
		dev_err(&pdev->dev, "failed to allocate input device\n");
		return -ENOENT;
	}

	priv->input->evbit[0] = BIT_MASK(EV_KEY);
	priv->input->dev.parent = &pdev->dev;
	priv->input->phys = "hisi_on/input0";
	priv->input->name = "HISI 65xx PowerOn Key";

	for (i = 0; i < ARRAY_SIZE(pkey_report); i++)
		input_set_capability(priv->input, EV_KEY, pkey_report[i].code);

	for (i = 0; i < ARRAY_SIZE(irq_info); i++) {

		irq = platform_get_irq_byname(pdev, irq_info[i].name);
		if (irq < 0) {
			dev_err(dev, "couldn't get irq %s\n", irq_info[i].name);
			ret = irq;
			goto err_irq;
		}

		ret = devm_request_threaded_irq(dev, irq, NULL,
					irq_info[i].handler, IRQF_ONESHOT,
					irq_info[i].name, priv);
		if (ret < 0) {
			dev_err(dev, "couldn't get irq %s\n", irq_info[i].name);
			goto err_irq;
		}

		irq_info[i].irq = irq;
	}

	wakeup_source_init(&priv->wlock, "hisi-powerkey");

	ret = input_register_device(priv->input);
	if (ret) {
		dev_err(&pdev->dev, "failed to register input device: %d\n",
			ret);
		ret = -ENOENT;
		goto err_register;
	}

	platform_set_drvdata(pdev, priv);

	return 0;

err_register:
	wakeup_source_trash(&priv->wlock);
err_irq:
	input_free_device(priv->input);

	return ret;
}

static int hi65xx_powerkey_remove(struct platform_device *pdev)
{
	struct hi65xx_priv *priv = platform_get_drvdata(pdev);

	wakeup_source_trash(&priv->wlock);
	input_unregister_device(priv->input);

	return 0;
}

static const struct of_device_id hi65xx_powerkey_of_match[] = {
	{ .compatible = "hisilicon,hi6552-powerkey", },
	{},
};

MODULE_DEVICE_TABLE(of, hi65xx_powerkey_of_match);

static struct platform_driver hi65xx_powerkey_driver = {
	.driver = {
		.owner = THIS_MODULE,
		.name = "hi65xx-powerkey",
		.of_match_table = hi65xx_powerkey_of_match,
	},
	.probe = hi65xx_powerkey_probe,
	.remove  = hi65xx_powerkey_remove,
};

module_platform_driver(hi65xx_powerkey_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Zhiliang Xue <xuezhiliang@huawei.com");
MODULE_DESCRIPTION("Hisi PMIC Power key driver");
MODULE_LICENSE("GPL v2");


