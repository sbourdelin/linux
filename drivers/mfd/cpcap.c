/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/device.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/regmap.h>
#include <linux/sysfs.h>

#include <linux/mfd/cpcap.h>
#include <linux/spi/spi.h>

#define CPCAP_NR_IRQ_BANKS	6
#define CPCAP_NR_IRQ_DOMAINS	3

struct cpcap_device {
	struct spi_device *spi;
	struct device *dev;
	u16 vendor;
	u16 revision;
	const struct cpcap_platform_data *conf;
	struct regmap_irq *irqs;
	struct regmap_irq_chip_data *irqdata[CPCAP_NR_IRQ_DOMAINS];
	const struct regmap_config *regmap_conf;
	struct regmap *regmap;
};

static int cpcap_check_revision(struct cpcap_device *cpcap)
{
	unsigned int val;
	int error;

	error = regmap_read(cpcap->regmap, CPCAP_REG_VERSC1, &val);
	if (error)
		return error;

	cpcap->vendor = (val >> 6) & 0x0007;
	cpcap->revision = ((val >> 3) & 0x0007) | ((val << 3) & 0x0038);
	dev_info(cpcap->dev, "CPCAP vendor: %s rev: %i.%i (%x)\n",
		 cpcap->vendor ? "TI" : "ST", (cpcap->revision >> 4) + 1,
		 cpcap->revision & 0xf, cpcap->revision);

	if (cpcap->revision < CPCAP_REVISION_2_1) {
		dev_info(cpcap->dev,
			 "Please add old CPCAP revision support as needed\n");
		return -ENODEV;
	}

	return 0;
}

/*
 * First domain is the two private macro interrupt banks, the third
 * domain is for banks 1 - 4 and is available for drivers to use.
 */
static struct regmap_irq_chip cpcap_irq_chip[CPCAP_NR_IRQ_DOMAINS] = {
	{
		.name = "cpcap-m2",
		.num_regs = 1,
		.status_base = CPCAP_REG_MI1,
		.ack_base = CPCAP_REG_MI1,
		.mask_base = CPCAP_REG_MIM1,
		.use_ack = true,
	},
	{
		.name = "cpcap-m2",
		.num_regs = 1,
		.status_base = CPCAP_REG_MI2,
		.ack_base = CPCAP_REG_MI2,
		.mask_base = CPCAP_REG_MIM2,
		.use_ack = true,
	},
	{
		.name = "cpcap1-4",
		.num_regs = 4,
		.status_base = CPCAP_REG_INT1,
		.ack_base = CPCAP_REG_INT1,
		.mask_base = CPCAP_REG_INTM1,
		.type_base = CPCAP_REG_INTS1,
		.use_ack = true,
	},
};

static int cpcap_init_irq_bank(struct cpcap_device *cpcap, int irq_domain,
			       int irq_start, int nr_irqs)
{
	struct regmap_irq_chip *domain = &cpcap_irq_chip[irq_domain];
	int i, error;

	for (i = irq_start; i < irq_start + nr_irqs; i++) {
		struct regmap_irq *cpcap_irq = &cpcap->irqs[i];

		cpcap_irq->reg_offset =
			((i - irq_start) / cpcap->regmap_conf->val_bits) *
			cpcap->regmap_conf->reg_stride;
		cpcap_irq->mask = BIT(i % cpcap->regmap_conf->val_bits);
	}
	domain->irqs = &cpcap->irqs[irq_start];
	domain->num_irqs = nr_irqs;
	domain->irq_drv_data = cpcap;

	error = devm_regmap_add_irq_chip(cpcap->dev, cpcap->regmap,
					 cpcap->spi->irq,
					 IRQF_TRIGGER_RISING |
					 IRQF_SHARED, -1,
					 domain, &cpcap->irqdata[irq_domain]);
	if (error) {
		dev_err(cpcap->dev, "could not add irq domain %i: %i\n",
			irq_domain, error);
		return error;
	}

	return 0;
}

static int cpcap_init_irq(struct cpcap_device *cpcap)
{
	int error;

	cpcap->irqs = devm_kzalloc(cpcap->dev,
				   sizeof(*cpcap->irqs) *
				   CPCAP_NR_IRQ_BANKS *
				   cpcap->regmap_conf->val_bits,
				   GFP_KERNEL);
	if (!cpcap->irqs)
		return -ENOMEM;

	error = cpcap_init_irq_bank(cpcap, 0, 0, 16);
	if (error)
		return error;

	error = cpcap_init_irq_bank(cpcap, 1, 16, 16);
	if (error)
		return error;

	error = cpcap_init_irq_bank(cpcap, 2, 32, 64);
	if (error)
		return error;

	enable_irq_wake(cpcap->spi->irq);

	return 0;
}

static const struct of_device_id cpcap_of_match[] = {
	{
		.compatible = "motorola,cpcap",
	},
	{
		.compatible = "st,6556002",
	},
	{},
};
MODULE_DEVICE_TABLE(of, cpcap_of_match);

static const struct regmap_config cpcap_regmap_config = {
	.reg_bits = 16,
	.reg_stride = 4,
	.pad_bits = 0,
	.val_bits = 16,
	.write_flag_mask = 0x8000,
	.max_register = CPCAP_REG_ST_TEST2,
	.cache_type = REGCACHE_NONE,
	.reg_format_endian = REGMAP_ENDIAN_LITTLE,
	.val_format_endian = REGMAP_ENDIAN_LITTLE,
};

static const struct of_device_id cpcap_dt_match_table[] = {
	{ .compatible = "simple-bus", },
	{ },
};

static int cpcap_probe(struct spi_device *spi)
{
	const struct of_device_id *match;
	int error = -EINVAL;
	struct cpcap_device *cpcap;

	match = of_match_device(of_match_ptr(cpcap_of_match), &spi->dev);
	if (!match)
		return -ENODEV;

	cpcap = devm_kzalloc(&spi->dev, sizeof(*cpcap), GFP_KERNEL);
	if (!cpcap)
		return -ENOMEM;

	cpcap->conf = match->data;
	cpcap->spi = spi;
	cpcap->dev = &spi->dev;
	spi_set_drvdata(spi, cpcap);

	spi->bits_per_word = 16;
	spi->mode = SPI_MODE_0 | SPI_CS_HIGH;
	error = spi_setup(spi);
	if (error < 0)
		return error;

	cpcap->regmap_conf = &cpcap_regmap_config;
	cpcap->regmap = devm_regmap_init_spi(spi, &cpcap_regmap_config);
	if (IS_ERR(cpcap->regmap)) {
		error = PTR_ERR(cpcap->regmap);
		dev_err(cpcap->dev, "Failed to initialize regmap: %d\n",
			error);

		return error;
	}

	error = cpcap_check_revision(cpcap);
	if (error)
		return error;

	error = cpcap_init_irq(cpcap);
	if (error)
		return error;

	error = of_platform_populate(spi->dev.of_node,
				     cpcap_dt_match_table,
				     NULL, cpcap->dev);
	if (error)
		return error;

	return 0;
}

static int cpcap_remove(struct spi_device *pdev)
{
	struct cpcap_device *cpcap = spi_get_drvdata(pdev);

	of_platform_depopulate(cpcap->dev);

	return 0;
}

static struct spi_driver cpcap_driver = {
	.driver = {
		.name = "cpcap-core",
		.owner = THIS_MODULE,
		.of_match_table = cpcap_of_match,
	},
	.probe = cpcap_probe,
	.remove = cpcap_remove,
};
module_spi_driver(cpcap_driver);

MODULE_ALIAS("platform:cpcap");
MODULE_DESCRIPTION("CPCAP driver");
MODULE_AUTHOR("Tony Lindgren <tony@atomide.com>");
MODULE_LICENSE("GPL v2");
