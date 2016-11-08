/*
 * Copyright (c) 2011, Code Aurora Forum. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#define pr_fmt(fmt) "%s: " fmt, __func__

#include <linux/kernel.h>
#include <linux/interrupt.h>
#include <linux/irqchip/chained_irq.h>
#include <linux/irq.h>
#include <linux/irqdomain.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/err.h>
#include <linux/ssbi.h>
#include <linux/regmap.h>
#include <linux/of_platform.h>
#include <linux/mfd/core.h>

#define	SSBI_REG_ADDR_IRQ_BASE		0x1BB
#define	SSBI_PM8821_REG_ADDR_IRQ_BASE	0x100

#define	SSBI_REG_ADDR_IRQ_ROOT		(0)
#define	SSBI_REG_ADDR_IRQ_M_STATUS1	(1)
#define	SSBI_REG_ADDR_IRQ_M_STATUS2	(2)
#define	SSBI_REG_ADDR_IRQ_M_STATUS3	(3)
#define	SSBI_REG_ADDR_IRQ_M_STATUS4	(4)
#define	SSBI_REG_ADDR_IRQ_BLK_SEL	(5)
#define	SSBI_REG_ADDR_IRQ_IT_STATUS	(6)
#define	SSBI_REG_ADDR_IRQ_CONFIG	(7)
#define	SSBI_REG_ADDR_IRQ_RT_STATUS	(8)

#define	PM8821_TOTAL_IRQ_MASTERS	2
#define	PM8821_BLOCKS_PER_MASTER	7
#define	PM8821_IRQ_MASTER1_SET		0x01
#define	PM8821_IRQ_CLEAR_OFFSET		0x01
#define	PM8821_IRQ_RT_STATUS_OFFSET	0x0f
#define	PM8821_IRQ_MASK_REG_OFFSET	0x08
#define	SSBI_REG_ADDR_IRQ_MASTER0	0x30
#define	SSBI_REG_ADDR_IRQ_MASTER1	0xb0

#define	PM_IRQF_LVL_SEL			0x01	/* level select */
#define	PM_IRQF_MASK_FE			0x02	/* mask falling edge */
#define	PM_IRQF_MASK_RE			0x04	/* mask rising edge */
#define	PM_IRQF_CLR			0x08	/* clear interrupt */
#define	PM_IRQF_BITS_MASK		0x70
#define	PM_IRQF_BITS_SHIFT		4
#define	PM_IRQF_WRITE			0x80

#define	PM_IRQF_MASK_ALL		(PM_IRQF_MASK_FE | \
					PM_IRQF_MASK_RE)

#define REG_HWREV		0x002  /* PMIC4 revision */
#define REG_HWREV_2		0x0E8  /* PMIC4 revision 2 */

#define PM8921_NR_IRQS		256
#define PM8821_NR_IRQS		112

struct pm_irq_chip {
	struct regmap		*regmap;
	spinlock_t		pm_irq_lock;
	struct irq_domain	*irqdomain;
	unsigned int		irq_reg_base;
	unsigned int		num_irqs;
	unsigned int		num_blocks;
	unsigned int		num_masters;
	u8			config[0];
};

struct pm8xxx_data {
	int num_irqs;
	unsigned int		irq_reg_base;
	const struct irq_domain_ops  *irq_domain_ops;
	void (*irq_handler)(struct irq_desc *desc);
};

static int pm8xxx_read_block_irq(struct pm_irq_chip *chip, unsigned int bp,
				 unsigned int *ip)
{
	int	rc;

	spin_lock(&chip->pm_irq_lock);
	rc = regmap_write(chip->regmap,
			  chip->irq_reg_base + SSBI_REG_ADDR_IRQ_BLK_SEL, bp);
	if (rc) {
		pr_err("Failed Selecting Block %d rc=%d\n", bp, rc);
		goto bail;
	}

	rc = regmap_read(chip->regmap,
			 chip->irq_reg_base + SSBI_REG_ADDR_IRQ_IT_STATUS, ip);
	if (rc)
		pr_err("Failed Reading Status rc=%d\n", rc);
bail:
	spin_unlock(&chip->pm_irq_lock);
	return rc;
}

static int
pm8xxx_config_irq(struct pm_irq_chip *chip, unsigned int bp, unsigned int cp)
{
	int	rc;

	spin_lock(&chip->pm_irq_lock);
	rc = regmap_write(chip->regmap,
			  chip->irq_reg_base + SSBI_REG_ADDR_IRQ_BLK_SEL, bp);
	if (rc) {
		pr_err("Failed Selecting Block %d rc=%d\n", bp, rc);
		goto bail;
	}

	cp |= PM_IRQF_WRITE;
	rc = regmap_write(chip->regmap,
			  chip->irq_reg_base + SSBI_REG_ADDR_IRQ_CONFIG, cp);
	if (rc)
		pr_err("Failed Configuring IRQ rc=%d\n", rc);
bail:
	spin_unlock(&chip->pm_irq_lock);
	return rc;
}

static int pm8xxx_irq_block_handler(struct pm_irq_chip *chip, int block)
{
	int pmirq, irq, i, ret = 0;
	unsigned int bits;

	ret = pm8xxx_read_block_irq(chip, block, &bits);
	if (ret) {
		pr_err("Failed reading %d block ret=%d", block, ret);
		return ret;
	}
	if (!bits) {
		pr_err("block bit set in master but no irqs: %d", block);
		return 0;
	}

	/* Check IRQ bits */
	for (i = 0; i < 8; i++) {
		if (bits & (1 << i)) {
			pmirq = block * 8 + i;
			irq = irq_find_mapping(chip->irqdomain, pmirq);
			generic_handle_irq(irq);
		}
	}
	return 0;
}

static int pm8xxx_irq_master_handler(struct pm_irq_chip *chip, int master)
{
	unsigned int blockbits;
	int block_number, i, ret = 0;

	ret = regmap_read(chip->regmap, chip->irq_reg_base +
			  SSBI_REG_ADDR_IRQ_M_STATUS1 + master, &blockbits);
	if (ret) {
		pr_err("Failed to read master %d ret=%d\n", master, ret);
		return ret;
	}
	if (!blockbits) {
		pr_err("master bit set in root but no blocks: %d", master);
		return 0;
	}

	for (i = 0; i < 8; i++)
		if (blockbits & (1 << i)) {
			block_number = master * 8 + i;	/* block # */
			ret |= pm8xxx_irq_block_handler(chip, block_number);
		}
	return ret;
}

static void pm8xxx_irq_handler(struct irq_desc *desc)
{
	struct pm_irq_chip *chip = irq_desc_get_handler_data(desc);
	struct irq_chip *irq_chip = irq_desc_get_chip(desc);
	unsigned int root;
	int	i, ret, masters = 0;

	chained_irq_enter(irq_chip, desc);

	ret = regmap_read(chip->regmap,
			  chip->irq_reg_base + SSBI_REG_ADDR_IRQ_ROOT, &root);
	if (ret) {
		pr_err("Can't read root status ret=%d\n", ret);
		return;
	}

	/* on pm8xxx series masters start from bit 1 of the root */
	masters = root >> 1;

	/* Read allowed masters for blocks. */
	for (i = 0; i < chip->num_masters; i++)
		if (masters & (1 << i))
			pm8xxx_irq_master_handler(chip, i);

	chained_irq_exit(irq_chip, desc);
}

static int pm8821_read_master_irq(const struct pm_irq_chip *chip,
				  int m, unsigned int *master)
{
	unsigned int base;

	if (!m)
		base = chip->irq_reg_base + SSBI_REG_ADDR_IRQ_MASTER0;
	else
		base = chip->irq_reg_base + SSBI_REG_ADDR_IRQ_MASTER1;

	return regmap_read(chip->regmap, base, master);
}

static int pm8821_read_block_irq(struct pm_irq_chip *chip, int master,
				 u8 block, unsigned int *bits)
{
	int rc;

	unsigned int base;

	if (!master)
		base = chip->irq_reg_base + SSBI_REG_ADDR_IRQ_MASTER0;
	else
		base = chip->irq_reg_base + SSBI_REG_ADDR_IRQ_MASTER1;

	spin_lock(&chip->pm_irq_lock);

	rc = regmap_read(chip->regmap, base + block, bits);
	if (rc)
		pr_err("Failed Reading Status rc=%d\n", rc);

	spin_unlock(&chip->pm_irq_lock);
	return rc;
}

static int pm8821_irq_block_handler(struct pm_irq_chip *chip,
				    int master_number, int block)
{
	int pmirq, irq, i, ret;
	unsigned int bits;

	ret = pm8821_read_block_irq(chip, master_number, block, &bits);
	if (ret) {
		pr_err("Failed reading %d block ret=%d", block, ret);
		return ret;
	}
	if (!bits) {
		pr_err("block bit set in master but no irqs: %d", block);
		return 0;
	}

	/* Convert block offset to global block number */
	block += (master_number * PM8821_BLOCKS_PER_MASTER) - 1;

	/* Check IRQ bits */
	for (i = 0; i < 8; i++) {
		if (bits & BIT(i)) {
			pmirq = block * 8 + i;
			irq = irq_find_mapping(chip->irqdomain, pmirq);
			generic_handle_irq(irq);
		}
	}

	return 0;
}

static int pm8821_irq_read_master(struct pm_irq_chip *chip,
				int master_number, u8 master_val)
{
	int ret = 0;
	int block;

	for (block = 1; block < 8; block++) {
		if (master_val & BIT(block)) {
			ret |= pm8821_irq_block_handler(chip,
					master_number, block);
		}
	}

	return ret;
}

static void pm8821_irq_handler(struct irq_desc *desc)
{
	struct pm_irq_chip *chip = irq_desc_get_handler_data(desc);
	struct irq_chip *irq_chip = irq_desc_get_chip(desc);
	int ret;
	unsigned int master;

	chained_irq_enter(irq_chip, desc);
	/* check master 0 */
	ret = pm8821_read_master_irq(chip, 0, &master);
	if (ret) {
		pr_err("Failed to re:Qad master 0 ret=%d\n", ret);
		return;
	}

	if (master & ~PM8821_IRQ_MASTER1_SET)
		pm8821_irq_read_master(chip, 0, master);

	/* check master 1 */
	if (!(master & PM8821_IRQ_MASTER1_SET))
		goto done;

	ret = pm8821_read_master_irq(chip, 1, &master);
	if (ret) {
		pr_err("Failed to read master 1 ret=%d\n", ret);
		return;
	}

	pm8821_irq_read_master(chip, 1, master);

done:
	chained_irq_exit(irq_chip, desc);
}

static void pm8xxx_irq_mask_ack(struct irq_data *d)
{
	struct pm_irq_chip *chip = irq_data_get_irq_chip_data(d);
	unsigned int pmirq = irqd_to_hwirq(d);
	u8	block, config;

	block = pmirq / 8;

	config = chip->config[pmirq] | PM_IRQF_MASK_ALL | PM_IRQF_CLR;
	pm8xxx_config_irq(chip, block, config);
}

static void pm8xxx_irq_unmask(struct irq_data *d)
{
	struct pm_irq_chip *chip = irq_data_get_irq_chip_data(d);
	unsigned int pmirq = irqd_to_hwirq(d);
	u8	block, config;

	block = pmirq / 8;

	config = chip->config[pmirq];
	pm8xxx_config_irq(chip, block, config);
}

static int pm8xxx_irq_set_type(struct irq_data *d, unsigned int flow_type)
{
	struct pm_irq_chip *chip = irq_data_get_irq_chip_data(d);
	unsigned int pmirq = irqd_to_hwirq(d);
	int irq_bit;
	u8 block, config;

	block = pmirq / 8;
	irq_bit  = pmirq % 8;

	chip->config[pmirq] = (irq_bit << PM_IRQF_BITS_SHIFT)
							| PM_IRQF_MASK_ALL;
	if (flow_type & (IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING)) {
		if (flow_type & IRQF_TRIGGER_RISING)
			chip->config[pmirq] &= ~PM_IRQF_MASK_RE;
		if (flow_type & IRQF_TRIGGER_FALLING)
			chip->config[pmirq] &= ~PM_IRQF_MASK_FE;
	} else {
		chip->config[pmirq] |= PM_IRQF_LVL_SEL;

		if (flow_type & IRQF_TRIGGER_HIGH)
			chip->config[pmirq] &= ~PM_IRQF_MASK_RE;
		else
			chip->config[pmirq] &= ~PM_IRQF_MASK_FE;
	}

	config = chip->config[pmirq] | PM_IRQF_CLR;
	return pm8xxx_config_irq(chip, block, config);
}

static int pm8xxx_irq_get_irqchip_state(struct irq_data *d,
					enum irqchip_irq_state which,
					bool *state)
{
	struct pm_irq_chip *chip = irq_data_get_irq_chip_data(d);
	unsigned int pmirq = irqd_to_hwirq(d);
	unsigned int bits;
	int irq_bit;
	u8 block;
	int rc;

	if (which != IRQCHIP_STATE_LINE_LEVEL)
		return -EINVAL;

	block = pmirq / 8;
	irq_bit = pmirq % 8;

	spin_lock(&chip->pm_irq_lock);
	rc = regmap_write(chip->regmap, chip->irq_reg_base +
			  SSBI_REG_ADDR_IRQ_BLK_SEL, block);
	if (rc) {
		pr_err("Failed Selecting Block %d rc=%d\n", block, rc);
		goto bail;
	}

	rc = regmap_read(chip->regmap, chip->irq_reg_base +
			 SSBI_REG_ADDR_IRQ_RT_STATUS, &bits);
	if (rc) {
		pr_err("Failed Reading Status rc=%d\n", rc);
		goto bail;
	}

	*state = !!(bits & BIT(irq_bit));
bail:
	spin_unlock(&chip->pm_irq_lock);

	return rc;
}

static struct irq_chip pm8xxx_irq_chip = {
	.name		= "pm8xxx",
	.irq_mask_ack	= pm8xxx_irq_mask_ack,
	.irq_unmask	= pm8xxx_irq_unmask,
	.irq_set_type	= pm8xxx_irq_set_type,
	.irq_get_irqchip_state = pm8xxx_irq_get_irqchip_state,
	.flags		= IRQCHIP_MASK_ON_SUSPEND | IRQCHIP_SKIP_SET_WAKE,
};

static int pm8xxx_irq_domain_map(struct irq_domain *d, unsigned int irq,
				   irq_hw_number_t hwirq)
{
	struct pm_irq_chip *chip = d->host_data;

	irq_set_chip_and_handler(irq, &pm8xxx_irq_chip, handle_level_irq);
	irq_set_chip_data(irq, chip);
	irq_set_noprobe(irq);

	return 0;
}

static const struct irq_domain_ops pm8xxx_irq_domain_ops = {
	.xlate = irq_domain_xlate_twocell,
	.map = pm8xxx_irq_domain_map,
};

static void pm8821_irq_mask_ack(struct irq_data *d)
{
	struct pm_irq_chip *chip = irq_data_get_irq_chip_data(d);
	unsigned int base, pmirq = irqd_to_hwirq(d);
	u8 block, master;
	int irq_bit, rc;

	block = pmirq / 8;
	master = block / PM8821_BLOCKS_PER_MASTER;
	irq_bit = pmirq % 8;
	block %= PM8821_BLOCKS_PER_MASTER;

	if (!master)
		base = chip->irq_reg_base + SSBI_REG_ADDR_IRQ_MASTER0;
	else
		base = chip->irq_reg_base + SSBI_REG_ADDR_IRQ_MASTER1;

	spin_lock(&chip->pm_irq_lock);
	rc = regmap_update_bits(chip->regmap,
				base + PM8821_IRQ_MASK_REG_OFFSET + block,
				BIT(irq_bit), BIT(irq_bit));

	if (rc) {
		pr_err("Failed to read/write mask IRQ:%d rc=%d\n", pmirq, rc);
		goto fail;
	}

	rc = regmap_update_bits(chip->regmap,
				base + PM8821_IRQ_CLEAR_OFFSET + block,
				BIT(irq_bit), BIT(irq_bit));

	if (rc) {
		pr_err("Failed to read/write IT_CLEAR IRQ:%d rc=%d\n",
								pmirq, rc);
	}

fail:
	spin_unlock(&chip->pm_irq_lock);
}

static void pm8821_irq_unmask(struct irq_data *d)
{
	struct pm_irq_chip *chip = irq_data_get_irq_chip_data(d);
	unsigned int base, pmirq = irqd_to_hwirq(d);
	int irq_bit, rc;
	u8 block, master;

	block = pmirq / 8;
	master = block / PM8821_BLOCKS_PER_MASTER;
	irq_bit = pmirq % 8;
	block %= PM8821_BLOCKS_PER_MASTER;

	if (!master)
		base = chip->irq_reg_base + SSBI_REG_ADDR_IRQ_MASTER0;
	else
		base = chip->irq_reg_base + SSBI_REG_ADDR_IRQ_MASTER1;

	spin_lock(&chip->pm_irq_lock);

	rc = regmap_update_bits(chip->regmap,
				base + PM8821_IRQ_MASK_REG_OFFSET + block,
				BIT(irq_bit), ~BIT(irq_bit));

	if (rc)
		pr_err("Failed to read/write unmask IRQ:%d rc=%d\n", pmirq, rc);

	spin_unlock(&chip->pm_irq_lock);
}

static int pm8821_irq_set_type(struct irq_data *d, unsigned int flow_type)
{

	/*
	 * PM8821 IRQ controller does not have explicit software support for
	 * IRQ flow type.
	 */
	return 0;
}

static int pm8821_irq_get_irqchip_state(struct irq_data *d,
					enum irqchip_irq_state which,
					bool *state)
{
	struct pm_irq_chip *chip = irq_data_get_irq_chip_data(d);
	int pmirq, rc;
	u8 block, irq_bit, master;
	unsigned int bits;
	unsigned int base;
	unsigned long flags;

	pmirq = irqd_to_hwirq(d);

	block = pmirq / 8;
	master = block / PM8821_BLOCKS_PER_MASTER;
	irq_bit = pmirq % 8;
	block %= PM8821_BLOCKS_PER_MASTER;

	if (!master)
		base = chip->irq_reg_base + SSBI_REG_ADDR_IRQ_MASTER0;
	else
		base = chip->irq_reg_base + SSBI_REG_ADDR_IRQ_MASTER1;

	spin_lock_irqsave(&chip->pm_irq_lock, flags);

	rc = regmap_read(chip->regmap,
		base + PM8821_IRQ_RT_STATUS_OFFSET + block, &bits);
	if (rc) {
		pr_err("Failed Reading Status rc=%d\n", rc);
		goto bail_out;
	}

	*state = !!(bits & BIT(irq_bit));

bail_out:
	spin_unlock_irqrestore(&chip->pm_irq_lock, flags);

	return rc;
}

static struct irq_chip pm8821_irq_chip = {
	.name		= "pm8821",
	.irq_mask_ack	= pm8821_irq_mask_ack,
	.irq_unmask	= pm8821_irq_unmask,
	.irq_set_type	= pm8821_irq_set_type,
	.irq_get_irqchip_state = pm8821_irq_get_irqchip_state,
	.flags		= IRQCHIP_MASK_ON_SUSPEND | IRQCHIP_SKIP_SET_WAKE,
};

static int pm8821_irq_domain_map(struct irq_domain *d, unsigned int irq,
				   irq_hw_number_t hwirq)
{
	struct pm_irq_chip *chip = d->host_data;

	irq_set_chip_and_handler(irq, &pm8821_irq_chip, handle_level_irq);
	irq_set_chip_data(irq, chip);
	irq_set_noprobe(irq);

	return 0;
}

static const struct irq_domain_ops pm8821_irq_domain_ops = {
	.xlate = irq_domain_xlate_twocell,
	.map = pm8821_irq_domain_map,
};

static const struct regmap_config ssbi_regmap_config = {
	.reg_bits = 16,
	.val_bits = 8,
	.max_register = 0x3ff,
	.fast_io = true,
	.reg_read = ssbi_reg_read,
	.reg_write = ssbi_reg_write
};

static const struct pm8xxx_data pm8xxx_data = {
	.num_irqs = PM8921_NR_IRQS,
	.irq_reg_base = SSBI_REG_ADDR_IRQ_BASE,
	.irq_domain_ops = &pm8xxx_irq_domain_ops,
	.irq_handler = pm8xxx_irq_handler,
};

static const struct pm8xxx_data pm8821_data = {
	.num_irqs = PM8821_NR_IRQS,
	.irq_reg_base = SSBI_PM8821_REG_ADDR_IRQ_BASE,
	.irq_domain_ops = &pm8821_irq_domain_ops,
	.irq_handler = pm8821_irq_handler,
};

static const struct of_device_id pm8921_id_table[] = {
	{ .compatible = "qcom,pm8018", .data = &pm8xxx_data},
	{ .compatible = "qcom,pm8058", .data = &pm8xxx_data},
	{ .compatible = "qcom,pm8821", .data = &pm8821_data},
	{ .compatible = "qcom,pm8921", .data = &pm8xxx_data},
	{ }
};
MODULE_DEVICE_TABLE(of, pm8921_id_table);

static int pm8921_probe(struct platform_device *pdev)
{
	struct regmap *regmap;
	const struct pm8xxx_data *data;
	int irq, rc;
	unsigned int val;
	u32 rev;
	struct pm_irq_chip *chip;

	data = of_match_node(pm8921_id_table, pdev->dev.of_node)->data;
	if (!data) {
		dev_err(&pdev->dev, "No matching driver data found\n");
		return -EINVAL;
	}

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;

	regmap = devm_regmap_init(&pdev->dev, NULL, pdev->dev.parent,
				  &ssbi_regmap_config);
	if (IS_ERR(regmap))
		return PTR_ERR(regmap);

	/* Read PMIC chip revision */
	rc = regmap_read(regmap, REG_HWREV, &val);
	if (rc) {
		pr_err("Failed to read hw rev reg %d:rc=%d\n", REG_HWREV, rc);
		return rc;
	}
	pr_info("PMIC revision 1: %02X\n", val);
	rev = val;

	/* Read PMIC chip revision 2 */
	rc = regmap_read(regmap, REG_HWREV_2, &val);
	if (rc) {
		pr_err("Failed to read hw rev 2 reg %d:rc=%d\n",
			REG_HWREV_2, rc);
		return rc;
	}
	pr_info("PMIC revision 2: %02X\n", val);
	rev |= val << BITS_PER_BYTE;

	chip = devm_kzalloc(&pdev->dev, sizeof(*chip) +
			    sizeof(chip->config[0]) * data->num_irqs,
			    GFP_KERNEL);
	if (!chip)
		return -ENOMEM;

	platform_set_drvdata(pdev, chip);
	chip->regmap = regmap;
	chip->num_irqs = data->num_irqs;
	chip->irq_reg_base = data->irq_reg_base;
	chip->num_blocks = DIV_ROUND_UP(chip->num_irqs, 8);
	chip->num_masters = DIV_ROUND_UP(chip->num_blocks, 8);
	spin_lock_init(&chip->pm_irq_lock);

	chip->irqdomain = irq_domain_add_linear(pdev->dev.of_node,
						data->num_irqs,
						data->irq_domain_ops,
						chip);
	if (!chip->irqdomain)
		return -ENODEV;

	irq_set_chained_handler_and_data(irq, data->irq_handler, chip);
	irq_set_irq_wake(irq, 1);

	rc = of_platform_populate(pdev->dev.of_node, NULL, NULL, &pdev->dev);
	if (rc) {
		irq_set_chained_handler_and_data(irq, NULL, NULL);
		irq_domain_remove(chip->irqdomain);
	}

	return rc;
}

static int pm8921_remove_child(struct device *dev, void *unused)
{
	platform_device_unregister(to_platform_device(dev));
	return 0;
}

static int pm8921_remove(struct platform_device *pdev)
{
	int irq = platform_get_irq(pdev, 0);
	struct pm_irq_chip *chip = platform_get_drvdata(pdev);

	device_for_each_child(&pdev->dev, NULL, pm8921_remove_child);
	irq_set_chained_handler_and_data(irq, NULL, NULL);
	irq_domain_remove(chip->irqdomain);

	return 0;
}

static struct platform_driver pm8921_driver = {
	.probe		= pm8921_probe,
	.remove		= pm8921_remove,
	.driver		= {
		.name	= "pm8921-core",
		.of_match_table = pm8921_id_table,
	},
};

static int __init pm8921_init(void)
{
	return platform_driver_register(&pm8921_driver);
}
subsys_initcall(pm8921_init);

static void __exit pm8921_exit(void)
{
	platform_driver_unregister(&pm8921_driver);
}
module_exit(pm8921_exit);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("PMIC 8921 core driver");
MODULE_VERSION("1.0");
MODULE_ALIAS("platform:pm8921-core");
