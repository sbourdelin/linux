// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * TI DS90Ux9xx I2C bridge/alias controller driver
 *
 * Copyright (c) 2017-2018 Mentor Graphics Inc.
 */

#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/kthread.h>
#include <linux/mfd/ds90ux9xx.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

/* Serializer Registers */
#define SER_REG_REMOTE_ID		0x06
#define SER_REG_I2C_CTRL		0x17

/* Deserializer Registers */
#define DES_REG_I2C_CTRL		0x05
#define DES_REG_REMOTE_ID		0x07

/* Common Register address */
#define SER_DES_REG_DEVICE_ID		0x00
#define DEVICE_ID_OVERRIDE		BIT(0)

#define SER_DES_REG_CONFIG		0x03
#define SER_CONFIG_I2C_AUTO_ACK		BIT(5)
#define CONFIG_I2C_PASS_THROUGH		BIT(3)
#define DES_CONFIG_I2C_AUTO_ACK	BIT(2)

#define I2C_CTRL_PASS_ALL		BIT(7)

#define DS90UX9XX_MAX_LINKED_DEVICES	2
#define DS90UX9XX_MAX_SLAVE_DEVICES	8

/* Chosen link connection timings */
#define CONN_MIN_TIME_MSEC		400U
#define CONN_STEP_TIME_MSEC		50U
#define CONN_MAX_TIME_MSEC		1000U

struct ds90ux9xx_i2c_data {
	const u8 slave_reg[DS90UX9XX_MAX_SLAVE_DEVICES];
	const u8 alias_reg[DS90UX9XX_MAX_SLAVE_DEVICES];
	const unsigned int num_slaves;
};

static const struct ds90ux9xx_i2c_data ds90ux925_i2c = {
	.slave_reg = { 0x07, },
	.alias_reg = { 0x08, },
	.num_slaves = 1,
};

static const struct ds90ux9xx_i2c_data ds90ux926_i2c = {
	.slave_reg = { 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, },
	.alias_reg = { 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, },
	.num_slaves = 8,
};

static const struct ds90ux9xx_i2c_data ds90ux927_i2c = {
	.slave_reg = { 0x07, 0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, },
	.alias_reg = { 0x08, 0x77, 0x78, 0x79, 0x7a, 0x7b, 0x7c, 0x7d, },
	.num_slaves = 8,
};

struct ds90ux9xx_i2c_bridged {
	struct device_node *np;
	struct i2c_client *i2c;
	u8 addr;
};

struct ds90ux9xx_i2c_map {
	u8 real_addr;
	u8 alias_addr;
};

struct ds90ux9xx_i2c_linked {
	struct ds90ux9xx_i2c_bridged remote;
	struct ds90ux9xx_i2c_bridged slave[DS90UX9XX_MAX_SLAVE_DEVICES];
	struct ds90ux9xx_i2c_map map[DS90UX9XX_MAX_SLAVE_DEVICES];
	unsigned int num_slaves;
	unsigned int num_maps;
	bool is_bridged;
};

struct ds90ux9xx_i2c {
	struct device *dev;
	struct regmap *regmap;
	struct i2c_adapter *adapter;
	const struct ds90ux9xx_i2c_data *i2c_data;
	bool remote;
	struct task_struct *conn_monitor;
	struct ds90ux9xx_i2c_linked linked[DS90UX9XX_MAX_LINKED_DEVICES];
	unsigned int link;
	bool pass_all;
};

static int ds90ux9xx_setup_i2c_pass_through(struct ds90ux9xx_i2c *i2c_bridge,
					    bool enable)
{
	int ret;

	ret = regmap_update_bits(i2c_bridge->regmap, SER_DES_REG_CONFIG,
				 CONFIG_I2C_PASS_THROUGH,
				 enable ? CONFIG_I2C_PASS_THROUGH : 0x0);
	if (ret)
		dev_err(i2c_bridge->dev, "Failed to setup pass through\n");

	return ret;
}

static int ds90ux9xx_setup_i2c_pass_all(struct ds90ux9xx_i2c *i2c_bridge,
					bool enable)
{
	unsigned int reg;
	int ret;

	if (ds90ux9xx_is_serializer(i2c_bridge->dev->parent))
		reg = SER_REG_I2C_CTRL;
	else
		reg = DES_REG_I2C_CTRL;

	ret = regmap_update_bits(i2c_bridge->regmap, reg, I2C_CTRL_PASS_ALL,
				 enable ? I2C_CTRL_PASS_ALL : 0x0);
	if (ret)
		dev_err(i2c_bridge->dev, "Failed to setup pass all mode\n");

	return ret;
}

static int ds90ux9xx_setup_auto_ack(struct ds90ux9xx_i2c *i2c_bridge,
				    bool enable)
{
	unsigned int val;
	int ret;

	if (ds90ux9xx_is_serializer(i2c_bridge->dev->parent))
		val = SER_CONFIG_I2C_AUTO_ACK;
	else
		val = DES_CONFIG_I2C_AUTO_ACK;

	ret = regmap_update_bits(i2c_bridge->regmap, SER_DES_REG_CONFIG, val,
				 enable ? val : 0x0);
	if (ret)
		dev_err(i2c_bridge->dev, "Failed to setup auto ack mode\n");

	return ret;
}

static int ds90ux9xx_setup_address_mapping(struct ds90ux9xx_i2c *i2c_bridge,
					   unsigned int i, u8 slave, u8 alias)
{
	const struct ds90ux9xx_i2c_data *i2c_data = i2c_bridge->i2c_data;
	int ret;

	if (i >= i2c_data->num_slaves)
		return -EINVAL;

	ret = regmap_write(i2c_bridge->regmap, i2c_data->slave_reg[i],
			   slave << 1);
	if (ret)
		return ret;

	return regmap_write(i2c_bridge->regmap, i2c_data->alias_reg[i],
			    alias << 1);
}

static void ds90ux9xx_clear_address_mappings(struct ds90ux9xx_i2c *i2c_bridge)
{
	unsigned int i;

	for (i = 0; i < i2c_bridge->i2c_data->num_slaves; i++)
		ds90ux9xx_setup_address_mapping(i2c_bridge, i, 0, 0);
}

static int ds90ux9xx_setup_address_mappings(struct ds90ux9xx_i2c *i2c_bridge,
					    struct ds90ux9xx_i2c_linked *linked)
{
	struct ds90ux9xx_i2c_map *map;
	unsigned int i;
	int ret;

	/* To avoid address collisions disable the remaining slave/aliases */
	for (i = 0; i < i2c_bridge->i2c_data->num_slaves; i++) {
		map = &linked->map[i];

		if (i < linked->num_maps)
			dev_dbg(i2c_bridge->dev, "Mapping remote slave %#x to %#x\n",
				map->real_addr, map->alias_addr);

		ret = ds90ux9xx_setup_address_mapping(i2c_bridge, i,
						      map->real_addr,
						      map->alias_addr);
		if (ret)
			return ret;
	}

	return 0;
}

static int ds90ux9xx_get_remote_addr(struct ds90ux9xx_i2c *i2c_bridge, u8 *addr)
{
	unsigned int reg, val;
	int ret;

	if (ds90ux9xx_is_serializer(i2c_bridge->dev->parent))
		reg = SER_REG_REMOTE_ID;
	else
		reg = DES_REG_REMOTE_ID;

	ret = regmap_read(i2c_bridge->regmap, reg, &val);
	if (ret)
		dev_err(i2c_bridge->dev, "Failed to get remote addr: %d\n",
			ret);
	else
		*addr = val >> 1;

	return ret;
}

static int ds90ux9xx_set_remote_addr(struct ds90ux9xx_i2c *i2c_bridge, u8 addr)
{
	u8 remote_addr, data[2] = { SER_DES_REG_DEVICE_ID,
				    (addr << 1) | DEVICE_ID_OVERRIDE };
	struct i2c_msg msg = {
		.addr	= addr,
		.flags	= 0x00,
		.len	= 2,
		.buf	= data,
	};
	int ret;

	ret = ds90ux9xx_get_remote_addr(i2c_bridge, &remote_addr);
	if (ret)
		return ret;

	if (remote_addr == addr)
		return 0;

	ret = ds90ux9xx_setup_address_mapping(i2c_bridge, 0, remote_addr, addr);
	if (ret)
		return ret;

	ret = i2c_transfer(i2c_bridge->adapter, &msg, 1);
	if (ret != 1)
		return ret ? ret : -EIO;

	dev_dbg(i2c_bridge->dev, "New address set for remote %#x\n", addr);

	return 0;
}

static void ds90ux9xx_remove_slave_devices(struct ds90ux9xx_i2c_linked *linked,
					   bool drop_reference)
{
	struct ds90ux9xx_i2c_bridged *slave;
	unsigned int i;

	if (!linked->is_bridged)
		return;

	for (i = 0; i < linked->num_slaves; i++) {
		slave = &linked->slave[i];

		if (slave->i2c) {
			i2c_unregister_device(slave->i2c);
			slave->i2c = NULL;
		}

		if (drop_reference && slave->np) {
			of_node_put(slave->np);
			slave->np = NULL;
		}
	}
}

static void ds90ux9xx_remove_linked_device(struct ds90ux9xx_i2c *i2c_bridge,
					   struct ds90ux9xx_i2c_linked *linked,
					   bool drop_reference)
{
	struct ds90ux9xx_i2c_bridged *remote = &linked->remote;

	ds90ux9xx_remove_slave_devices(linked, drop_reference);

	if (remote->i2c) {
		if (linked->is_bridged)
			i2c_unregister_device(remote->i2c);
		else
			put_device(&remote->i2c->dev);

		remote->i2c = NULL;
	}

	if (drop_reference && remote->np) {
		of_node_put(remote->np);
		remote->np = NULL;
	}

	ds90ux9xx_clear_address_mappings(i2c_bridge);
}

static void ds90ux9xx_remove_linked_devices(struct ds90ux9xx_i2c *i2c_bridge)
{
	struct ds90ux9xx_i2c_linked *linked;
	unsigned int i;

	for (i = 0; i < ds90ux9xx_num_fpd_links(i2c_bridge->dev->parent); i++) {
		linked = &i2c_bridge->linked[i];
		ds90ux9xx_remove_linked_device(i2c_bridge, linked, true);
	}
}

static void ds90ux9xx_add_bridged_device(struct ds90ux9xx_i2c *i2c_bridge,
					 struct ds90ux9xx_i2c_bridged *bridged)
{
	struct i2c_board_info info = {};
	int ret;

	dev_dbg(i2c_bridge->dev, "Add I2C device '%s'\n", bridged->np->name);

	info.addr = bridged->addr;
	info.of_node = bridged->np;

	/* Non-critical, in case of the problem report it and fallback */
	ret = of_modalias_node(bridged->np, info.type, sizeof(info.type));
	if (ret)
		dev_err(i2c_bridge->dev, "Cannot get module alias for '%s'\n",
			bridged->np->full_name);

	bridged->i2c = i2c_new_device(i2c_bridge->adapter, &info);
	if (!bridged->i2c)
		dev_err(i2c_bridge->dev, "Cannot add new I2C device\n");
}

static int ds90ux9xx_configure_remote_devices(struct ds90ux9xx_i2c *i2c_bridge,
					struct ds90ux9xx_i2c_linked *linked)
{
	struct ds90ux9xx_i2c_bridged *remote = &linked->remote, *slave;
	unsigned int i;
	int ret;

	ret = ds90ux9xx_setup_address_mappings(i2c_bridge, linked);
	if (ret)
		return ret;

	ds90ux9xx_add_bridged_device(i2c_bridge, remote);
	if (!remote->i2c)
		return -ENODEV;

	for (i = 0; i < linked->num_slaves; i++) {
		slave = &linked->slave[i];

		ds90ux9xx_add_bridged_device(i2c_bridge, slave);
		if (!slave->i2c) {
			ds90ux9xx_remove_linked_device(i2c_bridge,
						       linked, false);
			return -ENODEV;
		}
	}

	return 0;
}

static void ds90ux9xx_disconnect_remotes(struct ds90ux9xx_i2c *i2c_bridge)
{
	unsigned int link = i2c_bridge->link;

	dev_dbg(i2c_bridge->dev, "Link %d is disconnected\n", link);

	ds90ux9xx_remove_linked_device(i2c_bridge,
				       &i2c_bridge->linked[link], false);

	ds90ux9xx_setup_i2c_pass_through(i2c_bridge, false);
}

static int ds90ux9xx_connect_remotes(struct ds90ux9xx_i2c *i2c_bridge,
				     unsigned int link)
{
	struct ds90ux9xx_i2c_linked *linked = &i2c_bridge->linked[link];
	struct ds90ux9xx_i2c_bridged *remote = &linked->remote;
	int ret;

	dev_dbg(i2c_bridge->dev, "Link %d is connected\n", link);

	i2c_bridge->link = link;

	ret = ds90ux9xx_setup_i2c_pass_through(i2c_bridge, linked->is_bridged);
	if (ret)
		return ret;

	if (!linked->is_bridged) {
		remote->i2c = of_find_i2c_device_by_node(remote->np);
		return remote->i2c ? 0 : -ENODEV;
	}

	ret = ds90ux9xx_set_remote_addr(i2c_bridge, remote->addr);
	if (ret)
		return ret;

	return ds90ux9xx_configure_remote_devices(i2c_bridge, linked);
}

static int ds90ux9xx_conn_monitor(void *data)
{
	unsigned int link, sleep_time = CONN_MIN_TIME_MSEC;
	struct ds90ux9xx_i2c *i2c_bridge = data;
	struct ds90ux9xx_i2c_bridged *remote;
	struct ds90ux9xx_i2c_linked *linked;
	bool lock;
	u8 addr;
	int ret;

	while (!kthread_should_stop()) {
		ret = ds90ux9xx_get_link_status(i2c_bridge->dev->parent,
						&link, &lock);
		if (ret)
			goto sleep;

		linked = &i2c_bridge->linked[i2c_bridge->link];
		remote = &linked->remote;

		ret = ds90ux9xx_get_remote_addr(i2c_bridge, &addr);
		if (ret < 0)
			goto sleep;

		lock = lock && (addr != 0);

		if (lock)
			sleep_time = CONN_MAX_TIME_MSEC;
		else if (remote->i2c)
			sleep_time = CONN_MIN_TIME_MSEC;
		else
			sleep_time = min_t(unsigned int, CONN_MAX_TIME_MSEC,
					   sleep_time + CONN_STEP_TIME_MSEC);

		if (remote->i2c && lock && i2c_bridge->link == link) {
			if (!linked->is_bridged)
				goto sleep;

			if (remote->addr == addr)
				goto sleep;
		}

		if (remote->i2c)
			ds90ux9xx_disconnect_remotes(i2c_bridge);

		if (!remote->i2c && lock) {
			ret = ds90ux9xx_connect_remotes(i2c_bridge, link);
			if (ret < 0)
				dev_err(i2c_bridge->dev,
					"Can't establish connection\n");
		}
sleep:
		msleep(sleep_time);
	}

	return 0;
}

static int ds90ux9xx_parse_address_mappings(struct ds90ux9xx_i2c *i2c_bridge)
{
	const struct ds90ux9xx_i2c_data *i2c_data = i2c_bridge->i2c_data;
	struct ds90ux9xx_i2c_linked *remote;
	u32 link, real_addr, alias_addr;
	unsigned int size, i;
	const __be32 *list;

	list = of_get_property(i2c_bridge->dev->of_node, "ti,i2c-bridge-maps",
			       &size);
	if (!list)
		return 0;

	if (!size || size % 12) {
		dev_err(i2c_bridge->dev, "Failed to get valid alias maps\n");
		return -EINVAL;
	}

	for (i = 0; i < size / 12; i++) {
		link = be32_to_cpu(*list++);
		real_addr = be32_to_cpu(*list++);
		alias_addr = be32_to_cpu(*list++);

		if (link >= ds90ux9xx_num_fpd_links(i2c_bridge->dev->parent)) {
			dev_info(i2c_bridge->dev, "Invalid link id %d\n", i);
			continue;
		}

		remote = &i2c_bridge->linked[link];
		if (remote->num_maps >= i2c_data->num_slaves) {
			dev_info(i2c_bridge->dev, "Too many aliases\n");
			break;
		}

		remote->map[remote->num_maps].real_addr = real_addr;
		remote->map[remote->num_maps].alias_addr = alias_addr;
		remote->num_maps++;
	}

	return 0;
}

static u8 ds90ux9xx_get_mapped_address(struct ds90ux9xx_i2c_linked *linked,
				       u8 remote_addr)
{
	unsigned int i;

	for (i = 0; i < linked->num_maps; i++) {
		if (linked->map[i].real_addr == remote_addr)
			return linked->map[i].alias_addr;
	}

	/* Fallback to preset address, remote device may be inaccessible */
	return remote_addr;
}

static int ds90ux9xx_parse_remote_slaves(struct ds90ux9xx_i2c *i2c_bridge,
					 struct ds90ux9xx_i2c_linked *linked)
{
	struct ds90ux9xx_i2c_bridged *slave;
	struct device_node *child, *np;
	u32 addr;

	np = of_get_child_by_name(linked->remote.np, "i2c-bridge");
	if (!np) {
		dev_info(i2c_bridge->dev, "I2C bridge device node not found\n");
		return 0;
	}

	if (of_get_child_count(np) > DS90UX9XX_MAX_SLAVE_DEVICES) {
		dev_err(i2c_bridge->dev, "Too many aliased I2C devices\n");
		of_node_put(np);
		return -EINVAL;
	}

	for_each_child_of_node(np, child) {
		if (of_property_read_u32(child, "reg", &addr)) {
			dev_err(i2c_bridge->dev, "No I2C device address '%s'\n",
				child->full_name);
			/* Try the next one */
			continue;
		}

		slave = &linked->slave[linked->num_slaves];
		slave->np = of_node_get(child);
		if (i2c_bridge->pass_all)
			slave->addr = addr;
		else
			slave->addr = ds90ux9xx_get_mapped_address(linked,
								   addr);

		linked->num_slaves++;
	}

	of_node_put(np);

	return 0;
}

static int ds90ux9xx_configure_link(struct ds90ux9xx_i2c *i2c_bridge)
{
	struct device_node *np = i2c_bridge->dev->parent->of_node;
	struct i2c_client *client;
	int ret;

	/* The link value is updated when established connection is detected */
	i2c_bridge->link = 0;

	client = of_find_i2c_device_by_node(np);
	if (!client || !client->adapter)
		return -ENODEV;

	i2c_bridge->adapter = client->adapter;
	put_device(&client->dev);

	ret = ds90ux9xx_setup_i2c_pass_all(i2c_bridge, i2c_bridge->pass_all);
	if (ret)
		return ret;

	i2c_bridge->conn_monitor = kthread_run(ds90ux9xx_conn_monitor,
					       i2c_bridge,
					       "ds90ux9xx_conn_monitor");
	if (IS_ERR(i2c_bridge->conn_monitor))
		return PTR_ERR(i2c_bridge->conn_monitor);

	return 0;
}

static int ds90ux9xx_set_link_attributes(struct ds90ux9xx_i2c *i2c_bridge)
{
	struct device_node *np = i2c_bridge->dev->of_node;
	struct ds90ux9xx_i2c_linked *linked;
	struct of_phandle_args remote;
	unsigned int link, num_links, i;
	bool auto_ack;
	int ret;

	if (of_property_read_bool(np, "ti,i2c-bridge-pass-all"))
		i2c_bridge->pass_all = true;

	auto_ack = of_property_read_bool(np, "ti,i2c-bridge-auto-ack");
	ret = ds90ux9xx_setup_auto_ack(i2c_bridge, auto_ack);
	if (ret)
		return ret;

	ret = ds90ux9xx_parse_address_mappings(i2c_bridge);
	if (ret)
		return ret;

	num_links = ds90ux9xx_num_fpd_links(i2c_bridge->dev->parent);

	for (i = 0; i < num_links; i++) {
		ret = of_parse_phandle_with_fixed_args(np, "ti,i2c-bridges",
						       2, i, &remote);
		if (ret) {
			if (ret == -ENOENT)
				break;
			goto drop_nodes;
		}

		link = remote.args[0];
		if (link >= num_links) {
			ret = -EINVAL;
			goto drop_nodes;
		}

		linked = &i2c_bridge->linked[link];
		if (linked->remote.np) {
			ret = -EINVAL;
			goto drop_nodes;
		}

		linked->remote.np = remote.np;
		linked->remote.addr = remote.args[1];

		/*
		 * Don't open I2C access over the FPD-link bidirectional channel
		 * to the remote's slave devices, if the remote is an I2C slave
		 * attached to a local bus, because the remote's slaves would
		 * also necessarily have to hang off the same local bus.
		 * Enabling pass-through in this case will cause I2C collisions
		 * due to multiple routes to the same device.
		 */
		if (of_property_read_bool(linked->remote.np, "reg")) {
			linked->is_bridged = false;
			continue;
		}

		linked->is_bridged = true;

		ret = ds90ux9xx_parse_remote_slaves(i2c_bridge, linked);
		if (ret)
			goto drop_nodes;
	}

	ret = ds90ux9xx_configure_link(i2c_bridge);
	if (ret)
		goto drop_nodes;

	return 0;

drop_nodes:
	ds90ux9xx_remove_linked_devices(i2c_bridge);

	return ret;
}

static void ds90ux9xx_get_i2c_data(struct ds90ux9xx_i2c *i2c_bridge)
{
	enum ds90ux9xx_device_id id =
		ds90ux9xx_get_ic_type(i2c_bridge->dev->parent);

	switch (id) {
	case TI_DS90UB925:
	case TI_DS90UH925:
		i2c_bridge->i2c_data = &ds90ux925_i2c;
		break;
	case TI_DS90UB927:
	case TI_DS90UH927:
		i2c_bridge->i2c_data = &ds90ux927_i2c;
		break;
	case TI_DS90UB926:
	case TI_DS90UH926:
	case TI_DS90UB928:
	case TI_DS90UH928:
	case TI_DS90UB940:
	case TI_DS90UH940:
		i2c_bridge->i2c_data = &ds90ux926_i2c;
		break;
	default:
		dev_err(i2c_bridge->dev, "Not supported hardware: [%d]\n", id);
	}
}

static int ds90ux9xx_i2c_bridge_probe(struct platform_device *pdev)
{
	struct ds90ux9xx_i2c *i2c_bridge;
	struct device *dev = &pdev->dev;

	i2c_bridge = devm_kzalloc(dev, sizeof(*i2c_bridge), GFP_KERNEL);
	if (!i2c_bridge)
		return -ENOMEM;

	i2c_bridge->dev = dev;
	i2c_bridge->regmap = dev_get_regmap(dev->parent, NULL);
	if (!i2c_bridge->regmap)
		return -ENODEV;

	i2c_bridge->i2c_data = of_device_get_match_data(dev);
	if (!i2c_bridge->i2c_data)
		ds90ux9xx_get_i2c_data(i2c_bridge);

	if (!i2c_bridge->i2c_data)
		return -ENODEV;

	platform_set_drvdata(pdev, i2c_bridge);

	if (of_property_read_bool(dev->of_node, "ti,i2c-bridges"))
		return ds90ux9xx_set_link_attributes(i2c_bridge);

	i2c_bridge->remote = true;

	return ds90ux9xx_setup_i2c_pass_through(i2c_bridge, false);
}

static int ds90ux9xx_i2c_bridge_remove(struct platform_device *pdev)
{
	struct ds90ux9xx_i2c *i2c_bridge = platform_get_drvdata(pdev);

	if (i2c_bridge->remote)
		return 0;

	kthread_stop(i2c_bridge->conn_monitor);
	ds90ux9xx_remove_linked_devices(i2c_bridge);

	return 0;
}

static const struct of_device_id ds90ux9xx_i2c_dt_ids[] = {
	{ .compatible = "ti,ds90ux9xx-i2c-bridge", },
	{ .compatible = "ti,ds90ux925-i2c-bridge", .data = &ds90ux925_i2c, },
	{ .compatible = "ti,ds90ux926-i2c-bridge", .data = &ds90ux926_i2c, },
	{ .compatible = "ti,ds90ux927-i2c-bridge", .data = &ds90ux927_i2c, },
	{ .compatible = "ti,ds90ux928-i2c-bridge", .data = &ds90ux926_i2c, },
	{ .compatible = "ti,ds90ux940-i2c-bridge", .data = &ds90ux926_i2c, },
	{ },
};
MODULE_DEVICE_TABLE(of, ds90ux9xx_i2c_dt_ids);

static struct platform_driver ds90ux9xx_i2c_bridge_driver = {
	.probe = ds90ux9xx_i2c_bridge_probe,
	.remove = ds90ux9xx_i2c_bridge_remove,
	.driver = {
		.name = "ds90ux9xx-i2c-bridge",
		.of_match_table = ds90ux9xx_i2c_dt_ids,
	},
};
module_platform_driver(ds90ux9xx_i2c_bridge_driver);

MODULE_AUTHOR("Vladimir Zapolskiy <vladimir_zapolskiy@mentor.com>");
MODULE_AUTHOR("Balasubramani Vivekanandan <balasubramani_vivekanandan@mentor.com>");
MODULE_DESCRIPTION("TI DS90Ux9xx I2C bridge/alias controller driver");
MODULE_LICENSE("GPL");
