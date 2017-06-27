/*
 * Copyright (C) 2016 Maxime Ripard <maxime.ripard@free-electrons.com>
 * Copyright (C) 2017 Jonathan Liu <net147@gmail.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 */

#include <linux/clk.h>
#include <linux/i2c.h>
#include <linux/iopoll.h>
#include <linux/ktime.h>

#include "sun4i_hdmi.h"

#define SUN4I_HDMI_DDC_INT_STATUS_ERROR_MASK ( \
	SUN4I_HDMI_DDC_INT_STATUS_ILLEGAL_FIFO_OPERATION | \
	SUN4I_HDMI_DDC_INT_STATUS_DDC_RX_FIFO_UNDERFLOW | \
	SUN4I_HDMI_DDC_INT_STATUS_DDC_TX_FIFO_OVERFLOW | \
	SUN4I_HDMI_DDC_INT_STATUS_ARBITRATION_ERROR | \
	SUN4I_HDMI_DDC_INT_STATUS_ACK_ERROR | \
	SUN4I_HDMI_DDC_INT_STATUS_BUS_ERROR \
)

static bool is_err_status(u32 int_status)
{
	return !!(int_status & SUN4I_HDMI_DDC_INT_STATUS_ERROR_MASK);
}

static bool is_fifo_flag_unset(struct sun4i_hdmi *hdmi, u32 *fifo_status,
			       u32 flag)
{
	*fifo_status = readl(hdmi->base + SUN4I_HDMI_DDC_FIFO_STATUS_REG);
	return !(*fifo_status & flag);
}

static int fifo_transfer(struct sun4i_hdmi *hdmi, u8 *buf, int len, bool read)
{
	/* 1 byte takes 9 clock cycles (8 bits + 1 ACK) */
	unsigned long byte_time = DIV_ROUND_UP(USEC_PER_SEC,
					       clk_get_rate(hdmi->ddc_clk)) * 9;
	u32 int_status;
	u32 fifo_status;
	/* Read needs empty flag unset, write needs full flag unset */
	u32 flag = read ? SUN4I_HDMI_DDC_FIFO_STATUS_EMPTY :
			  SUN4I_HDMI_DDC_FIFO_STATUS_FULL;
	int ret;

	/* Wait until error or FIFO ready */
	ret = readl_poll_timeout(hdmi->base + SUN4I_HDMI_DDC_INT_STATUS_REG,
				 int_status,
				 is_err_status(int_status) ||
				 is_fifo_flag_unset(hdmi, &fifo_status, flag),
				 min(len, SUN4I_HDMI_DDC_FIFO_SIZE) * byte_time,
				 100000);

	if (is_err_status(int_status))
		return -EIO;
	if (ret)
		return -ETIMEDOUT;

	/* Read FIFO level */
	int level = (int)(fifo_status & SUN4I_HDMI_DDC_FIFO_STATUS_LEVEL_MASK);

	/* Limit transfer length using FIFO level to avoid underflow/overflow */
	len = min(len, read ? level : (SUN4I_HDMI_DDC_FIFO_SIZE - level));

	if (read)
		readsb(hdmi->base + SUN4I_HDMI_DDC_FIFO_DATA_REG, buf, len);
	else
		writesb(hdmi->base + SUN4I_HDMI_DDC_FIFO_DATA_REG, buf, len);

	return len;
}

static int xfer_msg(struct sun4i_hdmi *hdmi, struct i2c_msg *msg)
{
	int i, len;
	u32 reg;

	/* Clear errors */
	reg = readl(hdmi->base + SUN4I_HDMI_DDC_INT_STATUS_REG);
	reg &= ~SUN4I_HDMI_DDC_INT_STATUS_ERROR_MASK;
	writel(reg, hdmi->base + SUN4I_HDMI_DDC_INT_STATUS_REG);

	/* Set FIFO direction */
	reg = readl(hdmi->base + SUN4I_HDMI_DDC_CTRL_REG);
	reg &= ~SUN4I_HDMI_DDC_CTRL_FIFO_DIR_MASK;
	reg |= (msg->flags & I2C_M_RD) ?
	       SUN4I_HDMI_DDC_CTRL_FIFO_DIR_READ :
	       SUN4I_HDMI_DDC_CTRL_FIFO_DIR_WRITE;
	writel(reg, hdmi->base + SUN4I_HDMI_DDC_CTRL_REG);

	/* Set I2C address */
	writel(SUN4I_HDMI_DDC_ADDR_SLAVE(msg->addr),
	       hdmi->base + SUN4I_HDMI_DDC_ADDR_REG);

	/* Clear FIFO */
	reg = readl(hdmi->base + SUN4I_HDMI_DDC_FIFO_CTRL_REG);
	writel(reg | SUN4I_HDMI_DDC_FIFO_CTRL_CLEAR,
	       hdmi->base + SUN4I_HDMI_DDC_FIFO_CTRL_REG);
	if (readl_poll_timeout(hdmi->base + SUN4I_HDMI_DDC_FIFO_CTRL_REG,
			       reg,
			       !(reg & SUN4I_HDMI_DDC_FIFO_CTRL_CLEAR),
			       100, 100000))
		return -EIO;

	/* Set transfer length */
	writel(msg->len, hdmi->base + SUN4I_HDMI_DDC_BYTE_COUNT_REG);

	/* Set command */
	writel(msg->flags & I2C_M_RD ?
	       SUN4I_HDMI_DDC_CMD_IMPLICIT_READ :
	       SUN4I_HDMI_DDC_CMD_IMPLICIT_WRITE,
	       hdmi->base + SUN4I_HDMI_DDC_CMD_REG);

	/* Start command */
	reg = readl(hdmi->base + SUN4I_HDMI_DDC_CTRL_REG);
	writel(reg | SUN4I_HDMI_DDC_CTRL_START_CMD,
	       hdmi->base + SUN4I_HDMI_DDC_CTRL_REG);

	/* Transfer bytes */
	for (i = 0; i < msg->len; i += len) {
		len = fifo_transfer(hdmi, msg->buf + i, msg->len - i,
				    msg->flags & I2C_M_RD);
		if (len <= 0)
			return len;
	}

	/* Wait for command to finish */
	if (readl_poll_timeout(hdmi->base + SUN4I_HDMI_DDC_CTRL_REG,
			       reg,
			       !(reg & SUN4I_HDMI_DDC_CTRL_START_CMD),
			       100, 100000))
		return -EIO;

	/* Check for errors */
	reg = readl(hdmi->base + SUN4I_HDMI_DDC_INT_STATUS_REG);
	if ((reg & SUN4I_HDMI_DDC_INT_STATUS_ERROR_MASK) ||
	    !(reg & SUN4I_HDMI_DDC_INT_STATUS_TRANSFER_COMPLETE)) {
		return -EIO;
	}

	return 0;
}

static int sun4i_hdmi_i2c_xfer(struct i2c_adapter *adap,
			       struct i2c_msg *msgs, int num)
{
	struct sun4i_hdmi *hdmi = i2c_get_adapdata(adap);
	u32 reg;
	int err, i, ret = num;

	for (i = 0; i < num; i++) {
		if (!msgs[i].len)
			return -EINVAL;
		if (msgs[i].len > SUN4I_HDMI_DDC_BYTE_COUNT_MAX)
			return -EINVAL;
	}

	/* Reset I2C controller */
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

int sun4i_hdmi_i2c_create(struct device *dev, struct sun4i_hdmi *hdmi)
{
	struct i2c_adapter *adap;
	int ret = 0;

	ret = sun4i_ddc_create(hdmi, hdmi->tmds_clk);
	if (ret)
		return ret;

	adap = devm_kzalloc(dev, sizeof(*adap), GFP_KERNEL);
	if (!adap)
		return -ENOMEM;

	adap->owner = THIS_MODULE;
	adap->class = I2C_CLASS_DDC;
	adap->algo = &sun4i_hdmi_i2c_algorithm;
	strlcpy(adap->name, "sun4i_hdmi_i2c adapter", sizeof(adap->name));
	i2c_set_adapdata(adap, hdmi);

	ret = i2c_add_adapter(adap);
	if (ret)
		return ret;

	hdmi->i2c = adap;

	return ret;
}
