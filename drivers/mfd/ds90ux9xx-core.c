// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * TI DS90Ux9xx MFD driver
 *
 * Copyright (c) 2017-2018 Mentor Graphics Inc.
 */

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/mfd/core.h>
#include <linux/mfd/ds90ux9xx.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of_platform.h>
#include <linux/regmap.h>

/* Serializer Registers */
#define SER_REG_MODE_SELECT		0x04
#define SER_REG_GEN_STATUS		0x0C
#define SER_REG_CTRL_STATUS		0x13

/* Deserializer Registers */
#define DES_REG_GEN_CONFIG0		0x02
#define DES_REG_GEN_STATUS		0x1C
#define DES_REG_MODE_STATUS		0x23
#define DES_REG_SSCG_CTRL		0x2C
#define DES_REG_DUAL_RX_CTRL		0x34
#define DES_REG_MAPSEL			0x49
#define DES_REG_INDIRECT_ADDR		0x6C
#define DES_REG_INDIRECT_DATA		0x6D

#define DES_FPD_MODE_MASK		(BIT(4) | BIT(3))
#define DES_FPD_HW_MODE_AUTO		0x0
#define DES_FPD_HW_MODE_PRIMARY		BIT(4)
#define DES_FPD_HW_MODE_SECONDARY	(BIT(4) | BIT(3))

#define SSCG_REG_MASK			0x0F
#define SSCG_MAX_VALUE			0x7
#define SSCG_ENABLE			BIT(3)

/* Common Registers and bitfields */
#define SER_DES_REG_CONFIG		0x03

#define SER_DE_GATE_RGB			BIT(4)
#define DES_DE_GATE_RGB			BIT(1)

#define PIXEL_CLK_EDGE_RISING		BIT(0)

#define MAPSEL_MASK			(BIT(6) | BIT(5))
#define MAPSEL_SET			BIT(6)
#define MAPSEL_MSB			BIT(5)

#define BKWD_MODE_OVERRIDE		BIT(3)
#define BKWD_MODE_ENABLED		BIT(2)
#define LF_MODE_OVERRIDE		BIT(1)
#define LF_MODE_ENABLED			BIT(0)

#define LF_MODE_PIN_STATUS		BIT(3)
#define BKWD_MODE_PIN_STATUS		BIT(1)

#define GEN_STATUS_LOCK			BIT(0)

#define SER_DES_DEVICE_ID_REG		0xF0
#define DEVICE_ID_LEN			6

enum ds90ux9xx_capability {
	DS90UX9XX_CAP_SERIALIZER,
	DS90UX9XX_CAP_HDCP,
	DS90UX9XX_CAP_MODES,
	DS90UX9XX_CAP_SSCG,
	DS90UX9XX_CAP_PIXEL_CLK_EDGE,
	DS90UX9XX_CAP_MAPSEL,
	DS90UX9XX_CAP_DE_GATE,
};

struct ds90ux9xx_device_property {
	const char id[DEVICE_ID_LEN];
	unsigned int num_links;
	const u32 caps;
};

struct ds90ux9xx {
	struct device *dev;
	struct regmap *regmap;
	struct mutex indirect;	/* serializes access to indirect registers */
	struct gpio_desc *power_gpio;
	enum ds90ux9xx_device_id dev_id;
	const struct ds90ux9xx_device_property *property;
};

#define TO_CAP(_cap, _enabled)	(_enabled ? BIT(DS90UX9XX_CAP_ ## _cap) : 0x0)

#define DS90UX9XX_DEVICE(_id, _links, _ser, _hdcp, _modes, _sscg,	\
			 _pixel, _mapsel, _de_gate)			\
	[TI_DS90 ## _id] = {						\
		.id = "_" __stringify(_id),				\
		.num_links = _links,					\
		.caps =							\
			TO_CAP(SERIALIZER, _ser)	|		\
			TO_CAP(HDCP, _hdcp)		|		\
			TO_CAP(MODES, _modes)		|		\
			TO_CAP(SSCG, _sscg)		|		\
			TO_CAP(PIXEL_CLK_EDGE, _pixel)	|		\
			TO_CAP(MAPSEL, _mapsel)		|		\
			TO_CAP(DE_GATE, _de_gate)			\
	}

static const struct ds90ux9xx_device_property ds90ux9xx_devices[] =  {
	/*
	 * List of TI DS90Ux9xx properties:
	 *      # FPD-links, serializer, HDCP, BW/LF modes, SSCG,
	 *      pixel clock edge, mapsel, RGB DE Gate
	 */
	DS90UX9XX_DEVICE(UB925, 1, 1, 0, 1, 0, 1, 0, 1),
	DS90UX9XX_DEVICE(UH925, 1, 1, 1, 1, 0, 1, 0, 1),
	DS90UX9XX_DEVICE(UB927, 1, 1, 0, 1, 0, 0, 1, 1),
	DS90UX9XX_DEVICE(UH927, 1, 1, 1, 1, 0, 0, 1, 1),
	DS90UX9XX_DEVICE(UB926, 1, 0, 0, 1, 1, 1, 0, 0),
	DS90UX9XX_DEVICE(UH926, 1, 0, 1, 1, 1, 1, 0, 0),
	DS90UX9XX_DEVICE(UB928, 1, 0, 0, 1, 0, 0, 1, 0),
	DS90UX9XX_DEVICE(UH928, 1, 0, 1, 1, 0, 0, 1, 0),
	DS90UX9XX_DEVICE(UB940, 2, 0, 0, 0, 0, 0, 0, 1),
	DS90UX9XX_DEVICE(UH940, 2, 0, 1, 0, 0, 0, 0, 1),
};

static const struct regmap_config ds90ux9xx_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = 0xFF,
	.cache_type = REGCACHE_NONE,
};

static bool ds90ux9xx_has_capability(struct ds90ux9xx *ds90ux9xx,
				     enum ds90ux9xx_capability cap)
{
	return ds90ux9xx->property->caps & BIT(cap);
}

bool ds90ux9xx_is_serializer(struct device *dev)
{
	struct ds90ux9xx *ds90ux9xx = dev_get_drvdata(dev);

	return ds90ux9xx_has_capability(ds90ux9xx, DS90UX9XX_CAP_SERIALIZER);
}
EXPORT_SYMBOL_GPL(ds90ux9xx_is_serializer);

unsigned int ds90ux9xx_num_fpd_links(struct device *dev)
{
	struct ds90ux9xx *ds90ux9xx = dev_get_drvdata(dev);

	return ds90ux9xx->property->num_links;
}
EXPORT_SYMBOL_GPL(ds90ux9xx_num_fpd_links);

int ds90ux9xx_update_bits_indirect(struct device *dev, u8 reg, u8 mask, u8 val)
{
	struct ds90ux9xx *ds90ux9xx = dev_get_drvdata(dev);
	int ret;

	mutex_lock(&ds90ux9xx->indirect);

	ret = regmap_write(ds90ux9xx->regmap, DES_REG_INDIRECT_ADDR, reg);
	if (ret) {
		mutex_unlock(&ds90ux9xx->indirect);
		return ret;
	}

	ret = regmap_update_bits(ds90ux9xx->regmap, DES_REG_INDIRECT_DATA,
				 mask, val);

	mutex_unlock(&ds90ux9xx->indirect);

	return ret;
}
EXPORT_SYMBOL_GPL(ds90ux9xx_update_bits_indirect);

int ds90ux9xx_write_indirect(struct device *dev, u8 reg, u8 val)
{
	struct ds90ux9xx *ds90ux9xx = dev_get_drvdata(dev);
	int ret;

	mutex_lock(&ds90ux9xx->indirect);

	ret = regmap_write(ds90ux9xx->regmap, DES_REG_INDIRECT_ADDR, reg);
	if (ret) {
		mutex_unlock(&ds90ux9xx->indirect);
		return ret;
	}

	ret = regmap_write(ds90ux9xx->regmap, DES_REG_INDIRECT_DATA, val);

	mutex_unlock(&ds90ux9xx->indirect);

	return ret;
}
EXPORT_SYMBOL_GPL(ds90ux9xx_write_indirect);

int ds90ux9xx_read_indirect(struct device *dev, u8 reg, u8 *val)
{
	struct ds90ux9xx *ds90ux9xx = dev_get_drvdata(dev);
	unsigned int data;
	int ret;

	mutex_lock(&ds90ux9xx->indirect);

	ret = regmap_write(ds90ux9xx->regmap, DES_REG_INDIRECT_ADDR, reg);
	if (ret) {
		mutex_unlock(&ds90ux9xx->indirect);
		return ret;
	}

	ret = regmap_read(ds90ux9xx->regmap, DES_REG_INDIRECT_DATA, &data);

	mutex_unlock(&ds90ux9xx->indirect);

	if (!ret)
		*val = data;

	return ret;
}
EXPORT_SYMBOL_GPL(ds90ux9xx_read_indirect);

static int ds90ux9xx_read_active_link(struct ds90ux9xx *ds90ux9xx,
				      unsigned int *link)
{
	unsigned int val;
	int ret;

	if (ds90ux9xx->property->num_links == 1) {
		*link = 0;
		return 0;
	}

	ret = regmap_read(ds90ux9xx->regmap, DES_REG_DUAL_RX_CTRL, &val);
	if (ret) {
		dev_err(ds90ux9xx->dev, "Failed to read FPD mode\n");
		return ret;
	}

	switch (val & DES_FPD_MODE_MASK) {
	case DES_FPD_HW_MODE_AUTO:
	case DES_FPD_HW_MODE_PRIMARY:
		*link = 0;
		break;
	case DES_FPD_HW_MODE_SECONDARY:
		*link = 1;
		break;
	default:
		dev_err(ds90ux9xx->dev, "Unhandled FPD mode\n");
		return -EINVAL;
	}

	return 0;
}

/* Note that lock status is reported if video pattern generator is enabled */
int ds90ux9xx_get_link_status(struct device *dev, unsigned int *link,
			      bool *locked)
{
	struct ds90ux9xx *ds90ux9xx = dev_get_drvdata(dev);
	unsigned int reg, status;
	int ret;

	ret = ds90ux9xx_read_active_link(ds90ux9xx, link);
	if (ret)
		return ret;

	if (ds90ux9xx_is_serializer(dev))
		reg = SER_REG_GEN_STATUS;
	else
		reg = DES_REG_GEN_STATUS;

	ret = regmap_read(ds90ux9xx->regmap, reg, &status);
	if (ret) {
		dev_err(dev, "Unable to get lock status\n");
		return ret;
	}

	*locked = status & GEN_STATUS_LOCK ? true : false;

	return 0;
}
EXPORT_SYMBOL_GPL(ds90ux9xx_get_link_status);

enum ds90ux9xx_device_id ds90ux9xx_get_ic_type(struct device *dev)
{
	struct ds90ux9xx *ds90ux9xx = dev_get_drvdata(dev);

	return ds90ux9xx->dev_id;
}
EXPORT_SYMBOL_GPL(ds90ux9xx_get_ic_type);

static int ds90ux9xx_set_de_gate(struct ds90ux9xx *ds90ux9xx, bool enable)
{
	unsigned int reg, val;

	if (!ds90ux9xx_has_capability(ds90ux9xx, DS90UX9XX_CAP_DE_GATE))
		return 0;

	if (ds90ux9xx_is_serializer(ds90ux9xx->dev)) {
		reg = SER_REG_MODE_SELECT;
		val = SER_DE_GATE_RGB;
	} else {
		reg = SER_DES_REG_CONFIG;
		val = DES_DE_GATE_RGB;
	}

	return regmap_update_bits(ds90ux9xx->regmap, reg, val,
				  enable ? val : 0x0);
}

static int ds90ux9xx_set_bcmode(struct ds90ux9xx *ds90ux9xx, bool enable)
{
	unsigned int reg, val;
	int ret;

	if (ds90ux9xx_is_serializer(ds90ux9xx->dev))
		reg = SER_REG_MODE_SELECT;
	else
		reg = DES_REG_GEN_CONFIG0;

	val = BKWD_MODE_OVERRIDE;
	if (enable)
		val |= BKWD_MODE_ENABLED;

	ret = regmap_update_bits(ds90ux9xx->regmap, reg,
				 BKWD_MODE_OVERRIDE | BKWD_MODE_ENABLED, val);
	if (ret)
		return ret;

	return ds90ux9xx_set_de_gate(ds90ux9xx, !enable);
}

static int ds90ux9xx_set_lfmode(struct ds90ux9xx *ds90ux9xx, bool enable)
{
	unsigned int reg, val;

	if (ds90ux9xx_is_serializer(ds90ux9xx->dev))
		reg = SER_REG_MODE_SELECT;
	else
		reg = DES_REG_GEN_CONFIG0;

	val = LF_MODE_OVERRIDE;
	if (enable)
		val |= LF_MODE_ENABLED;

	return regmap_update_bits(ds90ux9xx->regmap, reg,
				  LF_MODE_OVERRIDE | LF_MODE_ENABLED, val);
}

static int ds90ux9xx_set_sscg(struct ds90ux9xx *ds90ux9xx, unsigned int val)
{
	if ((val & ~SSCG_ENABLE) > SSCG_MAX_VALUE) {
		dev_err(ds90ux9xx->dev, "Invalid SSCG value: %#x", val);
		return -EINVAL;
	}

	return regmap_write(ds90ux9xx->regmap, DES_REG_SSCG_CTRL, val);
}

static int ds90ux9xx_set_pixel_clk_edge(struct ds90ux9xx *ds90ux9xx,
					bool rising)
{
	return regmap_update_bits(ds90ux9xx->regmap, SER_DES_REG_CONFIG,
				  PIXEL_CLK_EDGE_RISING,
				  rising ? PIXEL_CLK_EDGE_RISING : 0x0);
}

static ssize_t backward_compatible_mode_show(struct device *dev,
					     struct device_attribute *attr,
					     char *buf)
{
	struct ds90ux9xx *ds90ux9xx = dev_get_drvdata(dev);
	unsigned int cfg_reg, sts_reg, val;
	int ret, bcmode;

	if (ds90ux9xx_is_serializer(ds90ux9xx->dev)) {
		cfg_reg = SER_REG_MODE_SELECT;
		sts_reg = SER_REG_CTRL_STATUS;
	} else {
		cfg_reg = DES_REG_GEN_CONFIG0;
		sts_reg = DES_REG_MODE_STATUS;
	}

	ret = regmap_read(ds90ux9xx->regmap, cfg_reg, &val);
	if (ret)
		return ret;

	if (val & BKWD_MODE_OVERRIDE) {
		bcmode = val & BKWD_MODE_ENABLED;
	} else {
		/* Read mode from pin status */
		ret = regmap_read(ds90ux9xx->regmap, sts_reg, &val);
		if (ret)
			return ret;

		bcmode = val & BKWD_MODE_PIN_STATUS;
	}

	return sprintf(buf, "%d\n", bcmode ? 1 : 0);
}

static ssize_t backward_compatible_mode_store(struct device *dev,
					      struct device_attribute *attr,
					      const char *buf, size_t count)
{
	struct ds90ux9xx *ds90ux9xx = dev_get_drvdata(dev);
	unsigned int val;
	int ret;

	ret = kstrtouint(buf, 0, &val);
	if (ret)
		return ret;

	ret = ds90ux9xx_set_bcmode(ds90ux9xx, val);
	if (ret)
		return ret;

	return count;
}
static DEVICE_ATTR_RW(backward_compatible_mode);

static ssize_t low_frequency_mode_show(struct device *dev,
				       struct device_attribute *attr, char *buf)
{
	struct ds90ux9xx *ds90ux9xx = dev_get_drvdata(dev);
	unsigned int cfg_reg, sts_reg, val;
	int ret, lfmode;

	if (ds90ux9xx_is_serializer(ds90ux9xx->dev)) {
		cfg_reg = SER_REG_MODE_SELECT;
		sts_reg = SER_REG_CTRL_STATUS;
	} else {
		cfg_reg = DES_REG_GEN_CONFIG0;
		sts_reg = DES_REG_MODE_STATUS;
	}

	ret = regmap_read(ds90ux9xx->regmap, cfg_reg, &val);
	if (ret)
		return ret;

	if (val & LF_MODE_OVERRIDE) {
		lfmode = val & LF_MODE_ENABLED;
	} else {
		/* Read mode from pin status */
		ret = regmap_read(ds90ux9xx->regmap, sts_reg, &val);
		if (ret)
			return ret;

		lfmode = val & LF_MODE_PIN_STATUS;
	}

	return sprintf(buf, "%d\n", lfmode ? 1 : 0);
}

static ssize_t low_frequency_mode_store(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t count)
{
	struct ds90ux9xx *ds90ux9xx = dev_get_drvdata(dev);
	unsigned int val;
	int ret;

	ret = kstrtouint(buf, 0, &val);
	if (ret)
		return ret;

	ret = ds90ux9xx_set_lfmode(ds90ux9xx, val);
	if (ret)
		return ret;

	return count;
}
static DEVICE_ATTR_RW(low_frequency_mode);

static ssize_t sscg_show(struct device *dev, struct device_attribute *attr,
			 char *buf)
{
	struct ds90ux9xx *ds90ux9xx = dev_get_drvdata(dev);
	unsigned int val;
	int ret;

	ret = regmap_read(ds90ux9xx->regmap, DES_REG_SSCG_CTRL, &val);
	if (ret)
		return ret;

	return sprintf(buf, "%d\n", val & SSCG_REG_MASK);
}

static ssize_t sscg_store(struct device *dev, struct device_attribute *attr,
			  const char *buf, size_t count)
{
	struct ds90ux9xx *ds90ux9xx = dev_get_drvdata(dev);
	int ret, val;

	ret = kstrtoint(buf, 0, &val);
	if (ret)
		return ret;

	ret = ds90ux9xx_set_sscg(ds90ux9xx, val);
	if (ret)
		return ret;

	return count;
}
static DEVICE_ATTR_RW(sscg);

static ssize_t pixel_clock_edge_show(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	struct ds90ux9xx *ds90ux9xx = dev_get_drvdata(dev);
	unsigned int val;
	int ret;

	ret = regmap_read(ds90ux9xx->regmap, SER_DES_REG_CONFIG, &val);
	if (ret)
		return ret;

	return sprintf(buf, "%s\n",
		       (val & PIXEL_CLK_EDGE_RISING) ? "rising" : "falling");
}

static ssize_t pixel_clock_edge_store(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf, size_t count)
{
	struct ds90ux9xx *ds90ux9xx = dev_get_drvdata(dev);
	bool rising;
	int ret;

	if (sysfs_streq(buf, "rising") || sysfs_streq(buf, "1"))
		rising = true;
	else if (sysfs_streq(buf, "falling") || sysfs_streq(buf, "0"))
		rising = false;
	else
		return -EINVAL;

	ret = ds90ux9xx_set_pixel_clk_edge(ds90ux9xx, rising);
	if (ret)
		return ret;

	return count;
}
static DEVICE_ATTR_RW(pixel_clock_edge);

static ssize_t link_show(struct device *dev,
			 struct device_attribute *attr, char *buf)
{
	unsigned int link;
	bool locked;
	int ret;

	ret = ds90ux9xx_get_link_status(dev, &link, &locked);
	if (ret)
		return ret;

	return sprintf(buf, "%u: %s\n", link, locked ? "locked" : "unlocked");
}
static DEVICE_ATTR_RO(link);

static struct attribute *ds90ux9xx_attributes[] = {
	&dev_attr_backward_compatible_mode.attr,
	&dev_attr_low_frequency_mode.attr,
	&dev_attr_sscg.attr,
	&dev_attr_pixel_clock_edge.attr,
	&dev_attr_link.attr,
	NULL,
};

static umode_t ds90ux9xx_attr_is_visible(struct kobject *kobj,
					 struct attribute *attr, int index)
{
	struct device *dev = container_of(kobj, struct device, kobj);
	struct ds90ux9xx *ds90ux9xx = dev_get_drvdata(dev);
	enum ds90ux9xx_capability cap;

	/* Attribute indices match the ds90ux9xx_attributes[] array indices */
	switch (index) {
	case 0:	/* Backward compatible mode */
	case 1:	/* Low frequency mode */
		cap = DS90UX9XX_CAP_MODES;
		break;
	case 2:	/* Spread spectrum clock generator */
		cap = DS90UX9XX_CAP_SSCG;
		break;
	case 3:	/* Pixel clock edge */
		cap = DS90UX9XX_CAP_PIXEL_CLK_EDGE;
		break;
	case 4: /* FPD-III link lock status - visible for all chips */
		return attr->mode;
	default:
		return 0;
	}

	return ds90ux9xx_has_capability(ds90ux9xx, cap) ? attr->mode : 0;
}

static const struct attribute_group ds90ux9xx_attr_group = {
	.attrs = ds90ux9xx_attributes,
	.is_visible = ds90ux9xx_attr_is_visible,
};
__ATTRIBUTE_GROUPS(ds90ux9xx_attr);

static int ds90ux9xx_config_modes(struct ds90ux9xx *ds90ux9xx)
{
	struct device_node *np = ds90ux9xx->dev->of_node;
	u32 mode;
	int ret;

	if (!ds90ux9xx_has_capability(ds90ux9xx, DS90UX9XX_CAP_MODES))
		return 0;

	ret = of_property_read_u32(np, "ti,backward-compatible-mode", &mode);
	if (!ret) {
		ret = ds90ux9xx_set_bcmode(ds90ux9xx, mode);
		if (ret)
			return ret;
	} else {
		ret = ds90ux9xx_set_de_gate(ds90ux9xx, true);
		if (ret)
			return ret;
	}

	ret = of_property_read_u32(np, "ti,low-frequency-mode", &mode);
	if (!ret) {
		ret = ds90ux9xx_set_lfmode(ds90ux9xx, mode);
		if (ret)
			return ret;
	}

	return 0;
}

static int ds90ux9xx_config_sscg(struct ds90ux9xx *ds90ux9xx)
{
	struct device_node *np = ds90ux9xx->dev->of_node;
	u32 sscg;
	int ret;

	if (!ds90ux9xx_has_capability(ds90ux9xx, DS90UX9XX_CAP_SSCG))
		return 0;

	ret = of_property_read_u32(np, "ti,spread-spectrum-clock-generation",
				   &sscg);
	if (ret)
		return 0;

	return ds90ux9xx_set_sscg(ds90ux9xx, sscg);
}

static int ds90ux9xx_config_pixel_clk_edge(struct ds90ux9xx *ds90ux9xx)
{
	struct device_node *np = ds90ux9xx->dev->of_node;
	const char *pixel;
	bool rising;

	if (!ds90ux9xx_has_capability(ds90ux9xx, DS90UX9XX_CAP_PIXEL_CLK_EDGE))
		return 0;

	if (of_property_read_string(np, "ti,pixel-clock-edge", &pixel))
		return 0;

	if (!strcmp(pixel, "rising"))
		rising = true;
	else if (!strcmp(pixel, "falling"))
		rising = false;
	else
		return -EINVAL;

	return ds90ux9xx_set_pixel_clk_edge(ds90ux9xx, rising);
}

static int ds90ux9xx_config_map_select(struct ds90ux9xx *ds90ux9xx)
{
	struct device_node *np = ds90ux9xx->dev->of_node;
	unsigned int reg, val;

	if (!ds90ux9xx_has_capability(ds90ux9xx, DS90UX9XX_CAP_MAPSEL))
		return 0;

	if (ds90ux9xx_is_serializer(ds90ux9xx->dev))
		reg = SER_REG_CTRL_STATUS;
	else
		reg = DES_REG_MAPSEL;

	if (of_property_read_bool(np, "ti,video-map-select-msb"))
		val = MAPSEL_SET | MAPSEL_MSB;
	else if (of_property_read_bool(np, "ti,video-map-select-lsb"))
		val = MAPSEL_SET;
	else
		val = 0;

	return regmap_update_bits(ds90ux9xx->regmap, reg, MAPSEL_MASK, val);
}

static int ds90ux9xx_config_properties(struct ds90ux9xx *ds90ux9xx)
{
	int ret;

	ret = ds90ux9xx_config_modes(ds90ux9xx);
	if (ret)
		return ret;

	ret = ds90ux9xx_config_sscg(ds90ux9xx);
	if (ret)
		return ret;

	ret = ds90ux9xx_config_pixel_clk_edge(ds90ux9xx);
	if (ret)
		return ret;

	return ds90ux9xx_config_map_select(ds90ux9xx);
}

static const struct i2c_device_id ds90ux9xx_ids[] = {
	/* Supported serializers */
	{ "ds90ub925q", TI_DS90UB925 },
	{ "ds90uh925q", TI_DS90UH925 },
	{ "ds90ub927q", TI_DS90UB927 },
	{ "ds90uh927q", TI_DS90UH927 },

	/* Supported deserializers */
	{ "ds90ub926q", TI_DS90UB926 },
	{ "ds90uh926q", TI_DS90UH926 },
	{ "ds90ub928q", TI_DS90UB928 },
	{ "ds90uh928q", TI_DS90UH928 },
	{ "ds90ub940q", TI_DS90UB940 },
	{ "ds90uh940q", TI_DS90UH940 },
	{ },
};
MODULE_DEVICE_TABLE(i2c, ds90ux9xx_ids);

static const struct of_device_id ds90ux9xx_dt_ids[] = {
	{ .compatible = "ti,ds90ux9xx",  },
	{ .compatible = "ti,ds90ub925q", },
	{ .compatible = "ti,ds90uh925q", },
	{ .compatible = "ti,ds90ub927q", },
	{ .compatible = "ti,ds90uh927q", },
	{ .compatible = "ti,ds90ub926q", },
	{ .compatible = "ti,ds90uh926q", },
	{ .compatible = "ti,ds90ub928q", },
	{ .compatible = "ti,ds90uh928q", },
	{ .compatible = "ti,ds90ub940q", },
	{ .compatible = "ti,ds90uh940q", },
	{ },
};
MODULE_DEVICE_TABLE(of, ds90ux9xx_dt_ids);

static int ds90ux9xx_read_ic_type(struct i2c_client *client,
				  struct ds90ux9xx *ds90ux9xx)
{
	u8 device_id[DEVICE_ID_LEN + 1] = { 0 };
	const struct i2c_device_id *id;
	const char *id_code;
	unsigned int i;
	int ret;

	ret = regmap_raw_read(ds90ux9xx->regmap, SER_DES_DEVICE_ID_REG,
			      device_id, DEVICE_ID_LEN);
	if (ret < 0) {
		dev_err(ds90ux9xx->dev, "Failed to read device id: %d\n", ret);
		return ret;
	}

	id = i2c_match_id(ds90ux9xx_ids, client);
	if (id) {
		id_code = ds90ux9xx_devices[id->driver_data].id;
		if (!strncmp(device_id, id_code, DEVICE_ID_LEN)) {
			ds90ux9xx->dev_id = id->driver_data;
			return 0;
		}
	}

	/* DS90UH925 device quirk */
	if (!memcmp(device_id, "\0\0\0\0\0\0", DEVICE_ID_LEN)) {
		if (id && id->driver_data != TI_DS90UH925)
			dev_err(ds90ux9xx->dev,
				"Device ID returned all zeroes, assuming device is DS90UH925\n");
		ds90ux9xx->dev_id = TI_DS90UH925;
		return 0;
	}

	for (i = 0; i < ARRAY_SIZE(ds90ux9xx_devices); i++) {
		if (strncmp(device_id, ds90ux9xx_devices[i].id, DEVICE_ID_LEN))
			continue;

		if (id)
			dev_err(ds90ux9xx->dev,
				"Mismatch between probed device ID [%s] and HW device ID [%s]\n",
				id_code, device_id);

		ds90ux9xx->dev_id = i;
		return 0;
	}

	dev_err(ds90ux9xx->dev, "Device ID [%s] is not supported\n", device_id);

	return -ENODEV;
}

static int ds90ux9xx_probe(struct i2c_client *client)
{
	struct ds90ux9xx *ds90ux9xx;
	int ret;

	ds90ux9xx = devm_kzalloc(&client->dev, sizeof(*ds90ux9xx), GFP_KERNEL);
	if (!ds90ux9xx)
		return -ENOMEM;

	/* Get optional GPIO connected to PDB pin */
	ds90ux9xx->power_gpio = devm_gpiod_get_optional(&client->dev, "power",
							GPIOD_OUT_HIGH);
	if (IS_ERR(ds90ux9xx->power_gpio))
		return PTR_ERR(ds90ux9xx->power_gpio);

	/* Give time to the controller to initialize itself after power-up */
	if (ds90ux9xx->power_gpio)
		usleep_range(2000, 2500);

	ds90ux9xx->dev = &client->dev;
	ds90ux9xx->regmap = devm_regmap_init_i2c(client,
						 &ds90ux9xx_regmap_config);
	if (IS_ERR(ds90ux9xx->regmap))
		return PTR_ERR(ds90ux9xx->regmap);

	mutex_init(&ds90ux9xx->indirect);

	ret = ds90ux9xx_read_ic_type(client, ds90ux9xx);
	if (ret)
		return ret;

	ds90ux9xx->property = &ds90ux9xx_devices[ds90ux9xx->dev_id];

	i2c_set_clientdata(client, ds90ux9xx);

	ret = ds90ux9xx_config_properties(ds90ux9xx);
	if (ret)
		return ret;

	ret = sysfs_create_groups(&ds90ux9xx->dev->kobj, ds90ux9xx_attr_groups);
	if (ret)
		return ret;

	ret = devm_of_platform_populate(ds90ux9xx->dev);
	if (ret)
		sysfs_remove_groups(&ds90ux9xx->dev->kobj,
				    ds90ux9xx_attr_groups);

	return ret;
}

static int ds90ux9xx_remove(struct i2c_client *client)
{
	struct ds90ux9xx *ds90ux9xx = i2c_get_clientdata(client);

	sysfs_remove_groups(&ds90ux9xx->dev->kobj, ds90ux9xx_attr_groups);

	if (ds90ux9xx->power_gpio)
		gpiod_set_value_cansleep(ds90ux9xx->power_gpio, 0);

	return 0;
}

static struct i2c_driver ds90ux9xx_driver = {
	.driver = {
		.name = "ds90ux9xx",
		.of_match_table = ds90ux9xx_dt_ids,
	},
	.probe_new = ds90ux9xx_probe,
	.remove = ds90ux9xx_remove,
	.id_table = ds90ux9xx_ids,
};
module_i2c_driver(ds90ux9xx_driver);

MODULE_AUTHOR("Balasubramani Vivekanandan <balasubramani_vivekanandan@mentor.com>");
MODULE_AUTHOR("Vladimir Zapolskiy <vladimir_zapolskiy@mentor.com>");
MODULE_DESCRIPTION("TI DS90Ux9xx MFD driver");
MODULE_LICENSE("GPL");
