// SPDX-License-Identifier: GPL-2.0
//
// UP Board platform controller driver
//
// Copyright (c) 2018, Emutex Ltd.
//
// Author: Javier Arteaga <javier@emutex.com>
//

#include <linux/acpi.h>
#include <linux/gpio/consumer.h>
#include <linux/kernel.h>
#include <linux/mfd/core.h>
#include <linux/mfd/upboard.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

#define UPBOARD_FW_BUILD_SHIFT 12
#define UPBOARD_FW_MAJOR_SHIFT 8
#define UPBOARD_FW_MINOR_SHIFT 4
#define UPBOARD_FW_PATCH_SHIFT 0

#define UPBOARD_FW_BUILD(id) (((id) >> UPBOARD_FW_BUILD_SHIFT) & 0x0f)
#define UPBOARD_FW_MAJOR(id) (((id) >> UPBOARD_FW_MAJOR_SHIFT) & 0x0f)
#define UPBOARD_FW_MINOR(id) (((id) >> UPBOARD_FW_MINOR_SHIFT) & 0x0f)
#define UPBOARD_FW_PATCH(id) (((id) >> UPBOARD_FW_PATCH_SHIFT) & 0x0f)

#define AAEON_MANUFACTURER_ID 0x01
#define SUPPORTED_FW_MAJOR 0x0

/* MSb of 8-bit address is an R/W flag */
#define UPBOARD_ADDRESS_SIZE  8
#define UPBOARD_READ_FLAG     BIT(7)

enum upboard_id {
	UPBOARD_ID_UP2 = 1,
};

struct upboard_ddata {
	struct gpio_desc *clear_gpio;
	struct gpio_desc *strobe_gpio;
	struct gpio_desc *datain_gpio;
	struct gpio_desc *dataout_gpio;
	const struct regmap_config *regmapconf;
	const struct mfd_cell *cells;
	size_t ncells;
};

/*
 * UP boards include a platform controller with a proprietary GPIO-bitbanged
 * control interface to access its configuration registers.
 *
 * The following macros and functions implement the read/write handlers for
 * that interface, to provide a regmap-based abstraction for the controller.
 */

#define set_clear(u, x) gpiod_set_value((u)->clear_gpio, (x))
#define set_strobe(u, x) gpiod_set_value((u)->strobe_gpio, (x))
#define set_datain(u, x) gpiod_set_value((u)->datain_gpio, (x))
#define get_dataout(u) gpiod_get_value((u)->dataout_gpio)

static void __reg_io_start(const struct upboard_ddata * const ddata)
{
	/*
	 * CLEAR signal must be pulsed low before any register access.
	 * This resets internal counters in the controller and marks
	 * the start of a new register access.
	 */
	set_clear(ddata, 0);
	set_clear(ddata, 1);
}

static void __reg_io_end(const struct upboard_ddata * const ddata)
{
	/*
	 * STROBE signal must be cycled again to mark the end of a register
	 * access.  Partial register accesses are discarded harmlessly
	 * by the controller if this final strobe cycle is not sent
	 */
	set_strobe(ddata, 0);
	set_strobe(ddata, 1);
}

static void __reg_io_write(const struct upboard_ddata * const ddata,
			   unsigned int size, unsigned int val)
{
	/*
	 * DATAIN is latched on each rising edge of the STROBE signal.
	 * Data (register address or value) is sent MSb first.
	 */
	while (size--) {
		set_strobe(ddata, 0);
		set_datain(ddata, (val >> size) & 0x1);
		set_strobe(ddata, 1);
	}
}

static void __reg_io_read(const struct upboard_ddata * const ddata,
			  unsigned int size, unsigned int *val)
{
	/*
	 * DATAOUT is latched on on each rising edge of the STROBE signal.
	 * Data (register value) is received MSb first.
	 */
	*val = 0;
	while (size--) {
		set_strobe(ddata, 0);
		set_strobe(ddata, 1);
		*val |= get_dataout(ddata) << size;
	}
}

static int upboard_reg_read(void *context, unsigned int reg, unsigned int *val)
{
	const struct upboard_ddata * const ddata = context;

	__reg_io_start(ddata);
	__reg_io_write(ddata, UPBOARD_ADDRESS_SIZE, reg | UPBOARD_READ_FLAG);
	__reg_io_read(ddata, UPBOARD_REGISTER_SIZE, val);
	__reg_io_end(ddata);

	return 0;
}

static int upboard_reg_write(void *context, unsigned int reg, unsigned int val)
{
	const struct upboard_ddata * const ddata = context;

	__reg_io_start(ddata);
	__reg_io_write(ddata, UPBOARD_ADDRESS_SIZE, reg);
	__reg_io_write(ddata, UPBOARD_REGISTER_SIZE, val);
	__reg_io_end(ddata);

	return 0;
}

/* UP Squared */

static const struct regmap_range upboard_up2_readable_ranges[] = {
	regmap_reg_range(UPBOARD_REG_PLATFORM_ID, UPBOARD_REG_FIRMWARE_ID),
	regmap_reg_range(UPBOARD_REG_FUNC_EN0, UPBOARD_REG_FUNC_EN1),
	regmap_reg_range(UPBOARD_REG_GPIO_EN0, UPBOARD_REG_GPIO_EN2),
	regmap_reg_range(UPBOARD_REG_GPIO_DIR0, UPBOARD_REG_GPIO_DIR2),
};

static const struct regmap_range upboard_up2_writable_ranges[] = {
	regmap_reg_range(UPBOARD_REG_FUNC_EN0, UPBOARD_REG_FUNC_EN1),
	regmap_reg_range(UPBOARD_REG_GPIO_EN0, UPBOARD_REG_GPIO_EN2),
	regmap_reg_range(UPBOARD_REG_GPIO_DIR0, UPBOARD_REG_GPIO_DIR2),
};

static const struct regmap_access_table upboard_up2_readable_table = {
	.yes_ranges = upboard_up2_readable_ranges,
	.n_yes_ranges = ARRAY_SIZE(upboard_up2_readable_ranges),
};

static const struct regmap_access_table upboard_up2_writable_table = {
	.yes_ranges = upboard_up2_writable_ranges,
	.n_yes_ranges = ARRAY_SIZE(upboard_up2_writable_ranges),
};

static const struct regmap_config upboard_up2_regmap_config = {
	.reg_bits = UPBOARD_ADDRESS_SIZE,
	.val_bits = UPBOARD_REGISTER_SIZE,
	.max_register = UPBOARD_REG_MAX,
	.reg_read = upboard_reg_read,
	.reg_write = upboard_reg_write,
	.fast_io = false,
	.cache_type = REGCACHE_RBTREE,
	.rd_table = &upboard_up2_readable_table,
	.wr_table = &upboard_up2_writable_table,
};

static const struct mfd_cell upboard_up2_mfd_cells[] = {
	{
		.name = "upboard-led",
		.id = 0,
	},
	{
		.name = "upboard-led",
		.id = 1,
	},
	{
		.name = "upboard-led",
		.id = 2,
	},
	{
		.name = "upboard-led",
		.id = 3,
	},
	{
		.name = "upboard-pinctrl"
	},
};

static int upboard_init_gpio(struct device *dev)
{
	struct upboard_ddata *ddata = dev_get_drvdata(dev);
	struct gpio_desc *enable_gpio;

	ddata->clear_gpio = devm_gpiod_get(dev, "clear", GPIOD_OUT_LOW);
	if (IS_ERR(ddata->clear_gpio))
		return PTR_ERR(ddata->clear_gpio);

	ddata->strobe_gpio = devm_gpiod_get(dev, "strobe", GPIOD_OUT_LOW);
	if (IS_ERR(ddata->strobe_gpio))
		return PTR_ERR(ddata->strobe_gpio);

	ddata->datain_gpio = devm_gpiod_get(dev, "datain", GPIOD_OUT_LOW);
	if (IS_ERR(ddata->datain_gpio))
		return PTR_ERR(ddata->datain_gpio);

	ddata->dataout_gpio = devm_gpiod_get(dev, "dataout", GPIOD_IN);
	if (IS_ERR(ddata->dataout_gpio))
		return PTR_ERR(ddata->dataout_gpio);

	/* External I/O signals are gated by ENABLE - ensure this is high */
	enable_gpio = devm_gpiod_get(dev, "enable", GPIOD_OUT_HIGH);
	if (IS_ERR(enable_gpio))
		return PTR_ERR(enable_gpio);

	return 0;
}

static int upboard_check_supported(struct device *dev, struct regmap *regmap)
{
	uint8_t manufacturer_id, build, major, minor, patch;
	unsigned int platform_id, firmware_id;
	int ret;

	ret = regmap_read(regmap, UPBOARD_REG_PLATFORM_ID, &platform_id);
	if (ret)
		return ret;

	manufacturer_id = platform_id;
	if (manufacturer_id != AAEON_MANUFACTURER_ID) {
		dev_err(dev,
			"unsupported FPGA firmware from manufacturer 0x%02x",
			manufacturer_id);
		return -ENODEV;
	}

	ret = regmap_read(regmap, UPBOARD_REG_FIRMWARE_ID, &firmware_id);
	if (ret)
		return ret;

	build = UPBOARD_FW_BUILD(firmware_id);
	major = UPBOARD_FW_MAJOR(firmware_id);
	minor = UPBOARD_FW_MINOR(firmware_id);
	patch = UPBOARD_FW_PATCH(firmware_id);
	if (major != SUPPORTED_FW_MAJOR) {
		dev_err(dev, "unsupported FPGA firmware v%u.%u.%u.%u",
			 major, minor, patch, build);
		return -ENODEV;
	}

	dev_dbg(dev, "supported FPGA firmware v%u.%u.%u.%u",
		major, minor, patch, build);
	return 0;
}

static const struct acpi_device_id upboard_acpi_match[] = {
	{ "AANT0F01", (kernel_ulong_t)UPBOARD_ID_UP2 },
	{ }
};
MODULE_DEVICE_TABLE(acpi, upboard_acpi_match);

static int upboard_match_device(struct device *dev)
{
	struct upboard_ddata *ddata = dev_get_drvdata(dev);
	enum upboard_id id = (kernel_ulong_t)device_get_match_data(dev);

	switch (id) {
	case UPBOARD_ID_UP2:
		ddata->regmapconf = &upboard_up2_regmap_config;
		ddata->cells = upboard_up2_mfd_cells;
		ddata->ncells = ARRAY_SIZE(upboard_up2_mfd_cells);
		break;
	default:
		dev_err(dev, "unsupported ID %u\n", id);
		return -EINVAL;
	}

	return 0;
}

static int upboard_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct upboard_ddata *ddata;
	struct regmap *regmap;
	int ret;

	ddata = devm_kzalloc(dev, sizeof(*ddata), GFP_KERNEL);
	if (!ddata)
		return -ENOMEM;

	dev_set_drvdata(dev, ddata);

	ret = upboard_match_device(dev);
	if (ret)
		return ret;

	regmap = devm_regmap_init(dev, NULL, ddata, ddata->regmapconf);
	if (IS_ERR(regmap))
		return PTR_ERR(regmap);

	ret = upboard_init_gpio(dev);
	if (ret) {
		if (ret != -EPROBE_DEFER)
			dev_err(dev, "failed to init GPIOs: %d", ret);
		return ret;
	}

	ret = upboard_check_supported(dev, regmap);
	if (ret)
		return ret;

	return devm_mfd_add_devices(dev, 0, ddata->cells, ddata->ncells,
				    NULL, 0, NULL);
}

static struct platform_driver upboard_driver = {
	.probe = upboard_probe,
	.driver = {
		.name = "upboard",
		.acpi_match_table = upboard_acpi_match,
	},
};

module_platform_driver(upboard_driver);

MODULE_AUTHOR("Javier Arteaga <javier@emutex.com>");
MODULE_DESCRIPTION("UP Board platform controller driver");
MODULE_LICENSE("GPL v2");
