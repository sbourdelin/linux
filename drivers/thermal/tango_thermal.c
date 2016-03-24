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

#define IDX_MIN		12
#define IDX_MAX		40
#define CYCLE_COUNT	50 /* Time to wait before sampling the result */

static const u8 temperature[] = {
	15, 19, 23, 28, 33, 37, 41, 46, 51, 55, 60, 64, 69, 74, 79, 83,
	88, 93, 97, 101, 106, 110, 115, 120, 124, 129, 133, 137,
};

struct tango_thermal_priv {
	struct thermal_zone_device *zone;
	void __iomem *base;
	int thresh_idx;
};

#define sensor_idle(base) (readl(base + TEMPSI_CMD) & BIT(7))

static bool temp_above_thresh(void __iomem *base, int thresh_idx)
{
	writel_relaxed(thresh_idx << 8 | CMD_READ, base + TEMPSI_CMD);

	while (!sensor_idle(base))
		cpu_relax();

	return readl_relaxed(base + TEMPSI_RES);
}

static int tango_get_temp(void *arg, int *res)
{
	struct tango_thermal_priv *priv = arg;
	void __iomem *base = priv->base;
	int idx = priv->thresh_idx;

	if (temp_above_thresh(base, idx)) {
		/* Downward linear search */
		while (idx > IDX_MIN && temp_above_thresh(base, --idx))
			cpu_relax();
	} else {
		/* Upward linear search */
		while (idx < IDX_MAX && !temp_above_thresh(base, ++idx))
			cpu_relax();
		idx = idx - 1;
	}

	priv->thresh_idx = idx;
	*res = temperature[idx - IDX_MIN] * 1000;
	return 0;
}

static const struct thermal_zone_of_device_ops ops = {
	.get_temp	= tango_get_temp,
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
	writel_relaxed(CYCLE_COUNT, priv->base + TEMPSI_CFG);

	priv->zone = thermal_zone_of_sensor_register(dev, 0, priv, &ops);
	if (IS_ERR(priv->zone))
		return PTR_ERR(priv->zone);

	platform_set_drvdata(pdev, priv);
	return 0;
}

static int tango_thermal_remove(struct platform_device *pdev)
{
	struct tango_thermal_priv *priv = platform_get_drvdata(pdev);

	thermal_zone_of_sensor_unregister(&pdev->dev, priv->zone);
	writel_relaxed(CMD_OFF, priv->base + TEMPSI_CMD);
	return 0;
}

static const struct of_device_id tango_sensor_ids[] = {
	{
		.compatible = "sigma,smp8758-thermal",
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
