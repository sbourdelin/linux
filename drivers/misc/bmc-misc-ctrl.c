// SPDX-License-Identifier: GPL-2.0+
// Copyright 2018 IBM Corp.

#include <linux/bitops.h>
#include <linux/device.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

struct bmc_misc_label {
	const char *label;
	struct device_attribute label_attr;
};

struct bmc_misc_field {
	u32 shift;
	u32 mask;
	struct device_attribute mask_attr;
};

struct bmc_misc_type {
	const char *type;
	struct device_attribute type_attr;
};

struct bmc_misc_rw {
	struct regmap *map;

	struct bmc_misc_field field;
	struct bmc_misc_label label;
	struct bmc_misc_type type;

	u32 value;
	struct device_attribute value_attr;

	struct attribute_group attr_grp;
	struct attribute *attrs[5];
};

struct bmc_misc_sc {
	struct regmap *map;

	struct bmc_misc_field field;
	struct bmc_misc_label label;
	struct bmc_misc_type type;

	u32 read;
	u32 set;
	u32 clear;

	struct device_attribute read_attr;
	struct device_attribute set_attr;
	struct device_attribute clear_attr;

	struct attribute_group attr_grp;
	struct attribute *attrs[7];
};

static ssize_t bmc_misc_label_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct bmc_misc_label *priv;

	priv = container_of(attr, struct bmc_misc_label, label_attr);

	return sprintf(buf, "%s\n", priv->label);
}

static int bmc_misc_label_init(struct device_node *node,
			       struct bmc_misc_label *priv)
{
	int rc;

	rc = of_property_read_string(node, "label", &priv->label);
	if (rc < 0)
		return rc;

	sysfs_attr_init(&priv->label_attr.attr);
	priv->label_attr.attr.name = "label";
	priv->label_attr.attr.mode = 0440;
	priv->label_attr.show = bmc_misc_label_show;

	return 0;
}

static ssize_t bmc_misc_mask_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct bmc_misc_field *priv;

	priv = container_of(attr, struct bmc_misc_field, mask_attr);

	return sprintf(buf, "0x%x\n", priv->mask >> priv->shift);
}

static int bmc_misc_field_init(struct device_node *node,
			       struct bmc_misc_field *priv)
{
	int rc;

	rc = of_property_read_u32(node, "mask", &priv->mask);
	if (rc < 0)
		return rc;

	priv->shift = __ffs(priv->mask);

	sysfs_attr_init(&priv->mask_attr.attr);
	priv->mask_attr.attr.name = "mask";
	priv->mask_attr.attr.mode = 0440;
	priv->mask_attr.show = bmc_misc_mask_show;

	return 0;
}

static ssize_t bmc_misc_type_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct bmc_misc_type *priv;

	priv = container_of(attr, struct bmc_misc_type, type_attr);

	return sprintf(buf, "%s\n", priv->type);
}

static int bmc_misc_type_init(struct device_node *node,
			      struct bmc_misc_type *priv)
{
	bool ro, w1sc;

	ro = of_property_read_bool(node, "read-only");
	w1sc = of_property_read_bool(node, "set-clear");

	if (ro && !w1sc)
		priv->type = "ro";
	else if (!ro && w1sc)
		priv->type = "w1sc";
	else if (!ro && !w1sc)
		priv->type = "rw";
	else
		return -EINVAL;

	sysfs_attr_init(&priv->type_attr.attr);
	priv->type_attr.attr.name = "type";
	priv->type_attr.attr.mode = 0440;
	priv->type_attr.show = bmc_misc_type_show;

	return 0;
}

static ssize_t bmc_misc_rw_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct bmc_misc_rw *rw;
	unsigned int val;
	int rc;

	rw = container_of(attr, struct bmc_misc_rw, value_attr);
	rc = regmap_read(rw->map, rw->value, &val);
	if (rc)
		return rc;

	val = (val & rw->field.mask) >> rw->field.shift;

	return sprintf(buf, "%u\n", val);
}

static ssize_t bmc_misc_rw_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t count)
{
	struct bmc_misc_rw *rw;
	long val;
	int rc;

	rc = kstrtol(buf, 0, &val);
	if (rc)
		return rc;

	rw = container_of(attr, struct bmc_misc_rw, value_attr);
	val <<= rw->field.shift;
	if (val & ~rw->field.mask)
		return -EINVAL;
	rc = regmap_update_bits(rw->map, rw->value, rw->field.mask,
				val);

	return rc < 0 ? rc : count;
}

static int bmc_misc_rw_init(struct platform_device *pdev)
{
	struct device_node *node;
	struct bmc_misc_rw *priv;
	u32 val;
	int rc;

	node = pdev->dev.of_node;

	priv = devm_kmalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->map = syscon_node_to_regmap(pdev->dev.parent->of_node);

	rc = of_property_read_u32(node, "offset", &priv->value);
	if (rc < 0)
		return rc;

	rc = bmc_misc_label_init(node, &priv->label);
	if (rc < 0)
		return rc;

	rc = bmc_misc_field_init(node, &priv->field);
	if (rc < 0)
		return rc;

	rc = bmc_misc_type_init(node, &priv->type);
	if (rc < 0)
		return rc;

	if (!of_property_read_u32(node, "default-value", &val)) {
		val <<= priv->field.shift;
		if (val & ~priv->field.mask)
			return -EINVAL;
		val &= priv->field.mask;
		regmap_update_bits(priv->map, priv->value, priv->field.mask,
				   val);
	}

	sysfs_attr_init(&priv->value_attr.attr);
	priv->value_attr.attr.name = "value";
	priv->value_attr.attr.mode = 0440;
	if (!of_property_read_bool(node, "read-only"))
		priv->value_attr.attr.mode |= 0220;
	priv->value_attr.show = bmc_misc_rw_show;
	priv->value_attr.store = bmc_misc_rw_store;

	priv->attrs[0] = &priv->label.label_attr.attr;
	priv->attrs[1] = &priv->field.mask_attr.attr;
	priv->attrs[2] = &priv->type.type_attr.attr;
	priv->attrs[3] = &priv->value_attr.attr;
	priv->attrs[4] = NULL;

	memset(&priv->attr_grp, 0, sizeof(priv->attr_grp));
	priv->attr_grp.name = priv->label.label;
	priv->attr_grp.attrs = priv->attrs;


	rc = sysfs_create_group(&pdev->dev.kobj, &priv->attr_grp);
	if (rc < 0)
		return rc;

	platform_set_drvdata(pdev, priv);

	dev_info(&pdev->dev, "%s field %s\n", priv->type.type,
		 priv->label.label);

	return 0;
}

static ssize_t bmc_misc_sc_read_show(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	struct bmc_misc_sc *priv;
	unsigned int val;
	int rc;

	priv = container_of(attr, struct bmc_misc_sc, read_attr);
	rc = regmap_read(priv->map, priv->read, &val);
	if (rc)
		return rc;

	val = (val & priv->field.mask) >> priv->field.shift;

	return sprintf(buf, "%u\n", val);
}

static ssize_t bmc_misc_sc_set_store(struct device *dev,
				     struct device_attribute *attr,
				     const char *buf, size_t count)
{
	struct bmc_misc_sc *priv;
	long val;
	int rc;

	rc = kstrtol(buf, 0, &val);
	if (rc)
		return rc;

	priv = container_of(attr, struct bmc_misc_sc, set_attr);
	val <<= priv->field.shift;
	if (val & ~priv->field.mask)
		return -EINVAL;
	val &= priv->field.mask;
	rc = regmap_write(priv->map, priv->set, val);

	return rc < 0 ? rc : count;
}

static ssize_t bmc_misc_sc_clear_store(struct device *dev,
				       struct device_attribute *attr,
				       const char *buf, size_t count)
{
	struct bmc_misc_sc *priv;
	long val;
	int rc;

	rc = kstrtol(buf, 0, &val);
	if (rc)
		return rc;

	priv = container_of(attr, struct bmc_misc_sc, clear_attr);
	val <<= priv->field.shift;
	if (val & ~priv->field.mask)
		return -EINVAL;
	val &= priv->field.mask;
	rc = regmap_write(priv->map, priv->clear, val);

	return rc < 0 ? rc : count;
}

static int bmc_misc_sc_init(struct platform_device *pdev)
{
	struct device_node *node;
	struct bmc_misc_sc *priv;
	int rc;

	node = pdev->dev.of_node;

	priv = devm_kmalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->map = syscon_node_to_regmap(pdev->dev.parent->of_node);

	rc = of_property_read_u32_array(node, "offset", &priv->read, 3);
	if (rc < 0)
		return rc;

	rc = bmc_misc_label_init(node, &priv->label);
	if (rc < 0)
		return rc;

	rc = bmc_misc_field_init(node, &priv->field);
	if (rc < 0)
		return rc;

	rc = bmc_misc_type_init(node, &priv->type);
	if (rc < 0)
		return rc;

	if (of_property_read_bool(node, "default-set"))
		regmap_write(priv->map, priv->set, priv->field.mask);
	else if (of_property_read_bool(node, "default-clear"))
		regmap_write(priv->map, priv->clear, priv->field.mask);

	sysfs_attr_init(&priv->read_attr.attr);
	priv->read_attr.attr.name = "value";
	priv->read_attr.attr.mode = 0440;
	priv->read_attr.show = bmc_misc_sc_read_show;

	sysfs_attr_init(&priv->set_attr.attr);
	priv->set_attr.attr.name = "set";
	priv->set_attr.attr.mode = 0220;
	priv->set_attr.store = bmc_misc_sc_set_store;

	sysfs_attr_init(&priv->clear_attr.attr);
	priv->clear_attr.attr.name = "clear";
	priv->clear_attr.attr.mode = 0220;
	priv->clear_attr.store = bmc_misc_sc_clear_store;

	priv->attrs[0] = &priv->label.label_attr.attr;
	priv->attrs[1] = &priv->field.mask_attr.attr;
	priv->attrs[2] = &priv->type.type_attr.attr;
	priv->attrs[3] = &priv->read_attr.attr;
	priv->attrs[4] = &priv->set_attr.attr;
	priv->attrs[5] = &priv->clear_attr.attr;
	priv->attrs[6] = NULL;

	memset(&priv->attr_grp, 0, sizeof(priv->attr_grp));
	priv->attr_grp.name = priv->label.label;
	priv->attr_grp.attrs = priv->attrs;

	rc = sysfs_create_group(&pdev->dev.kobj, &priv->attr_grp);
	if (rc < 0)
		return rc;

	platform_set_drvdata(pdev, priv);

	dev_info(&pdev->dev, "%s field %s\n", priv->type.type,
		 priv->label.label);

	return 0;
}

static int bmc_misc_probe(struct platform_device *pdev)
{
	if (of_property_read_bool(pdev->dev.of_node, "set-clear"))
		return bmc_misc_sc_init(pdev);

	return bmc_misc_rw_init(pdev);
}

static void bmc_misc_sc_del(struct platform_device *pdev)
{
	struct bmc_misc_sc *priv = platform_get_drvdata(pdev);

	sysfs_remove_group(&pdev->dev.kobj, &priv->attr_grp);
}

static void bmc_misc_rw_del(struct platform_device *pdev)
{
	struct bmc_misc_rw *priv = platform_get_drvdata(pdev);

	sysfs_remove_group(&pdev->dev.kobj, &priv->attr_grp);
}

static int bmc_misc_remove(struct platform_device *pdev)
{
	if (of_property_read_bool(pdev->dev.of_node, "set-clear"))
		bmc_misc_sc_del(pdev);
	else
		bmc_misc_rw_del(pdev);

	return 0;
}

static const struct of_device_id bmc_misc_ctrl_match[] = {
	{ .compatible = "bmc-misc-ctrl", },
	{ },
};

static struct platform_driver bmc_misc_ctrl = {
	.driver = {
		.name		=  "bmc-misc-ctrl",
		.of_match_table = bmc_misc_ctrl_match,
	},
	.probe = bmc_misc_probe,
	.remove = bmc_misc_remove,
};

module_platform_driver(bmc_misc_ctrl);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Andrew Jeffery <andrew@aj.id.au>");
