/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * Authors:
 * (C) 2015 Pengutronix, Alexander Aring <aar@pengutronix.de>
 * Eric Anholt <eric@anholt.net>
 */

#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <dt-bindings/arm/raspberrypi-power.h>
#include <soc/bcm2835/raspberrypi-firmware.h>

#define RPI_POWER_DOMAIN(_domain, _name)			\
	[_domain] = {						\
		.domain = _domain,				\
		.enabled = true,				\
		.base = {					\
			.name = _name,				\
			.power_off = rpi_domain_off,		\
			.power_on = rpi_domain_on,		\
		},						\
	}

struct rpi_power_domain {
	u32 domain;
	bool enabled;
	struct generic_pm_domain base;
};

struct rpi_power_domain_packet {
	u32 domain;
	u32 on;
} __packet;

static struct rpi_firmware *fw;
static struct genpd_onecell_data rpi_genpd_xlate;

/*
 * Asks the firmware to enable or disable power on a specific power
 * domain.
 */
static int rpi_firmware_set_power(u32 domain, bool on)
{
	struct rpi_power_domain_packet packet;

	packet.domain = domain;
	packet.on = on;
	return rpi_firmware_property(fw, RPI_FIRMWARE_SET_POWER_STATE, &packet,
				     sizeof(packet));
}

/* Asks the firmware to if power is on for a specific power domain. */
static int rpi_firmware_power_is_on(u32 domain)
{
	struct rpi_power_domain_packet packet;
	int ret;

	packet.domain = domain;
	ret = rpi_firmware_property(fw, RPI_FIRMWARE_GET_POWER_STATE, &packet,
				    sizeof(packet));
	if (ret < 0)
		return ret;

	return packet.on & BIT(0);
}

static int rpi_domain_off(struct generic_pm_domain *domain)
{
	struct rpi_power_domain *rpi_domain =
		container_of(domain, struct rpi_power_domain, base);

	return rpi_firmware_set_power(rpi_domain->domain, false);
}

static int rpi_domain_on(struct generic_pm_domain *domain)
{
	struct rpi_power_domain *rpi_domain =
		container_of(domain, struct rpi_power_domain, base);

	return rpi_firmware_set_power(rpi_domain->domain, true);
}

static struct rpi_power_domain rpi_power_domains[] = {
	RPI_POWER_DOMAIN(RPI_POWER_DOMAIN_USB, "USB"),
};

static int rpi_power_probe(struct platform_device *pdev)
{
	struct device_node *fw_np;
	struct device *dev = &pdev->dev;
	struct generic_pm_domain **power_domains;
	int i, ret, num_domains = ARRAY_SIZE(rpi_power_domains);

	fw_np = of_parse_phandle(pdev->dev.of_node, "firmware", 0);
	if (!fw_np) {
		dev_err(&pdev->dev, "no firmware node\n");
		return -ENODEV;
	}

	fw = rpi_firmware_get(fw_np);
	if (!fw)
		return -EPROBE_DEFER;

	power_domains = devm_kzalloc(dev, sizeof(*power_domains) * num_domains,
				     GFP_KERNEL);
	if (!power_domains)
		return -ENOMEM;

	rpi_genpd_xlate.domains = power_domains;
	rpi_genpd_xlate.num_domains = num_domains;

	for (i = 0; i < num_domains; i++) {
		bool is_off;

		if (!rpi_power_domains[i].enabled)
			continue;

		/* get the initial state */
		ret = rpi_firmware_power_is_on(rpi_power_domains[i].domain);
		if (ret < 0)
			goto uninit_pm;

		/* pm_genpd_init needs is_off, invert the logic here */
		is_off = !ret;
		pm_genpd_init(&rpi_power_domains[i].base, NULL, is_off);
		/* let power_domains array know about the registered pm */
		power_domains[i] = &rpi_power_domains[i].base;
	}

	ret = of_genpd_add_provider_onecell(dev->of_node, &rpi_genpd_xlate);
	if (ret < 0)
		goto uninit_pm;

	return 0;

uninit_pm:
	for (i = 0; i < num_domains; i++)
		pm_genpd_uninit(power_domains[i]);

	return ret;
}

static int rpi_power_remove(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	int i;

	for (i = 0; i < rpi_genpd_xlate.num_domains; i++)
		pm_genpd_uninit(rpi_genpd_xlate.domains[i]);

	of_genpd_del_provider(dev->of_node);

	return 0;
}

static const struct of_device_id rpi_power_of_match[] = {
	{ .compatible = "raspberrypi,bcm2835-power", },
	{},
};
MODULE_DEVICE_TABLE(of, rpi_power_of_match);

static struct platform_driver rpi_power_driver = {
	.driver = {
		.name = "raspberrypi-power",
		.of_match_table = rpi_power_of_match,
	},
	.probe		= rpi_power_probe,
	.remove		= rpi_power_remove,
};
module_platform_driver(rpi_power_driver);

MODULE_AUTHOR("Alexander Aring <aar@pengutronix.de>");
MODULE_AUTHOR("Eric Anholt <eric@anholt.net>");
MODULE_DESCRIPTION("Raspberry Pi power domain driver");
MODULE_LICENSE("GPL v2");
