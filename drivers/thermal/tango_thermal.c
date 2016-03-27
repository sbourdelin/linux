#include <linux/io.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/thermal.h>
#include <linux/platform_device.h>

#define TEMPSI_CMD	0
#define TEMPSI_RES	4
#define TEMPSI_CFG	8

#define CMD_OFF		0
#define CMD_ON		1
#define CMD_READ	2

#define IDX_MIN		0
#define IDX_MAX		40
#define CYCLE_COUNT	5000 /* Time to wait before sampling the result */

struct tango_thermal_priv {
	struct thermal_zone_device *zone;
	void __iomem *base;
	int thresh_idx;
};

static bool temp_above_thresh(void __iomem *base, int thresh_idx)
{
	writel(thresh_idx << 8 | CMD_READ, base + TEMPSI_CMD);
	usleep_range(100, 200);
	return readl(base + TEMPSI_RES);
}

static int tango_get_temp(void *arg, int *res)
{
	struct tango_thermal_priv *priv = arg;
	void __iomem *base = priv->base;
	int idx = priv->thresh_idx;

	if (temp_above_thresh(base, idx)) {
		/* Upward linear search, increment thresh */
		while (idx < IDX_MAX && temp_above_thresh(base, ++idx))
			cpu_relax();
		idx = idx - 1; /* always return lower bound */
	} else {
		/* Downward linear search, decrement thresh */
		while (idx > IDX_MIN && !temp_above_thresh(base, --idx))
			cpu_relax();
	}

	*res = idx * 4500 - 38000; /* millidegrees Celsius */
	priv->thresh_idx = idx;
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

	writel(CMD_ON, priv->base + TEMPSI_CMD);
	writel(CYCLE_COUNT, priv->base + TEMPSI_CFG);

	priv->zone = thermal_zone_of_sensor_register(dev, 0, priv, &ops);
	if (IS_ERR(priv->zone))
		return PTR_ERR(priv->zone);

	priv->thresh_idx = 15; /* arbitrary starting point */
	platform_set_drvdata(pdev, priv);
	return 0;
}

static int tango_thermal_remove(struct platform_device *pdev)
{
	struct tango_thermal_priv *priv = platform_get_drvdata(pdev);

	thermal_zone_of_sensor_unregister(&pdev->dev, priv->zone);
	writel(CMD_OFF, priv->base + TEMPSI_CMD);
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
