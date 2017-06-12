/*
 * Copyright (C) 2017 Jonathan Liu
 *
 * Jonathan Liu <net147@gmail.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 */

#include <linux/clk.h>
#include <linux/i2c.h>
#include <linux/iopoll.h>

#include "sun4i_hdmi.h"

static int xfer_msg_chunk(struct sun4i_hdmi *hdmi, struct i2c_msg *msg)
{
	u32 count = min_t(u32, msg->len, SUN4I_HDMI_DDC_FIFO_SIZE);
	u32 reg;
	int i;

	reg = readl(hdmi->base + SUN4I_HDMI_DDC_FIFO_CTRL_REG);
	writel(reg | SUN4I_HDMI_DDC_FIFO_CTRL_CLEAR,
	       hdmi->base + SUN4I_HDMI_DDC_FIFO_CTRL_REG);

	writel(count, hdmi->base
	       + SUN4I_HDMI_DDC_BYTE_COUNT_REG);
	writel(msg->flags & I2C_M_RD
	       ? SUN4I_HDMI_DDC_CMD_IMPLICIT_READ
	       : SUN4I_HDMI_DDC_CMD_IMPLICIT_WRITE,
	       hdmi->base + SUN4I_HDMI_DDC_CMD_REG);

	reg = readl(hdmi->base + SUN4I_HDMI_DDC_CTRL_REG);
	writel(reg | SUN4I_HDMI_DDC_CTRL_START_CMD,
	       hdmi->base + SUN4I_HDMI_DDC_CTRL_REG);

	if (!(msg->flags & I2C_M_RD)) {
		for (i = 0; i < count; i++) {
			writeb(*msg->buf++, hdmi->base
			       + SUN4I_HDMI_DDC_FIFO_DATA_REG);
			--msg->len;
		}
	}

	if (readl_poll_timeout(hdmi->base + SUN4I_HDMI_DDC_CTRL_REG,
			       reg,
			       !(reg & SUN4I_HDMI_DDC_CTRL_START_CMD),
			       100, 100000))
		return -EIO;

	reg = readl(hdmi->base + SUN4I_HDMI_DDC_INTERRUPT_STATUS_REG);
	reg &= ~SUN4I_HDMI_DDC_INTERRUPT_STATUS_FIFO_REQUEST;

	if (reg != SUN4I_HDMI_DDC_INTERRUPT_STATUS_TRANSFER_COMPLETE)
		return -EIO;

	if (msg->flags & I2C_M_RD) {
		for (i = 0; i < count; i++) {
			*msg->buf++ = readb(hdmi->base
					    + SUN4I_HDMI_DDC_FIFO_DATA_REG);
			--msg->len;
		}
	}

	return 0;
}

static int xfer_msg(struct sun4i_hdmi *hdmi, struct i2c_msg *msg)
{
	u32 reg;
	int ret;

	if (!msg->len)
		return -EIO;

	while (msg->len) {
		reg = readl(hdmi->base + SUN4I_HDMI_DDC_CTRL_REG);
		reg &= ~SUN4I_HDMI_DDC_CTRL_FIFO_DIR_MASK;

		writel(reg | (msg->flags & I2C_M_RD
			      ? SUN4I_HDMI_DDC_CTRL_FIFO_DIR_READ
			      : SUN4I_HDMI_DDC_CTRL_FIFO_DIR_WRITE),
		       hdmi->base + SUN4I_HDMI_DDC_CTRL_REG);

		writel(SUN4I_HDMI_DDC_ADDR_SLAVE(msg->addr),
		       hdmi->base + SUN4I_HDMI_DDC_ADDR_REG);

		ret = xfer_msg_chunk(hdmi, msg);
		if (ret)
			return ret;
	}

	return 0;
}

static int sun4i_hdmi_i2c_xfer(struct i2c_adapter *adap,
			       struct i2c_msg *msgs, int num)
{
	struct sun4i_hdmi *hdmi = i2c_get_adapdata(adap);
	u32 reg;
	int err, ret = num;
	int i;

	/* Reset i2c controller */
	writel(SUN4I_HDMI_DDC_CTRL_ENABLE | SUN4I_HDMI_DDC_CTRL_RESET,
	       hdmi->base + SUN4I_HDMI_DDC_CTRL_REG);
	if (readl_poll_timeout(hdmi->base + SUN4I_HDMI_DDC_CTRL_REG, reg,
			       !(reg & SUN4I_HDMI_DDC_CTRL_RESET),
			       100, 2000))
		return -EIO;

	writel(SUN4I_HDMI_DDC_LINE_CTRL_SDA_ENABLE |
	       SUN4I_HDMI_DDC_LINE_CTRL_SCL_ENABLE,
	       hdmi->base + SUN4I_HDMI_DDC_LINE_CTRL_REG);

	clk_prepare_enable(hdmi->ddc_clk);
	clk_set_rate(hdmi->ddc_clk, 100000);

	for (i = 0; i < num; i++) {
		err = xfer_msg(hdmi, &msgs[i]);
		if (err) {
			ret = err;
			break;
		}
	}

	clk_disable_unprepare(hdmi->ddc_clk);
	return ret;
}

static u32 sun4i_hdmi_i2c_func(struct i2c_adapter *adap)
{
	return I2C_FUNC_I2C | I2C_FUNC_SMBUS_EMUL;
}

static const struct i2c_algorithm sun4i_hdmi_i2c_algorithm = {
	.master_xfer	= sun4i_hdmi_i2c_xfer,
	.functionality	= sun4i_hdmi_i2c_func,
};

static struct i2c_adapter sun4i_hdmi_i2c_adapter = {
	.owner	= THIS_MODULE,
	.class	= I2C_CLASS_DDC,
	.algo	= &sun4i_hdmi_i2c_algorithm,
	.name	= "sun4i_hdmi_i2c adapter",
};

int sun4i_hdmi_i2c_create(struct sun4i_hdmi *hdmi)
{
	int ret = 0;

	i2c_set_adapdata(&sun4i_hdmi_i2c_adapter, hdmi);

	ret = i2c_add_adapter(&sun4i_hdmi_i2c_adapter);
	if (ret)
		return ret;

	hdmi->i2c = &sun4i_hdmi_i2c_adapter;

	return ret;
}
