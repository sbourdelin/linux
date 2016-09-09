/*
 * drivers/i2c/busses/i2c-mlxcpld.c
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

#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>

/* General defines */
#define MLXPLAT_CPLD_LPC_I2C_BASE_ADRR	0x2000
#define MLXCPLD_I2C_DEVICE_NAME		"i2c_mlxcpld"
#define MLXCPLD_I2C_VALID_FLAG		(I2C_M_RECV_LEN | I2C_M_RD)
#define MLXCPLD_I2C_BUS_NUM		1
#define MLXCPLD_I2C_DATA_REG_SZ		36
#define MLXCPLD_I2C_MAX_ADDR_LEN	4
#define MLXCPLD_I2C_RETR_NUM		2
#define MLXCPLD_I2C_XFER_TO		500000 /* msec */
#define MLXCPLD_I2C_POLL_TIME		2000   /* msec */

/* LPC I2C registers */
#define MLXCPLD_LPCI2C_LPF_REG		0x0
#define MLXCPLD_LPCI2C_CTRL_REG		0x1
#define MLXCPLD_LPCI2C_HALF_CYC_REG	0x4
#define MLXCPLD_LPCI2C_I2C_HOLD_REG	0x5
#define MLXCPLD_LPCI2C_CMD_REG		0x6
#define MLXCPLD_LPCI2C_NUM_DAT_REG	0x7
#define MLXCPLD_LPCI2C_NUM_ADDR_REG	0x8
#define MLXCPLD_LPCI2C_STATUS_REG	0x9
#define MLXCPLD_LPCI2C_DATA_REG		0xa

/* LPC I2C masks and parametres */
#define MLXCPLD_LPCI2C_RST_SEL_MASK	0x1
#define MLXCPLD_LPCI2C_LPF_DFLT		0x2
#define MLXCPLD_LPCI2C_HALF_CYC_100	0x1f
#define MLXCPLD_LPCI2C_I2C_HOLD_100	0x3c
#define MLXCPLD_LPCI2C_TRANS_END	0x1
#define MLXCPLD_LPCI2C_STATUS_NACK	0x10
#define MLXCPLD_LPCI2C_ERR_IND		-1
#define MLXCPLD_LPCI2C_NO_IND		0
#define MLXCPLD_LPCI2C_ACK_IND		1
#define MLXCPLD_LPCI2C_NACK_IND		2

/**
 * mlxcpld_i2c_regs - controller registers:
 * @half_cyc - half cycle register
 * @i2c_hold - hold register
 * @config - config register
 * @cmd - command register
 * @cmd - status register
 * @data - register data
**/
struct mlxcpld_i2c_regs {
	u8 half_cyc;
	u8 i2c_hold;
	u8 config;
	u8 cmd;
	u8 status;
	u8 data[MLXCPLD_I2C_DATA_REG_SZ];
};

/**
 * mlxcpld_i2c_curr_transf - current transaction parameters:
 * @cmd - command
 * @addr_width - address width
 * @data_len - data length
 * @cmd - command register
 * @msg_num - message number
 * @msg - pointer to message buffer
**/
struct mlxcpld_i2c_curr_transf {
	u8 cmd;
	u8 addr_width;
	u8 data_len;
	u8 msg_num;
	struct i2c_msg *msg;
};

/**
 * mlxcpld_i2c_priv - private controller data:
 * @lpc_gen_dec_reg - register space
 * @adap - i2c adapter
 * @dev_id - device id
 * @base_addr - base IO address
 * @poll_time - polling time
 * @xfer_to - transfer timeout in microsec (500000)
 * @retr_num - access retries number (2)
 * @block_sz - maximum data block size (36),
 * @lock - bus access lock
 * @lpc_i2c_res - lpc i2c resourse
 * @lpc_cpld_res - lpc cpld resource
 * @xfer - current i2c transfer block
 * @pdev - platform device
**/
struct mlxcpld_i2c_priv {
	struct i2c_adapter adap;
	u16 dev_id;
	u16 base_addr;
	u16 poll_time;
	int xfer_to;
	int retr_num;
	int block_sz;
	struct mutex lock;
	struct mlxcpld_i2c_curr_transf xfer;
	struct platform_device *pdev;
};
struct platform_device *mlxcpld_i2c_plat_dev;

static void mlxcpld_i2c_lpc_write_buf(u8 *data, u8 len, u32 addr)
{
	int i, nbyte, ndword;

	nbyte = len % 4;
	ndword = len / 4;
	for (i = 0; i < ndword; i++)
		outl(*((u32 *)data + i), addr + i * 4);
	ndword *= 4;
	addr += ndword;
	data += ndword;
	for (i = 0; i < nbyte; i++)
		outb(*(data + i), addr + i);
}

static void mlxcpld_i2c_lpc_read_buf(u8 *data, u8 len, u32 addr)
{
	int i, nbyte, ndword;

	nbyte = len % 4;
	ndword = len / 4;
	for (i = 0; i < ndword; i++)
		*((u32 *)data + i) = inl(addr + i * 4);
	ndword *= 4;
	addr += ndword;
	data += ndword;
	for (i = 0; i < nbyte; i++)
		*(data + i) = inb(addr + i);
}

static void mlxcpld_i2c_read_comm(struct mlxcpld_i2c_priv *priv, u8 offs,
				  u8 *data, u8 datalen)
{
	u32 addr = priv->base_addr + offs;

	switch (datalen) {
	case 1:
		*(data) = inb(addr);
		break;
	case 2:
		*((u16 *)data) = inw(addr);
		break;
	case 3:
		*((u16 *)data) = inw(addr);
		*(data + 2) = inb(addr + 2);
		break;
	case 4:
		*((u32 *)data) = inl(addr);
		break;
	default:
		mlxcpld_i2c_lpc_read_buf(data, datalen, addr);
		break;
	}
}

static void mlxcpld_i2c_write_comm(struct mlxcpld_i2c_priv *priv, u8 offs,
				   u8 *data, u8 datalen)
{
	u32 addr = priv->base_addr + offs;

	switch (datalen) {
	case 1:
		outb(*(data), addr);
		break;
	case 2:
		outw(*((u16 *)data), addr);
		break;
	case 3:
		outw(*((u16 *)data), addr);
		outb(*(data + 2), addr + 2);
		break;
	case 4:
		outl(*((u32 *)data), addr);
		break;
	default:
		mlxcpld_i2c_lpc_write_buf(data, datalen, addr);
		break;
	}
}

/* Check validity of current i2c message and all transfer.
 * Calculate also coomon length of all i2c messages in transfer.
 */
static int mlxcpld_i2c_invalid_len(struct mlxcpld_i2c_priv *priv,
				   const struct i2c_msg *msg, u8 *comm_len)
{
	u8 max_len = msg->flags == I2C_M_RD ? priv->block_sz -
		     MLXCPLD_I2C_MAX_ADDR_LEN : priv->block_sz;

	if (msg->len < 0 || msg->len > max_len)
		return -EINVAL;

	*comm_len += msg->len;
	if (*comm_len > priv->block_sz)
		return -EINVAL;
	else
		return 0;
}

/* Check validity of received i2c messages parameters.
 *  Returns 0 if OK, other - in case of invalid parameters
 *  or common length of data that should be passed to CPLD
 */
static int mlxcpld_i2c_check_msg_params(struct mlxcpld_i2c_priv *priv,
					struct i2c_msg *msgs, int num,
					u8 *comm_len)
{
	int i;

	if (!num) {
		dev_err(&priv->pdev->dev, "Incorrect 0 num of messages\n");
		return -EINVAL;
	}

	if (unlikely(msgs[0].addr > 0x7f)) {
		dev_err(&priv->pdev->dev, "Invalid address 0x%03x\n",
			msgs[0].addr);
		return -EINVAL;
	}

	for (i = 0; i < num; ++i) {
		if (unlikely(!msgs[i].buf)) {
			dev_err(&priv->pdev->dev, "Invalid buf in msg[%d]\n",
				i);
			return -EINVAL;
		}
		if (unlikely(msgs[0].addr != msgs[i].addr)) {
			dev_err(&priv->pdev->dev, "Invalid addr in msg[%d]\n",
				i);
			return -EINVAL;
		}
		if (unlikely(mlxcpld_i2c_invalid_len(priv, &msgs[i],
						     comm_len))) {
			dev_err(&priv->pdev->dev, "Invalid len %d msg[%d], addr 0x%x, lag %u\n",
				msgs[i].len, i, msgs[i].addr, msgs[i].flags);
			return -EINVAL;
		}
	}

	return 0;
}

/* Check if transfer is completed and status of operation.
 * Returns 0 - transfer completed (both ACK or NACK),
 * negative - transfer isn't finished.
 */
static int mlxcpld_i2c_check_status(struct mlxcpld_i2c_priv *priv, int *status)
{
	u8 val;

	mlxcpld_i2c_read_comm(priv, MLXCPLD_LPCI2C_STATUS_REG, &val, 1);

	if (val & MLXCPLD_LPCI2C_TRANS_END) {
		if (val & MLXCPLD_LPCI2C_STATUS_NACK)
			/* The slave is unable to accept the data. No such
			 * slave, command not understood, or unable to accept
			 * any more data.
			 */
			*status = MLXCPLD_LPCI2C_NACK_IND;
		else
			*status = MLXCPLD_LPCI2C_ACK_IND;
		return 0;
	}
	*status = MLXCPLD_LPCI2C_NO_IND;

	return -EIO;
}

static void mlxcpld_i2c_set_transf_data(struct mlxcpld_i2c_priv *priv,
					struct i2c_msg *msgs, int num,
					u8 comm_len)
{
	priv->xfer.msg = msgs;
	priv->xfer.msg_num = num;

	/*
	 * All upper layers currently are never use transfer with more than
	 * 2 messages. Actually, it's also not so relevant in Mellanox systems
	 * because of HW limitation. Max size of transfer is o more than 20B
	 * in current x86 LPCI2C bridge.
	 */
	priv->xfer.cmd = (msgs[num - 1].flags & I2C_M_RD);

	if (priv->xfer.cmd == I2C_M_RD) {
		if (comm_len == msgs[0].len) {
			/* Special case of addr_width = 0 */
			priv->xfer.addr_width = 0;
			priv->xfer.data_len = comm_len;
		} else {
			priv->xfer.addr_width = msgs[0].len;
			priv->xfer.data_len = comm_len - priv->xfer.addr_width;
		}
	} else {
		/* Width (I2C_NUM_ADDR reg) isn't used in write command. */
		priv->xfer.addr_width = 0;
		priv->xfer.data_len = comm_len;
	}
}

/* Reset CPLD LPCI2C block */
static void mlxcpld_i2c_reset(struct mlxcpld_i2c_priv *priv)
{
	u8 val;

	mutex_lock(&priv->lock);
	mlxcpld_i2c_read_comm(priv, MLXCPLD_LPCI2C_CTRL_REG, &val, 1);
	val &= ~MLXCPLD_LPCI2C_RST_SEL_MASK;
	mlxcpld_i2c_write_comm(priv, MLXCPLD_LPCI2C_CTRL_REG, &val, 1);
	mutex_unlock(&priv->lock);
}

/* Make sure the CPLD is ready to start transmitting.
 * Return 0 if it is, -EBUSY if it is not.
 */
static int mlxcpld_i2c_check_busy(struct mlxcpld_i2c_priv *priv)
{
	u8 val;

	mlxcpld_i2c_read_comm(priv, MLXCPLD_LPCI2C_STATUS_REG, &val, 1);

	if (val & MLXCPLD_LPCI2C_TRANS_END)
		return 0;

	return -EIO;
}

static int mlxcpld_i2c_wait_for_free(struct mlxcpld_i2c_priv *priv)
{
	int timeout = 0;

	do {
		if (!mlxcpld_i2c_check_busy(priv))
			break;
		usleep_range(priv->poll_time/2, priv->poll_time);
		timeout += priv->poll_time;
	} while (timeout < priv->xfer_to);

	if (timeout > priv->xfer_to)
		return -ETIMEDOUT;

	return 0;
}

/*
 * Wait for master transfer to complete.
 * It puts current process to sleep until we get interrupt or timeout expires.
 * Returns the number of transferred or read bytes or error (<0).
 */
static int mlxcpld_i2c_wait_for_tc(struct mlxcpld_i2c_priv *priv)
{
	int status, i = 1, timeout = 0;
	u8 datalen;
	int err = 0;

	do {
		usleep_range(priv->poll_time / 2, priv->poll_time);
		if (!mlxcpld_i2c_check_status(priv, &status))
			break;
		timeout += priv->poll_time;
	} while (status == 0 && timeout < priv->xfer_to);

	switch (status) {
	case MLXCPLD_LPCI2C_NO_IND:
		return -ETIMEDOUT;
	case MLXCPLD_LPCI2C_ACK_IND:
		if (priv->xfer.cmd == I2C_M_RD) {
			/*
			 * Actual read data len will be always the same as
			 * requested len. 0xff (line pull-up) will be returned
			 * if slave has no data to return. Thus don't read
			 * MLXCPLD_LPCI2C_NUM_DAT_REG reg from CPLD.
			 */
			err = datalen = priv->xfer.data_len;
			if (priv->xfer.msg_num == 1)
				i = 0;

			if (!priv->xfer.msg[i].buf)
				err = -EINVAL;
			else
				mlxcpld_i2c_read_comm(priv,
						      MLXCPLD_LPCI2C_DATA_REG,
						      priv->xfer.msg[i].buf,
						      datalen);
		} else {
			err = priv->xfer.addr_width + priv->xfer.data_len;
		}
		break;
	case MLXCPLD_LPCI2C_NACK_IND:
		err = -EAGAIN;
		break;
	case MLXCPLD_LPCI2C_ERR_IND:
		err = -EIO;
		break;
	default:
		break;
	}

	return err;
}

static void mlxcpld_i2c_xfer_msg(struct mlxcpld_i2c_priv *priv)
{
	int i, len = 0;
	u8 cmd;

	mlxcpld_i2c_write_comm(priv, MLXCPLD_LPCI2C_NUM_DAT_REG,
			       &priv->xfer.data_len, 1);
	mlxcpld_i2c_write_comm(priv, MLXCPLD_LPCI2C_NUM_ADDR_REG,
			       &priv->xfer.addr_width, 1);

	for (i = 0; i < priv->xfer.msg_num; i++) {
		if ((priv->xfer.msg[i].flags & I2C_M_RD) != I2C_M_RD) {
			/* Don't write to CPLD buffer in read transaction */
			mlxcpld_i2c_write_comm(priv, MLXCPLD_LPCI2C_DATA_REG +
					       len, priv->xfer.msg[i].buf,
					       priv->xfer.msg[i].len);
			len += priv->xfer.msg[i].len;
		}
	}

	/* Set target slave address with command for master transfer.
	 * It should be latest executed function before CPLD transaction.
	 */
	cmd = (priv->xfer.msg[0].addr << 1) | priv->xfer.cmd;
	mlxcpld_i2c_write_comm(priv, MLXCPLD_LPCI2C_CMD_REG, &cmd, 1);
}

/* Generic lpc-i2c transfer.
 * Returns the number of processed messages or error (<0).
 */
static int mlxcpld_i2c_xfer(struct i2c_adapter *adap, struct i2c_msg *msgs,
			    int num)
{
	struct mlxcpld_i2c_priv *priv = i2c_get_adapdata(adap);
	u8 comm_len = 0;
	int err;

	err = mlxcpld_i2c_check_msg_params(priv, msgs, num, &comm_len);
	if (err) {
		dev_err(&priv->pdev->dev, "Incorrect message\n");
		return err;
	}

	/* Check bus state */
	if (mlxcpld_i2c_wait_for_free(priv)) {
		dev_err(&priv->pdev->dev, "LPCI2C bridge is busy\n");

		/*
		 * Usually it means something serious has happened.
		 * We can not have unfinished previous transfer
		 * so it doesn't make any sense to try to stop it.
		 * Probably we were not able to recover from the
		 * previous error.
		 * The only reasonable thing - is soft reset.
		 */
		mlxcpld_i2c_reset(priv);
		if (mlxcpld_i2c_check_busy(priv)) {
			dev_err(&priv->pdev->dev, "LPCI2C bridge is busy after reset\n");
			return -EIO;
		}
	}

	mlxcpld_i2c_set_transf_data(priv, msgs, num, comm_len);

	mutex_lock(&priv->lock);
	/* Do real transfer. Can't fail */
	mlxcpld_i2c_xfer_msg(priv);
	/* Wait for transaction complete */
	err = mlxcpld_i2c_wait_for_tc(priv);
	mutex_unlock(&priv->lock);

	return err < 0 ? err : num;
}

static u32 mlxcpld_i2c_func(struct i2c_adapter *adap)
{
	return I2C_FUNC_I2C | I2C_FUNC_SMBUS_EMUL | I2C_FUNC_SMBUS_BLOCK_DATA;
}

static const struct i2c_algorithm mlxcpld_i2c_algo = {
	.master_xfer	= mlxcpld_i2c_xfer,
	.functionality	= mlxcpld_i2c_func
};

static struct i2c_adapter mlxcpld_i2c_adapter = {
	.owner          = THIS_MODULE,
	.name           = "i2c-mlxcpld",
	.class          = I2C_CLASS_HWMON | I2C_CLASS_SPD,
	.algo           = &mlxcpld_i2c_algo,
};

static int mlxcpld_i2c_probe(struct platform_device *pdev)
{
	struct mlxcpld_i2c_priv *priv;
	int err;

	priv = devm_kzalloc(&pdev->dev, sizeof(struct mlxcpld_i2c_priv),
			    GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	mutex_init(&priv->lock);
	platform_set_drvdata(pdev, priv);
	priv->pdev = pdev;
	priv->xfer_to = MLXCPLD_I2C_XFER_TO;
	priv->retr_num = MLXCPLD_I2C_RETR_NUM;
	priv->block_sz = MLXCPLD_I2C_DATA_REG_SZ;
	priv->poll_time = MLXCPLD_I2C_POLL_TIME;
	/* Register with i2c layer */
	priv->adap = mlxcpld_i2c_adapter;
	priv->adap.dev.parent = &pdev->dev;
	i2c_set_adapdata(&priv->adap, priv);
	priv->adap.retries = priv->retr_num;
	priv->adap.nr = MLXCPLD_I2C_BUS_NUM;
	priv->adap.timeout = usecs_to_jiffies(priv->xfer_to);

	err = i2c_add_numbered_adapter(&priv->adap);
	if (err) {
		dev_err(&pdev->dev, "Failed to add %s adapter (%d)\n",
			MLXCPLD_I2C_DEVICE_NAME, err);
		goto fail_adapter;
	}

	priv->base_addr = MLXPLAT_CPLD_LPC_I2C_BASE_ADRR;

	return 0;

fail_adapter:
	mutex_destroy(&priv->lock);
	return err;
}

static int mlxcpld_i2c_remove(struct platform_device *pdev)
{
	struct mlxcpld_i2c_priv *priv = platform_get_drvdata(pdev);

	i2c_del_adapter(&priv->adap);
	mutex_destroy(&priv->lock);

	return 0;
}

static struct platform_driver mlxcpld_i2c_driver = {
	.probe		= mlxcpld_i2c_probe,
	.remove		= mlxcpld_i2c_remove,
	.driver = {
		.name = MLXCPLD_I2C_DEVICE_NAME,
	},
};

module_platform_driver(mlxcpld_i2c_driver);

MODULE_AUTHOR("Michael Shych (michaels@mellanox.com)");
MODULE_DESCRIPTION("Mellanox I2C-CPLD controller driver");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:i2c-mlxcpld");
