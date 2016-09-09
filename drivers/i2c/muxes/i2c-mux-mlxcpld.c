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

#include <linux/device.h>
#include <linux/i2c.h>
#include <linux/i2c-mux.h>
#include <linux/io.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/version.h>
#include <linux/i2c/mlxcpld.h>

#define CPLD_MUX_MAX_NCHANS	8

/*
 * mlxcpld_mux types - kind of mux supported by driver:
 * @mlxcpld_mux_module - I2C access; 8 channels/legs.
 */
enum mlxcpld_mux_type {
	mlxcpld_mux_module,
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
 */
struct mlxcpld_mux_desc {
	u8 nchans;
};

/* MUX logic description.
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
 *     |        * CPLD (i2c bus)* select |
 *     |        * registers for *--------*
 *     |        * mux selection * deselect
 *     |        *---------------*
 *     |                 |
 * <-------->     <----------->
 * i2c cntrl      Board cntrl reg
 * reg space      space (mux select,
 *                IO, LED, WD, info)
 *
 */
static const struct mlxcpld_mux_desc muxes[] = {
	[mlxcpld_mux_module] = {
		.nchans = CPLD_MUX_MAX_NCHANS,
	},
};

static const struct i2c_device_id mlxcpld_mux_id[] = {
	{ "mlxcpld_mux_module", mlxcpld_mux_module },
	{ }
};
MODULE_DEVICE_TABLE(i2c, mlxcpld_mux_id);

/* Write to mux register. Don't use i2c_transfer() and
 * i2c_smbus_xfer() for this as they will try to lock adapter a second time
 */
static int mlxcpld_mux_reg_write(struct i2c_adapter *adap,
				 struct i2c_client *client, u8 val)
{
	struct mlxcpld_mux_plat_data *pdata = dev_get_platdata(&client->dev);

	if (adap->algo->master_xfer) {
		struct i2c_msg msg;
		u8 msgbuf[] = {pdata->sel_reg_addr, val};

		msg.addr = pdata->addr;
		msg.flags = 0;
		msg.len = 2;
		msg.buf = msgbuf;
		return __i2c_transfer(adap, &msg, 1);
	}
	dev_err(&client->dev, "SMBus isn't supported on this adapter\n");

	return -ENODEV;
}

static int mlxcpld_mux_select_chan(struct i2c_mux_core *muxc, u32 chan)
{
	struct mlxcpld_mux *data = i2c_mux_priv(muxc);
	struct i2c_client *client = data->client;
	u8 regval;
	int err = 0;

	if (data->type == mlxcpld_mux_module)
		regval = chan + 1;
	else
		return -ENXIO;

	/* Only select the channel if its different from the last channel */
	if (data->last_chan != regval) {
		err = mlxcpld_mux_reg_write(muxc->parent, client, regval);
		if (err)
			data->last_chan = 0;
		else
			data->last_chan = regval;
	}

	return err;
}

static int mlxcpld_mux_deselect(struct i2c_mux_core *muxc, u32 chan)
{
	struct mlxcpld_mux *data = i2c_mux_priv(muxc);
	struct i2c_client *client = data->client;

	/* Deselect active channel */
	data->last_chan = 0;

	return mlxcpld_mux_reg_write(muxc->parent, client, data->last_chan);
}

/* I2C init/probing/exit functions */
static int mlxcpld_mux_probe(struct i2c_client *client,
			     const struct i2c_device_id *id)
{
	struct i2c_adapter *adap = to_i2c_adapter(client->dev.parent);
	struct mlxcpld_mux_plat_data *pdata = dev_get_platdata(&client->dev);
	struct i2c_mux_core *muxc;
	int num, force;
	u8 nchans;
	struct mlxcpld_mux *data;
	int err;

	if (!pdata)
		return -EINVAL;

	if (!i2c_check_functionality(adap, I2C_FUNC_SMBUS_BYTE))
		return -ENODEV;

	switch (id->driver_data) {
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
		if (num < pdata->num_adaps)
			force = pdata->adap_ids[num];
		else
			/* discard unconfigured channels */
			break;

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
	},
	.probe		= mlxcpld_mux_probe,
	.remove		= mlxcpld_mux_remove,
	.id_table	= mlxcpld_mux_id,
};

module_i2c_driver(mlxcpld_mux_driver);

MODULE_AUTHOR("Michael Shych (michaels@mellanox.com)");
MODULE_DESCRIPTION("Mellanox I2C-CPLD-MUX driver");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:i2c-mux-mlxcpld");
