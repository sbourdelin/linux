/*
 * PTX PMB CPLD I2C multiplexer
 *
 * Copyright (c) 2012, Juniper Networks.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/i2c-mux.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/mfd/ptxpmb_cpld.h>
#include <linux/io.h>
#include <linux/of_device.h>

struct i2c_mux_ptxpmb {
	struct device *dev;
	struct ptxpmb_mux_data *pdata;
	struct pmb_boot_cpld __iomem *cpld;
	struct i2c_adapter *parent;
	int bus_count;
	struct i2c_mux_core *muxc;
};

static const struct of_device_id i2c_mux_ptxpmb_of_match[] = {
	{ .compatible = "jnx,i2c-mux-ptxpmb-cpld",
	  .data = (void *)CPLD_TYPE_PTXPMB },
	{ .compatible = "jnx,i2c-mux-ngpmb-bcpld",
	  .data = (void *)CPLD_TYPE_NGPMB },
	{ },
};
MODULE_DEVICE_TABLE(of, i2c_mux_ptxpmb_of_match);

#define I2C_GRP_FORCE_EN	0x80

static int i2c_mux_ptxpmb_select(struct i2c_mux_core *muxc, u32 chan)
{
	struct i2c_mux_ptxpmb *mux = i2c_mux_priv(muxc);
	struct ptxpmb_mux_data *pdata = mux->pdata;
	u8 group, enable, val;

	switch (pdata->cpld_type) {
	case CPLD_TYPE_PTXPMB:
		group = chan % pdata->num_channels;
		enable = 1 << (chan / pdata->num_channels);
		/*
		 * Writing into the enable register does not have an effect on
		 * FPC with P2020. It is necessary for FPC with P5020/P5040.
		 * The uKernel for SPMB uses undocumented CPLD registers to set
		 * group enable values (i2c_group_sel_force and
		 * i2c_group_en_force at offset 0x33 and 0x34). Bit 7 in
		 * i2c_group_sel_force must be set for this to work.
		 * i2c_group_en_force is active-low. This applies to SPMB with
		 * P2020; behavior with P5020/P5040 is unknown at this time.
		 */
		if (pdata->use_force) {
			iowrite8(group | I2C_GRP_FORCE_EN,
				 &mux->cpld->i2c_group_sel_force);
			iowrite8(~enable, &mux->cpld->i2c_group_en_force);
		} else {
			iowrite8(group, &mux->cpld->i2c_group_sel);
			ioread8(&mux->cpld->i2c_group_sel);
			iowrite8(enable, &mux->cpld->i2c_group_en);
			ioread8(&mux->cpld->i2c_group_en);
		}
		break;
	case CPLD_TYPE_NGPMB:
		val = ioread8(&mux->cpld->gpio_2);
		val &= ~NGPMB_I2C_GRP_SEL_MASK;
		val |= (chan << NGPMB_I2C_GRP_SEL_LSB) & NGPMB_I2C_GRP_SEL_MASK;
		iowrite8(val, &mux->cpld->gpio_2);
		break;
	}
	udelay(50);

	return 0;
}

static int i2c_mux_ptxpmb_deselect(struct i2c_mux_core *muxc, u32 chan)
{
	struct i2c_mux_ptxpmb *mux = i2c_mux_priv(muxc);
	u8 val;

	switch (mux->pdata->cpld_type) {
	case CPLD_TYPE_PTXPMB:
		/*
		 * Restore defaults. Note that setting i2c_group_en does not
		 * have an effect on FPC with P2020, but is necessary for FPC
		 * with P5020/P5040.
		 */
		if (mux->pdata->use_force) {
			iowrite8(0 | I2C_GRP_FORCE_EN,
				 &mux->cpld->i2c_group_sel_force);
			iowrite8(0xff, &mux->cpld->i2c_group_en_force);
		} else {
			iowrite8(0, &mux->cpld->i2c_group_sel);
			ioread8(&mux->cpld->i2c_group_sel);
			iowrite8(0, &mux->cpld->i2c_group_en);
			ioread8(&mux->cpld->i2c_group_en);
		}
		break;
	case CPLD_TYPE_NGPMB:
		/* Use the (unconnected) channel 3 to deselct */
		val = ioread8(&mux->cpld->gpio_2);
		val &= ~NGPMB_I2C_GRP_SEL_MASK;
		val |= (3 << NGPMB_I2C_GRP_SEL_LSB) & NGPMB_I2C_GRP_SEL_MASK;
		iowrite8(val, &mux->cpld->gpio_2);
		break;
	}
	return 0;
}

#ifdef CONFIG_OF
static int i2c_mux_ptxpmb_parse_dt(struct i2c_mux_ptxpmb *mux,
				   struct device *dev)
{
	struct device_node *np = dev->of_node;
	int ret;
	struct device_node *adapter_np;
	struct i2c_adapter *adapter;
	const struct of_device_id *match;

	if (!np)
		return 0;

	mux->pdata = devm_kzalloc(dev, sizeof(*mux->pdata), GFP_KERNEL);
	if (!mux->pdata)
		return -ENOMEM;

	match = of_match_device(i2c_mux_ptxpmb_of_match, dev);
	if (match)
		mux->pdata->cpld_type = (int)(unsigned long)match->data;

	ret = of_property_read_u32(np, "num-enable", &mux->pdata->num_enable);
	if (ret) {
		dev_err(dev, "num-enable missing\n");
		return -ENODEV;
	}

	ret = of_property_read_u32(np, "num-channels",
				   &mux->pdata->num_channels);
	if (ret)
		mux->pdata->num_channels = 8;

	ret = of_property_read_u32(np, "base-bus-num",
				   &mux->pdata->base_bus_num);
	if (ret)
		mux->pdata->base_bus_num = 0;

	if (of_find_property(np, "use-force", NULL))
		mux->pdata->use_force = true;

	adapter_np = of_parse_phandle(np, "i2c-parent", 0);
	if (!adapter_np) {
		dev_err(dev, "Cannot parse i2c-parent\n");
		return -ENODEV;
	}
	adapter = of_find_i2c_adapter_by_node(adapter_np);
	if (!adapter) {
		dev_err(dev, "Cannot find parent bus\n");
		return -ENODEV;
	}
	mux->pdata->parent_bus_num = i2c_adapter_id(adapter);
	put_device(&adapter->dev);

	return 0;
}
#else
static inline int i2c_mux_ptxpmb_parse_dt(struct i2c_mux_ptxpmb *mux,
					  struct device *dev)
{
	return 0;
}
#endif

static int i2c_mux_ptxpmb_probe(struct platform_device *pdev)
{
	struct i2c_mux_ptxpmb *mux;
	struct i2c_mux_core *muxc;
	int i, ret;
	struct resource *res;
	struct device *dev = &pdev->dev;

	mux = devm_kzalloc(dev, sizeof(*mux), GFP_KERNEL);
	if (!mux)
		return -ENOMEM;

	platform_set_drvdata(pdev, mux);

	mux->dev = dev;

	mux->pdata = dev->platform_data;
	if (!mux->pdata) {
		ret = i2c_mux_ptxpmb_parse_dt(mux, dev);
		if (ret < 0)
			return ret;
	}
	if (!mux->pdata) {
		dev_err(dev, "No platform / devicetree data\n");
		return -ENODEV;
	}

	if (mux->pdata->num_enable <= 0 || mux->pdata->num_enable > 8 ||
	    mux->pdata->num_channels <= 0 || mux->pdata->num_channels > 8) {
		dev_err(dev, "Invalid platform data\n");
		return -EINVAL;
	}

	mux->bus_count = mux->pdata->num_enable * mux->pdata->num_channels;

	mux->parent = i2c_get_adapter(mux->pdata->parent_bus_num);
	if (!mux->parent) {
		dev_err(dev, "Parent adapter (%d) not found\n",
			mux->pdata->parent_bus_num);
		return -ENODEV;
	}

	muxc = i2c_mux_alloc(mux->parent, dev, mux->bus_count, 0, 0,
			     i2c_mux_ptxpmb_select, i2c_mux_ptxpmb_deselect);
	if (!muxc) {
		ret =  -ENOMEM;
		goto err;
	}
	muxc->priv = mux;
	mux->muxc = muxc;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		dev_err(dev, "No memory resource\n");
		ret = -ENODEV;
		goto err;
	}

	mux->cpld = devm_ioremap_nocache(dev, res->start, resource_size(res));
	if (!mux->cpld) {
		ret = -ENOMEM;
		goto err;
	}

	for (i = 0; i < mux->bus_count; i++) {
		u32 bus = mux->pdata->base_bus_num ?
			mux->pdata->base_bus_num + i : 0;

		ret = i2c_mux_add_adapter(muxc, bus, i, 0);
		if (ret) {
			dev_err(dev, "Failed to add adapter %d\n", i);
			goto err_del_adapter;
		}
	}

	return 0;

err_del_adapter:
	i2c_mux_del_adapters(mux->muxc);
err:
	i2c_put_adapter(mux->parent);
	return ret;
}

static int i2c_mux_ptxpmb_remove(struct platform_device *pdev)
{
	struct i2c_mux_ptxpmb *mux = platform_get_drvdata(pdev);

	i2c_mux_del_adapters(mux->muxc);
	i2c_put_adapter(mux->parent);

	return 0;
}

static struct platform_driver i2c_mux_ptxpmb_driver = {
	.driver	= {
		.name	= "i2c-mux-ptxpmb-cpld",
		.owner	= THIS_MODULE,
		.of_match_table = i2c_mux_ptxpmb_of_match,
	},
	.probe	= i2c_mux_ptxpmb_probe,
	.remove	= i2c_mux_ptxpmb_remove,
};

module_platform_driver(i2c_mux_ptxpmb_driver);

MODULE_DESCRIPTION("ptxpmb CPLD I2C multiplexer driver");
MODULE_AUTHOR("Guenter Roeck <groeck@juniper.net>");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:i2c-mux-ptxpmb-cpld");
