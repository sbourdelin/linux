/*
 * drivers/i2c/muxes/i2c-mux-mlxcpld.c
 * Copyright (c) 2016 Mellanox Technologies. All rights reserved.
 * Copyright (c) 2016 Michael Shych <michaels@mellanox.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the names of the copyright holders nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 * Alternatively, this software may be distributed under the terms of the
 * GNU General Public License ("GPL") version 2 as published by the Free
 * Software Foundation.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <linux/module.h>
#include <linux/version.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/device.h>
#include <linux/i2c.h>
#include <linux/i2c-mux.h>
#include <linux/io.h>
#include <linux/platform_device.h>
#include <linux/i2c/mlxcpld.h>

#define CPLD_MUX_MAX_NCHANS	8
#define CPLD_MUX_EXT_MAX_NCHANS	24

/*
 * mlxcpld_mux types - kind of mux supported by driver:
 * @mlxcpld_mux_tor - LPC access; 8 channels/legs; select/deselect -
 *		      channel=first defined channel(2/10) + channel/leg
 * @mlxcpld_mux_mgmt - LPC access; 8 channels/legs; select/deselect  -
 *		       channel=1 + channel/leg
 * @mlxcpld_mux_mgmt_ext - LPC access; 24 channels/legs; select/deselect -
 *			   channel=1 + channel/leg
 * @mlxcpld_mux_module - I2C access; 8 channels/legs; select/deselect  -
 *			 channel=1 + channel/leg
 */
enum mlxcpld_mux_type {
	mlxcpld_mux_tor,
	mlxcpld_mux_mgmt,
	mlxcpld_mux_mgmt_ext,
	mlxcpld_mux_module,
};

/* mlxcpld_mux_type - underlying physical bus, to which device is connected:
 * @lpc_access - LPC connected CPLD device
 * @i2c_access - I2C connected CPLD device
 */
enum mlxcpld_mux_access_type {
	lpc_access,
	i2c_access,
};

/* mlxcpld_mux - mux control structure:
 * @type - mux type
 * @last_chan - last register value
 * @client - I2C device client
 */
struct mlxcpld_mux {
	enum mlxcpld_mux_type type;
	u8 last_chan;
	struct i2c_client *client;
};

/* mlxcpld_mux_desc - mux descriptor structure:
 * @nchans - number of channels
 * @muxtype - physical mux type (LPC or I2C)
 */
struct mlxcpld_mux_desc {
	u8 nchans;
	enum mlxcpld_mux_access_type muxtype;
};

/* MUX logic description.
 * Mux selector can control 256 mux (channels), if utilized one CPLD register
 * (8 bits) as select register - register value specifies mux id.
 * Mux selector can control n*256 mux, if utilized n CPLD registers as select
 * registers.
 * The number of registers within the same CPLD can be combined to support
 * mux hierarchy.
 * This logic can be applied for LPC attached CPLD and fro I2C attached CPLD.
 * Driver can support different mux control logic, according to CPLD
 * implementation.
 *
 * Connectivity schema.
 *
 * i2c-mlxcpld                                 Digital               Analog
 * driver
 * *--------*                                 * -> mux1 (virt bus2) -> mux -> |
 * | I2CLPC | i2c physical                    * -> mux2 (virt bus3) -> mux -> |
 * | bridge | bus 1                 *---------*                               |
 * | logic  |---------------------> * mux reg *                               |
 * | in CPLD|                       *---------*                               |
 * *--------*   i2c-mux-mlxpcld          ^    * -> muxn (virt busn) -> mux -> |
 *     |        driver                   |                                    |
 *     |        *---------------*        |                              Devices
 *     |        * CPLD (LPC bus)* select |
 *     |        * registers for *--------*
 *     |        * mux selection * deselect
 *     |        *---------------*
 *     |                 |
 * <-------->     <----------->
 * i2c cntrl      Board cntrl reg
 * reg space      space (mux select,
 *     |          IO, LED, WD, info)
 *     |                 |                  *-----*   *-----*
 *     *------------- LPC bus --------------| PCH |---| CPU |
 *                                          *-----*   *-----*
 *
 * i2c-mux-mlxpcld does not necessary required i2c-mlxcpld. It can be use along
 * with another bus driver, and still control i2c routing through CPLD mux
 * selection, in case the system is equipped with CPLD capable of mux selection
 * control.
 */
static const struct mlxcpld_mux_desc muxes[] = {
	[mlxcpld_mux_tor] = {
		.nchans = CPLD_MUX_MAX_NCHANS,
		.muxtype = lpc_access,
	},
	[mlxcpld_mux_mgmt] = {
		.nchans = CPLD_MUX_MAX_NCHANS,
		.muxtype = lpc_access,
	},
	[mlxcpld_mux_mgmt_ext] = {
		.nchans = CPLD_MUX_EXT_MAX_NCHANS,
		.muxtype = lpc_access,
	},
	[mlxcpld_mux_module] = {
		.nchans = CPLD_MUX_MAX_NCHANS,
		.muxtype = i2c_access,
	},
};

static const struct i2c_device_id mlxcpld_mux_id[] = {
	{ "mlxcpld_mux_tor", mlxcpld_mux_tor },
	{ "mlxcpld_mux_mgmt", mlxcpld_mux_mgmt },
	{ "mlxcpld_mux_mgmt_ext", mlxcpld_mux_mgmt_ext },
	{ "mlxcpld_mux_module", mlxcpld_mux_module },
	{ }
};
MODULE_DEVICE_TABLE(i2c, mlxcpld_mux_id);

/* Write to mux register. Don't use i2c_transfer() and
 * i2c_smbus_xfer() for this as they will try to lock adapter a second time
 */
static int mlxcpld_mux_reg_write(struct i2c_adapter *adap,
				 struct i2c_client *client, u8 val,
				 enum mlxcpld_mux_access_type muxtype)
{
	struct mlxcpld_mux_platform_data *pdata =
						dev_get_platdata(&client->dev);
	int ret = -ENODEV;

	switch (muxtype) {
	case lpc_access:
		outb(val, pdata->addr); /* addr = CPLD base + offset */
		ret = 1;
		break;

	case i2c_access:
		if (adap->algo->master_xfer) {
			struct i2c_msg msg;
			u8 msgbuf[] = {pdata->sel_reg_addr, val};

			msg.addr = pdata->addr;
			msg.flags = 0;
			msg.len = 2;
			msg.buf = msgbuf;
			return adap->algo->master_xfer(adap, &msg, 1);
		}
		dev_err(&client->dev, "SMBus isn't supported on this adapter\n");
		break;

	default:
		dev_err(&client->dev, "Incorrect muxtype %d\n", muxtype);
	}

	return ret;
}

static int mlxcpld_mux_select_chan(struct i2c_mux_core *muxc, u32 chan)
{
	struct mlxcpld_mux *data = i2c_mux_priv(muxc);
	struct i2c_client *client = data->client;
	struct mlxcpld_mux_platform_data *pdata =
						dev_get_platdata(&client->dev);
	const struct mlxcpld_mux_desc *mux = &muxes[data->type];
	u8 regval;
	int err;

	switch (data->type) {
	case mlxcpld_mux_tor:
		regval = pdata->first_channel + chan;
		break;

	case mlxcpld_mux_mgmt:
	case mlxcpld_mux_mgmt_ext:
	case mlxcpld_mux_module:
		regval = chan + 1;
		break;

	default:
		return -ENXIO;
	}

	/* Only select the channel if its different from the last channel */
	if (data->last_chan != regval) {
		err = mlxcpld_mux_reg_write(muxc->parent, client, regval,
					    mux->muxtype);
		data->last_chan = regval;
	}

	return 0;
}

static int mlxcpld_mux_deselect(struct i2c_mux_core *muxc, u32 chan)
{
	struct mlxcpld_mux *data = i2c_mux_priv(muxc);
	struct i2c_client *client = data->client;
	const struct mlxcpld_mux_desc *mux = &muxes[data->type];

	/* Deselect active channel */
	data->last_chan = 0;

	return mlxcpld_mux_reg_write(muxc->parent, client, data->last_chan,
				     mux->muxtype);
}

/* I2C init/probing/exit functions */
static int mlxcpld_mux_probe(struct i2c_client *client,
			     const struct i2c_device_id *id)
{
	struct i2c_adapter *adap = to_i2c_adapter(client->dev.parent);
	struct mlxcpld_mux_platform_data *pdata =
						dev_get_platdata(&client->dev);
	struct i2c_mux_core *muxc;
	int num, force;
	u8 nchans;
	struct mlxcpld_mux *data;
	int err;

	if (!i2c_check_functionality(adap, I2C_FUNC_SMBUS_BYTE))
		return -ENODEV;

	switch (id->driver_data) {
	case mlxcpld_mux_tor:
	case mlxcpld_mux_mgmt:
	case mlxcpld_mux_mgmt_ext:
	case mlxcpld_mux_module:
		nchans = muxes[id->driver_data].nchans;
		break;
	default:
		return -EINVAL;
	}

	muxc = i2c_mux_alloc(adap, &client->dev, nchans, sizeof(*data), 0,
			     mlxcpld_mux_select_chan, mlxcpld_mux_deselect);
	if (!muxc)
		return -ENOMEM;

	data = i2c_mux_priv(muxc);
	i2c_set_clientdata(client, muxc);
	data->client = client;
	data->type = id->driver_data;
	data->last_chan = 0; /* force the first selection */

	/* Only in mlxcpld_mux_tor first_channel can be different.
	 * In other mlxcpld_mux types channel numbering begin from 1
	 * Now create an adapter for each channel
	 */
	for (num = 0; num < muxes[data->type].nchans; num++) {
		force = 0; /* dynamic adap number */
		if (pdata) {
			if (num < pdata->num_modes)
				force = pdata->first_channel + num;
			else
				/* discard unconfigured channels */
				break;
		}

		err = i2c_mux_add_adapter(muxc, force, num, 0);
		if (err) {
			dev_err(&client->dev, "failed to register multiplexed adapter %d as bus %d\n",
				num, force);
			goto virt_reg_failed;
		}
	}

	return 0;

virt_reg_failed:
	i2c_mux_del_adapters(muxc);
	return err;
}

static int mlxcpld_mux_remove(struct i2c_client *client)
{
	struct i2c_mux_core *muxc = i2c_get_clientdata(client);

	i2c_mux_del_adapters(muxc);
	return 0;
}

static struct i2c_driver mlxcpld_mux_driver = {
	.driver		= {
		.name	= "mlxcpld-mux",
		.owner	= THIS_MODULE,
	},
	.probe		= mlxcpld_mux_probe,
	.remove		= mlxcpld_mux_remove,
	.id_table	= mlxcpld_mux_id,
};

static int __init mlxcpld_mux_init(void)
{
	return i2c_add_driver(&mlxcpld_mux_driver);
}

static void __exit mlxcpld_mux_exit(void)
{
	i2c_del_driver(&mlxcpld_mux_driver);
}

module_init(mlxcpld_mux_init);
module_exit(mlxcpld_mux_exit);

MODULE_AUTHOR("Michael Shych (michaels@mellanox.com)");
MODULE_DESCRIPTION("Mellanox I2C-CPLD-MUX driver");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:i2c-mux-mlxcpld");
