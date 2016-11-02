/*
 * GPIO driver for NVIDIA Tegra186
 *
 * Copyright (c) 2016, NVIDIA CORPORATION. All rights reserved.
 *
 * Author: Suresh Mangipudi <smangipudi@nvidia.com>
 * Author: Laxman Dewangan <ldewangan@nvidia.com>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 */

#include <linux/err.h>
#include <linux/init.h>
#include <linux/irq.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/gpio.h>
#include <linux/of_device.h>
#include <linux/of_gpio.h>
#include <linux/platform_device.h>
#include <linux/module.h>
#include <linux/irqdomain.h>
#include <linux/irqchip/chained_irq.h>
#include <dt-bindings/gpio/tegra186-gpio.h>

/* GPIO control registers */
#define GPIO_ENB_CONFIG_REG			0x00
#define GPIO_DBC_THRES_REG			0x04
#define GPIO_INPUT_REG				0x08
#define GPIO_OUT_CTRL_REG			0x0c
#define GPIO_OUT_VAL_REG			0x10
#define GPIO_INT_CLEAR_REG			0x14
#define GPIO_REG_DIFF				0x20
#define GPIO_INT_STATUS_OFFSET			0x100

/* GPIO SCR registers */
#define GPIO_SCR_REG				0x04
#define GPIO_SCR_DIFF				0x08

#define GPIO_INOUT_BIT				BIT(1)
#define GPIO_TRG_TYPE_BIT(x)			((x) & 0x3)
#define GPIO_TRG_TYPE_BIT_OFFSET		0x2
#define GPIO_TRG_LVL_BIT			BIT(4)
#define GPIO_DEB_FUNC_BIT			BIT(5)
#define GPIO_INT_FUNC_BIT			BIT(6)

#define GPIO_SCR_SEC_WEN			BIT(28)
#define GPIO_SCR_SEC_REN			BIT(27)
#define GPIO_SCR_SEC_G1W			BIT(9)
#define GPIO_SCR_SEC_G1R			BIT(1)
#define GPIO_FULL_ACCESS			(GPIO_SCR_SEC_WEN | \
						 GPIO_SCR_SEC_REN | \
						 GPIO_SCR_SEC_G1R | \
						 GPIO_SCR_SEC_G1W)

#define GPIO_INT_LVL_LEVEL_TRIGGER		0x1
#define GPIO_INT_LVL_SINGLE_EDGE_TRIGGER	0x2
#define GPIO_INT_LVL_BOTH_EDGE_TRIGGER		0x3

#define TRIGGER_LEVEL_LOW			0x0
#define TRIGGER_LEVEL_HIGH			0x1

#define GPIO_STATUS_G1				0x04

#define MAX_GPIO_CONTROLLERS			7
#define MAX_GPIO_PORTS				8

#define GPIO_PORT(g)				((g) >> 3)
#define GPIO_PIN(g)				((g) & 0x7)

struct tegra_gpio_port_soc_info {
	const char *port_name;
	int cont_id;
	int port_index;
	int valid_pins;
	int scr_offset;
	u32 reg_offset;
};

#define TEGRA_MAIN_GPIO_PORT_INFO(port, cid, cind, npins)	\
[TEGRA_MAIN_GPIO_PORT_##port] = {				\
		.port_name = #port,				\
		.cont_id = cid,					\
		.port_index = cind,				\
		.valid_pins = npins,				\
		.scr_offset = cid * 0x1000 + cind * 0x40,	\
		.reg_offset = cid * 0x1000 + cind * 0x200,	\
}

#define TEGRA_AON_GPIO_PORT_INFO(port, cid, cind, npins)	\
[TEGRA_AON_GPIO_PORT_##port] = {				\
		.port_name = #port,				\
		.cont_id = cid,					\
		.port_index = cind,				\
		.valid_pins = npins,				\
		.scr_offset = cind * 0x40,			\
		.reg_offset = cind * 0x200,			\
}

static struct tegra_gpio_port_soc_info tegra_main_gpio_cinfo[] = {
	TEGRA_MAIN_GPIO_PORT_INFO(A, 2, 0, 7),
	TEGRA_MAIN_GPIO_PORT_INFO(B, 3, 0, 7),
	TEGRA_MAIN_GPIO_PORT_INFO(C, 3, 1, 7),
	TEGRA_MAIN_GPIO_PORT_INFO(D, 3, 2, 6),
	TEGRA_MAIN_GPIO_PORT_INFO(E, 2, 1, 8),
	TEGRA_MAIN_GPIO_PORT_INFO(F, 2, 2, 6),
	TEGRA_MAIN_GPIO_PORT_INFO(G, 4, 1, 6),
	TEGRA_MAIN_GPIO_PORT_INFO(H, 1, 0, 7),
	TEGRA_MAIN_GPIO_PORT_INFO(I, 0, 4, 8),
	TEGRA_MAIN_GPIO_PORT_INFO(J, 5, 0, 8),
	TEGRA_MAIN_GPIO_PORT_INFO(K, 5, 1, 1),
	TEGRA_MAIN_GPIO_PORT_INFO(L, 1, 1, 8),
	TEGRA_MAIN_GPIO_PORT_INFO(M, 5, 3, 6),
	TEGRA_MAIN_GPIO_PORT_INFO(N, 0, 0, 7),
	TEGRA_MAIN_GPIO_PORT_INFO(O, 0, 1, 4),
	TEGRA_MAIN_GPIO_PORT_INFO(P, 4, 0, 7),
	TEGRA_MAIN_GPIO_PORT_INFO(Q, 0, 2, 6),
	TEGRA_MAIN_GPIO_PORT_INFO(R, 0, 5, 6),
	TEGRA_MAIN_GPIO_PORT_INFO(T, 0, 3, 4),
	TEGRA_MAIN_GPIO_PORT_INFO(X, 1, 2, 8),
	TEGRA_MAIN_GPIO_PORT_INFO(Y, 1, 3, 7),
	TEGRA_MAIN_GPIO_PORT_INFO(BB, 2, 3, 2),
	TEGRA_MAIN_GPIO_PORT_INFO(CC, 5, 2, 4),
};

static struct tegra_gpio_port_soc_info tegra_aon_gpio_cinfo[] = {
	TEGRA_AON_GPIO_PORT_INFO(S, 0, 1, 5),
	TEGRA_AON_GPIO_PORT_INFO(U, 0, 2, 6),
	TEGRA_AON_GPIO_PORT_INFO(V, 0, 4, 8),
	TEGRA_AON_GPIO_PORT_INFO(W, 0, 5, 8),
	TEGRA_AON_GPIO_PORT_INFO(Z, 0, 7, 4),
	TEGRA_AON_GPIO_PORT_INFO(AA, 0, 6, 8),
	TEGRA_AON_GPIO_PORT_INFO(EE, 0, 3, 3),
	TEGRA_AON_GPIO_PORT_INFO(FF, 0, 0, 5),
};

struct tegra_gpio_info;

struct tegra_gpio_soc_info {
	const char *name;
	const struct tegra_gpio_port_soc_info *port;
	int nports;
};

struct tegra_gpio_controller {
	int controller;
	int irq;
	struct tegra_gpio_info *tgi;
};

struct tegra_gpio_info {
	struct device *dev;
	int nbanks;
	void __iomem *gpio_regs;
	void __iomem *scr_regs;
	struct irq_domain *irq_domain;
	const struct tegra_gpio_soc_info *soc;
	struct tegra_gpio_controller tg_contrlr[MAX_GPIO_CONTROLLERS];
	struct gpio_chip gc;
	struct irq_chip ic;
};

#define GPIO_CNTRL_REG(tgi, gpio, roffset)				    \
	((tgi)->gpio_regs + (tgi)->soc->port[GPIO_PORT(gpio)].reg_offset + \
	(GPIO_REG_DIFF * GPIO_PIN(gpio)) + (roffset))

static u32 tegra_gpio_readl(struct tegra_gpio_info *tgi, u32 gpio,
				   u32 reg_offset)
{
	return __raw_readl(GPIO_CNTRL_REG(tgi, gpio, reg_offset));
}

static void tegra_gpio_writel(struct tegra_gpio_info *tgi, u32 val,
				     u32 gpio, u32 reg_offset)
{
	__raw_writel(val, GPIO_CNTRL_REG(tgi, gpio, reg_offset));
}

static void tegra_gpio_update(struct tegra_gpio_info *tgi, u32 gpio,
				     u32 reg_offset,	u32 mask, u32 val)
{
	u32 rval;

	rval = __raw_readl(GPIO_CNTRL_REG(tgi, gpio, reg_offset));
	rval = (rval & ~mask) | (val & mask);
	__raw_writel(rval, GPIO_CNTRL_REG(tgi, gpio, reg_offset));
}

/* This function will return if the GPIO is accessible by CPU */
static bool gpio_is_accessible(struct tegra_gpio_info *tgi, u32 offset)
{
	int port = GPIO_PORT(offset);
	int pin = GPIO_PIN(offset);
	u32 val;
	int cont_id;
	u32 scr_offset = tgi->soc->port[port].scr_offset;

	if (pin >= tgi->soc->port[port].valid_pins)
		return false;

	cont_id = tgi->soc->port[port].cont_id;
	if (cont_id  < 0)
		return false;

	val = __raw_readl(tgi->scr_regs + scr_offset +
			(pin * GPIO_SCR_DIFF) + GPIO_SCR_REG);

	if ((val & GPIO_FULL_ACCESS) == GPIO_FULL_ACCESS)
		return true;

	return false;
}

static void tegra_gpio_enable(struct tegra_gpio_info *tgi, int gpio)
{
	tegra_gpio_update(tgi, gpio, GPIO_ENB_CONFIG_REG, 0x1, 0x1);
}

static void tegra_gpio_disable(struct tegra_gpio_info *tgi, int gpio)
{
	tegra_gpio_update(tgi, gpio, GPIO_ENB_CONFIG_REG, 0x1, 0x0);
}

static void tegra_gpio_free(struct gpio_chip *chip, unsigned int offset)
{
	struct tegra_gpio_info *tgi = gpiochip_get_data(chip);

	tegra_gpio_disable(tgi, offset);
}

static void tegra_gpio_set(struct gpio_chip *chip, unsigned int offset,
			   int value)
{
	struct tegra_gpio_info *tgi = gpiochip_get_data(chip);
	u32 val = (value) ? 0x1 : 0x0;

	tegra_gpio_writel(tgi, val, offset, GPIO_OUT_VAL_REG);
	tegra_gpio_writel(tgi, 0, offset, GPIO_OUT_CTRL_REG);
}

static int tegra_gpio_get(struct gpio_chip *chip, unsigned int offset)
{
	struct tegra_gpio_info *tgi = gpiochip_get_data(chip);
	u32 val;

	val = tegra_gpio_readl(tgi, offset, GPIO_ENB_CONFIG_REG);
	if (val & GPIO_INOUT_BIT)
		return tegra_gpio_readl(tgi, offset, GPIO_OUT_VAL_REG) & 0x1;

	return tegra_gpio_readl(tgi, offset, GPIO_INPUT_REG) & 0x1;
}

static void set_gpio_direction_mode(struct gpio_chip *chip, u32 offset,
				    bool mode)
{
	struct tegra_gpio_info *tgi = gpiochip_get_data(chip);
	u32 val;

	val = tegra_gpio_readl(tgi, offset, GPIO_ENB_CONFIG_REG);
	if (mode)
		val |= GPIO_INOUT_BIT;
	else
		val &= ~GPIO_INOUT_BIT;
	tegra_gpio_writel(tgi, val, offset, GPIO_ENB_CONFIG_REG);
}

static int tegra_gpio_direction_input(struct gpio_chip *chip,
				      unsigned int offset)
{
	struct tegra_gpio_info *tgi = gpiochip_get_data(chip);

	set_gpio_direction_mode(chip, offset, 0);
	tegra_gpio_enable(tgi, offset);

	return 0;
}

static int tegra_gpio_direction_output(struct gpio_chip *chip,
				       unsigned int offset, int value)
{
	struct tegra_gpio_info *tgi = gpiochip_get_data(chip);

	tegra_gpio_set(chip, offset, value);
	set_gpio_direction_mode(chip, offset, 1);
	tegra_gpio_enable(tgi, offset);

	return 0;
}

static int tegra_gpio_set_debounce(struct gpio_chip *chip, unsigned int offset,
				   unsigned int debounce)
{
	struct tegra_gpio_info *tgi = gpiochip_get_data(chip);
	unsigned int dbc_ms = DIV_ROUND_UP(debounce, 1000);

	tegra_gpio_update(tgi, offset, GPIO_ENB_CONFIG_REG, 0x1, 0x1);
	tegra_gpio_update(tgi, offset, GPIO_DEB_FUNC_BIT, 0x5, 0x1);

	/* Update debounce threshold */
	tegra_gpio_writel(tgi, dbc_ms, offset, GPIO_DBC_THRES_REG);

	return 0;
}

static int tegra_gpio_get_direction(struct gpio_chip *chip, unsigned int offset)
{
	struct tegra_gpio_info *tgi = gpiochip_get_data(chip);
	u32 val;

	if (!gpio_is_accessible(tgi, offset))
		return 0;

	val = tegra_gpio_readl(tgi, offset, GPIO_OUT_CTRL_REG);

	return (val & 0x1);
}

static int tegra_gpio_to_irq(struct gpio_chip *chip, unsigned int offset)
{
	struct tegra_gpio_info *tgi = gpiochip_get_data(chip);

	return irq_find_mapping(tgi->irq_domain, offset);
}

static void tegra_gpio_irq_ack(struct irq_data *d)
{
	struct tegra_gpio_controller *ctrlr = irq_data_get_irq_chip_data(d);

	tegra_gpio_writel(ctrlr->tgi, 1, d->hwirq, GPIO_INT_CLEAR_REG);
}

static void tegra_gpio_irq_mask(struct irq_data *d)
{
	struct tegra_gpio_controller *c = irq_data_get_irq_chip_data(d);

	tegra_gpio_update(c->tgi, d->hwirq, GPIO_ENB_CONFIG_REG,
			  GPIO_INT_FUNC_BIT, 0);
}

static void tegra_gpio_irq_unmask(struct irq_data *d)
{
	struct tegra_gpio_controller *c = irq_data_get_irq_chip_data(d);

	tegra_gpio_update(c->tgi, d->hwirq, GPIO_ENB_CONFIG_REG,
			  GPIO_INT_FUNC_BIT, GPIO_INT_FUNC_BIT);
}

static int tegra_gpio_irq_set_type(struct irq_data *d, unsigned int type)
{
	struct tegra_gpio_controller *ctrlr = irq_data_get_irq_chip_data(d);
	int gpio = d->hwirq;
	u32 lvl_type;
	u32 trg_type;
	u32 val;

	switch (type & IRQ_TYPE_SENSE_MASK) {
	case IRQ_TYPE_EDGE_RISING:
		trg_type = TRIGGER_LEVEL_HIGH;
		lvl_type = GPIO_INT_LVL_SINGLE_EDGE_TRIGGER;
		break;

	case IRQ_TYPE_EDGE_FALLING:
		trg_type = TRIGGER_LEVEL_LOW;
		lvl_type = GPIO_INT_LVL_SINGLE_EDGE_TRIGGER;
		break;

	case IRQ_TYPE_EDGE_BOTH:
		lvl_type = GPIO_INT_LVL_BOTH_EDGE_TRIGGER;
		trg_type = 0;
		break;

	case IRQ_TYPE_LEVEL_HIGH:
		trg_type = TRIGGER_LEVEL_HIGH;
		lvl_type = GPIO_INT_LVL_LEVEL_TRIGGER;
		break;

	case IRQ_TYPE_LEVEL_LOW:
		trg_type = TRIGGER_LEVEL_LOW;
		lvl_type = GPIO_INT_LVL_LEVEL_TRIGGER;
		break;

	default:
		return -EINVAL;
	}

	trg_type = trg_type << 0x4;
	lvl_type = lvl_type << 0x2;

	/* Clear and Program the values */
	val = tegra_gpio_readl(ctrlr->tgi, gpio, GPIO_ENB_CONFIG_REG);
	val &= ~((0x3 << GPIO_TRG_TYPE_BIT_OFFSET) | (GPIO_TRG_LVL_BIT));
	val |= trg_type | lvl_type;
	tegra_gpio_writel(ctrlr->tgi, val, gpio, GPIO_ENB_CONFIG_REG);

	tegra_gpio_enable(ctrlr->tgi, gpio);

	if (type & (IRQ_TYPE_LEVEL_LOW | IRQ_TYPE_LEVEL_HIGH))
		irq_set_handler_locked(d, handle_level_irq);
	else if (type & (IRQ_TYPE_EDGE_FALLING | IRQ_TYPE_EDGE_RISING))
		irq_set_handler_locked(d, handle_edge_irq);

	return 0;
}

static void tegra_gpio_irq_handler(struct irq_desc *desc)
{
	struct irq_chip *chip = irq_desc_get_chip(desc);
	struct tegra_gpio_controller *tg_cont = irq_desc_get_handler_data(desc);
	struct tegra_gpio_info *tgi = tg_cont->tgi;
	int pin;
	int port;
	u32 i;
	unsigned long val;
	u32 gpio;
	u32 addr;
	int port_map[MAX_GPIO_PORTS];

	for (i = 0; i < MAX_GPIO_PORTS; ++i)
		port_map[i] = -1;

	for (i = 0; i < tgi->soc->nports; ++i) {
		if (tgi->soc->port[i].cont_id == tg_cont->controller)
			port_map[tgi->soc->port[i].port_index] = i;
	}

	chained_irq_enter(chip, desc);
	for (i = 0; i < MAX_GPIO_PORTS; i++) {
		port = port_map[i];
		if (port == -1)
			continue;

		addr = tgi->soc->port[port].reg_offset;
		val = __raw_readl(tg_cont->tgi->gpio_regs + addr +
				GPIO_INT_STATUS_OFFSET + GPIO_STATUS_G1);
		gpio = tgi->gc.base + (port * 8);
		for_each_set_bit(pin, &val, 8)
			generic_handle_irq(gpio_to_irq(gpio + pin));
	}

	chained_irq_exit(chip, desc);
}

#ifdef CONFIG_DEBUG_FS

#include <linux/debugfs.h>
#include <linux/seq_file.h>

static int dbg_gpio_show(struct seq_file *s, void *unused)
{
	struct tegra_gpio_info *tgi = s->private;
	int i;

	seq_puts(s, "Port:Pin:ENB DBC IN OUT_CTRL OUT_VAL INT_CLR\n");
	for (i = 0; i < tgi->gc.ngpio; i++) {
		if (!gpio_is_accessible(tgi, i))
			continue;
		seq_printf(s, "%s:%d 0x%x 0x%x 0x%x 0x%x 0x%x 0x%x\n",
			   tgi->soc->port[GPIO_PORT(i)].port_name, i % 8,
			   tegra_gpio_readl(tgi, i, GPIO_ENB_CONFIG_REG),
			   tegra_gpio_readl(tgi, i, GPIO_DBC_THRES_REG),
			   tegra_gpio_readl(tgi, i, GPIO_INPUT_REG),
			   tegra_gpio_readl(tgi, i, GPIO_OUT_CTRL_REG),
			   tegra_gpio_readl(tgi, i, GPIO_OUT_VAL_REG),
			   tegra_gpio_readl(tgi, i, GPIO_INT_CLEAR_REG));
	}

	return 0;
}

static int dbg_gpio_open(struct inode *inode, struct file *file)
{
	return single_open(file, dbg_gpio_show, inode->i_private);
}

static const struct file_operations debug_fops = {
	.open		= dbg_gpio_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static int __init tegra_gpio_debuginit(struct tegra_gpio_info *tgi)
{
	(void)debugfs_create_file(tgi->soc->name, 0444, NULL, tgi, &debug_fops);

	return 0;
}
#else
static int tegra_gpio_debuginit(struct tegra_gpio_info *tgi)
{
	return 0;
}
#endif

static int tegra_gpio_probe(struct platform_device *pdev)
{
	struct tegra_gpio_info *tgi;
	struct resource *res;
	int bank;
	int gpio;
	int ret;

	for (bank = 0;; bank++) {
		res = platform_get_resource(pdev, IORESOURCE_IRQ, bank);
		if (!res)
			break;
	}
	if (!bank) {
		dev_err(&pdev->dev, "No GPIO Controller found\n");
		return -ENODEV;
	}

	tgi = devm_kzalloc(&pdev->dev, sizeof(*tgi), GFP_KERNEL);
	if (!tgi)
		return -ENOMEM;
	tgi->dev = &pdev->dev;
	tgi->nbanks = bank;
	tgi->soc = of_device_get_match_data(&pdev->dev);

	tgi->gc.label			= tgi->soc->name;
	tgi->gc.free			= tegra_gpio_free;
	tgi->gc.direction_input		= tegra_gpio_direction_input;
	tgi->gc.get			= tegra_gpio_get;
	tgi->gc.direction_output	= tegra_gpio_direction_output;
	tgi->gc.set			= tegra_gpio_set;
	tgi->gc.get_direction		= tegra_gpio_get_direction;
	tgi->gc.to_irq			= tegra_gpio_to_irq;
	tgi->gc.set_debounce		= tegra_gpio_set_debounce;
	tgi->gc.base			= -1;
	tgi->gc.ngpio			= tgi->soc->nports * 8;
	tgi->gc.parent			= &pdev->dev;
	tgi->gc.of_node			= pdev->dev.of_node;

	tgi->ic.name			= tgi->soc->name;
	tgi->ic.irq_ack			= tegra_gpio_irq_ack;
	tgi->ic.irq_mask		= tegra_gpio_irq_mask;
	tgi->ic.irq_unmask		= tegra_gpio_irq_unmask;
	tgi->ic.irq_set_type		= tegra_gpio_irq_set_type;
	tgi->ic.irq_shutdown		= tegra_gpio_irq_mask;
	tgi->ic.irq_disable		= tegra_gpio_irq_mask;

	platform_set_drvdata(pdev, tgi);

	for (bank = 0; bank < tgi->nbanks; bank++) {
		res = platform_get_resource(pdev, IORESOURCE_IRQ, bank);
		tgi->tg_contrlr[bank].controller = bank;
		tgi->tg_contrlr[bank].irq = res->start;
		tgi->tg_contrlr[bank].tgi = tgi;
	}

	tgi->irq_domain = irq_domain_add_linear(pdev->dev.of_node,
						tgi->gc.ngpio,
						&irq_domain_simple_ops, NULL);
	if (!tgi->irq_domain)
		return -ENODEV;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "security");
	if (!res) {
		dev_err(&pdev->dev, "Missing security MEM resource\n");
		return -ENODEV;
	}
	tgi->scr_regs = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(tgi->scr_regs)) {
		ret = PTR_ERR(tgi->scr_regs);
		dev_err(&pdev->dev, "Failed to iomap for security: %d\n", ret);
		return ret;
	}

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "gpio");
	if (!res) {
		dev_err(&pdev->dev, "Missing gpio MEM resource\n");
		return -ENODEV;
	}
	tgi->gpio_regs = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(tgi->gpio_regs)) {
		ret = PTR_ERR(tgi->gpio_regs);
		dev_err(&pdev->dev, "Failed to iomap for gpio: %d\n", ret);
		return ret;
	}

	ret = devm_gpiochip_add_data(&pdev->dev, &tgi->gc, tgi);
	if (ret < 0) {
		dev_err(&pdev->dev, "Could not register gpiochip, %d\n", ret);
		return ret;
	}

	for (gpio = 0; gpio < tgi->gc.ngpio; gpio++) {
		int irq = irq_create_mapping(tgi->irq_domain, gpio);
		int cont_id = tgi->soc->port[GPIO_PORT(gpio)].cont_id;

		if (gpio_is_accessible(tgi, gpio))
			/* mask interrupts for this GPIO */
			tegra_gpio_update(tgi, gpio, GPIO_ENB_CONFIG_REG,
					  GPIO_INT_FUNC_BIT, 0);

		irq_set_chip_data(irq, &tgi->tg_contrlr[cont_id]);
		irq_set_chip_and_handler(irq, &tgi->ic, handle_simple_irq);
	}

	for (bank = 0; bank < tgi->nbanks; bank++)
		irq_set_chained_handler_and_data(tgi->tg_contrlr[bank].irq,
						 tegra_gpio_irq_handler,
						 &tgi->tg_contrlr[bank]);

	tegra_gpio_debuginit(tgi);

	return 0;
}

static const struct tegra_gpio_soc_info t186_main_gpio_soc = {
	.name = "tegra-main-gpio",
	.port = tegra_main_gpio_cinfo,
	.nports = ARRAY_SIZE(tegra_main_gpio_cinfo),
};

static const struct tegra_gpio_soc_info t186_aon_gpio_soc = {
	.name = "tegra-aon-gpio",
	.port = tegra_aon_gpio_cinfo,
	.nports = ARRAY_SIZE(tegra_aon_gpio_cinfo),
};

static const struct of_device_id tegra_gpio_of_match[] = {
	{ .compatible = "nvidia,tegra186-gpio", .data = &t186_main_gpio_soc},
	{ .compatible = "nvidia,tegra186-gpio-aon", .data = &t186_aon_gpio_soc},
	{ },
};

static struct platform_driver tegra_gpio_driver = {
	.driver		= {
		.name	= "tegra186-gpio",
		.of_match_table = tegra_gpio_of_match,
	},
	.probe		= tegra_gpio_probe,
};

static int __init tegra_gpio_init(void)
{
	return platform_driver_register(&tegra_gpio_driver);
}
postcore_initcall(tegra_gpio_init);

MODULE_AUTHOR("Suresh Mangipudi <smangipudi@nvidia.com>");
MODULE_AUTHOR("Laxman Dewangan <ldewangan@nvidia.com>");
MODULE_DESCRIPTION("NVIDIA Tegra186 GPIO driver");
MODULE_LICENSE("GPL v2");
