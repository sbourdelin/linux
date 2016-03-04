#include <linux/io.h>
#include <linux/module.h>
#include <linux/thermal.h>
#include <linux/platform_device.h>

#define TEMPSI_CMD	0
#define TEMPSI_RES	4
#define TEMPSI_CFG	8

#define CMD_OFF		0
#define CMD_ON		1
#define CMD_READ	2

#define INDEX_OFFSET	18

#define sensor_idle(x)		(readl_relaxed(x + TEMPSI_CMD) & BIT(7))
#define temp_above_thresh(x)	(readl_relaxed(x + TEMPSI_RES))

static const u8 temperature[] = {
	 37,  41,  46,  51,  55,  60,  64,  69,
	 74,  79,  83,  88,  93,  97, 101, 106,
	110, 115, 120, 124, 129, 133, 137, 142,
};

static int tango_get_temp(void *arg, int *res)
{
	int i;
	void __iomem *base = arg;

	for (i = INDEX_OFFSET; i < 41; ++i)
	{
		writel_relaxed(i << 8 | CMD_READ, base + TEMPSI_CMD);

		while (!sensor_idle(base))
			cpu_relax();

		if (!temp_above_thresh(base))
			break;
	}

	writel_relaxed(INDEX_OFFSET << 8 | CMD_READ, base + TEMPSI_CMD);
	*res = temperature[i - INDEX_OFFSET] * 1000;
	return 0;
}

static const struct thermal_zone_of_device_ops ops = {
	.get_temp	= tango_get_temp,
};

struct tango_thermal_priv {
	struct thermal_zone_device *zone;
	void __iomem *base;
};

static int tango_thermal_probe(struct platform_device *pdev)
{
	struct resource *res;
	struct tango_thermal_priv *priv;
	struct device *dev = &pdev->dev;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	priv->base = devm_ioremap_resource(dev, res);
	if (IS_ERR(priv->base))
		return PTR_ERR(priv->base);

	writel_relaxed(CMD_ON, priv->base + TEMPSI_CMD);
	writel_relaxed(    50, priv->base + TEMPSI_CFG);

	priv->zone = thermal_zone_of_sensor_register(dev, 0, priv->base, &ops);
	if (IS_ERR(priv->zone))
		return PTR_ERR(priv->zone);

	platform_set_drvdata(pdev, priv);
	return 0;
}

static int tango_thermal_remove(struct platform_device *pdev)
{
	struct tango_thermal_priv *priv = platform_get_drvdata(pdev);

	thermal_zone_of_sensor_unregister(&pdev->dev, priv->zone);

	writel_relaxed(      0, priv->base + TEMPSI_CFG);
	writel_relaxed(CMD_OFF, priv->base + TEMPSI_CMD);

	return 0;
}

static const struct of_device_id tango_sensor_ids[] = {
	{
		.compatible = "sigma,smp8758-sensor",
	},
	{ /* sentinel */ }
};

static struct platform_driver tango_thermal_driver = {
	.probe	= tango_thermal_probe,
	.remove	= tango_thermal_remove,
	.driver	= {
		.name		= "tango-thermal",
		.of_match_table	= tango_sensor_ids,
	},
};

module_platform_driver(tango_thermal_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Sigma Designs");
MODULE_DESCRIPTION("Tango temperature sensor");
