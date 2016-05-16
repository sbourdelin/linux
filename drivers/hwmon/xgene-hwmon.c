/*
 * APM X-Gene SoC Hardware Monitoring Driver
 *
 * Copyright (c) 2016, Applied Micro Circuits Corporation
 * Author: Loc Ho <lho@apm.com>
 *         Hoan Tran <hotran@apm.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 *
 * This driver provides the following features:
 *  - Retrieve CPU's total power (uW)
 *  - Retrieve IO's total power (uW)
 *  - Retrieve SoC total power (uW)
 *  - Retrieve SoC temperature (milli-degree C) and alarm
 */
#include <linux/acpi.h>
#include <linux/dma-mapping.h>
#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include <linux/kfifo.h>
#include <linux/interrupt.h>
#include <linux/mailbox_client.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <acpi/cppc_acpi.h>

/* SLIMpro message defines */
#define SLIMPRO_MSG_TYPE_DBG_ID		0
#define SLIMPRO_MSG_TYPE_ERR_ID		7
#define SLIMPRO_MSG_TYPE_PWRMGMT_ID	9

#define SLIMPRO_MSG_TYPE(v)		(((v) & 0xF0000000) >> 28)
#define SLIMPRO_MSG_TYPE_SET(v)		(((v) << 28) & 0xF0000000)
#define SLIMPRO_MSG_SUBTYPE(v)		(((v) & 0x0F000000) >> 24)
#define SLIMPRO_MSG_SUBTYPE_SET(v)	(((v) << 24) & 0x0F000000)

#define SLIMPRO_DBG_SUBTYPE_SENSOR_READ	4
#define SLIMPRO_SENSOR_READ_MSG		0x04FFE902
#define SLIMPRO_SENSOR_READ_ENCODE_ADDR(a) \
	((a) & 0x000FFFFF)
#define PMD_PWR_MW_REG			0x26
#define SOC_PWR_REG			0x21
#define SOC_TEMP_REG			0x10

#define SLIMPRO_PWRMGMT_SUBTYPE_TPC	1
#define SLIMPRO_TPC_ALARM		2
#define SLIMPRO_TPC_GET_ALARM		3
#define SLIMPRO_TPC_CMD(v)		(((v) & 0x00FF0000) >> 16)
#define SLIMPRO_TPC_CMD_SET(v)		(((v) << 16) & 0x00FF0000)
#define SLIMPRO_TPC_ENCODE_MSG(hndl, cmd, type) \
	(SLIMPRO_MSG_TYPE_SET(SLIMPRO_MSG_TYPE_PWRMGMT_ID) | \
	SLIMPRO_MSG_SUBTYPE_SET(hndl) | \
	SLIMPRO_TPC_CMD_SET(cmd) | \
	type)

/* PCC defines */
#define SLIMPRO_MSG_PCC_SUBSPACE	7
#define PCC_SIGNATURE_MASK		0x50424300
#define PCCC_GENERATE_DB_INT		BIT(15)
#define PCCS_CMD_COMPLETE		BIT(0)
#define PCCS_SCI_DOORBEL		BIT(1)
#define PCCS_PLATFORM_NOTIFICATION	BIT(3)
/*
 * Arbitrary retries in case the remote processor is slow to respond
 * to PCC commands
 */
#define PCC_NUM_RETRIES			500

#define ASYNC_MSG_FIFO_SIZE		16
#define MBOX_HWMON_INDEX		0
#define MBOX_OP_TIMEOUTMS		1000

#define SOC_TEMP			0
#define CPU_POWER			0
#define IO_POWER			1
#define SOC_POWER			2

#define WATT_TO_mWATT(x)		((x) * 1000)
#define mWATT_TO_uWATT(x)		((x) * 1000)
#define WATT_TO_uWATT(x)		((x) * 1000000)
#define CELSIUS_TO_mCELSIUS(x)		((x) * 1000)

#define to_xgene_hwmon_dev(cl)		\
	container_of(cl, struct xgene_hwmon_dev, mbox_client)

struct slimpro_resp_msg {
	u32 msg;
	u32 param1;
	u32 param2;
} __packed;

struct xgene_hwmon_dev {
	struct device		*dev;
	struct mbox_chan	*mbox_chan;
	struct mbox_client	mbox_client;

	spinlock_t		lock;
	struct completion	rd_complete;
	int			resp_pending;
	struct slimpro_resp_msg sync_msg;

	struct work_struct	workq;
	struct kfifo_rec_ptr_1	async_msg_fifo;

	struct device		*hwmon_dev;
	bool			temp_critical_alarm;

	phys_addr_t		comm_base_addr;
	void			*pcc_comm_addr;
	u64			usecs_lat;
};

/*
 * This function tests and clears a bitmask then returns its old value
 */
static u16 xgene_word_tst_and_clr(u16 *addr, u16 mask)
{
	u16 ret, val;

	val = readw_relaxed(addr);
	ret = val & mask;
	val &= ~mask;
	writew_relaxed(val, addr);

	return ret;
}

static int xgene_hwmon_pcc_rd(struct xgene_hwmon_dev *ctx, u32 *msg)
{
	struct acpi_pcct_shared_memory *generic_comm_base = ctx->pcc_comm_addr;
	void *ptr = generic_comm_base + 1;
	unsigned long flags;
	u16 val;
	int rc;

	spin_lock_irqsave(&ctx->lock, flags);
	if (ctx->resp_pending) {
		spin_unlock_irqrestore(&ctx->lock, flags);
		return -EAGAIN;
	}

	init_completion(&ctx->rd_complete);
	ctx->resp_pending = true;

	/* Write signature for subspace */
	writel_relaxed(PCC_SIGNATURE_MASK | SLIMPRO_MSG_PCC_SUBSPACE,
		       &generic_comm_base->signature);

	/* Write to the shared command region */
	writew_relaxed(SLIMPRO_MSG_TYPE(msg[0]) | PCCC_GENERATE_DB_INT,
		       &generic_comm_base->command);

	/* Flip CMD COMPLETE bit */
	val = readw_relaxed(&generic_comm_base->status);
	val &= ~PCCS_CMD_COMPLETE;
	writew_relaxed(val, &generic_comm_base->status);

	/* Copy the message to the PCC comm space */
	memcpy(ptr, msg, sizeof(struct slimpro_resp_msg));

	/* Ring the doorbell */
	rc = mbox_send_message(ctx->mbox_chan, msg);
	if (rc < 0) {
		dev_err(ctx->dev, "Mailbox send error %d\n", rc);
		goto err;
	}
	spin_unlock_irqrestore(&ctx->lock, flags);
	if (!wait_for_completion_timeout(&ctx->rd_complete,
					 usecs_to_jiffies(ctx->usecs_lat))) {
		spin_lock_irqsave(&ctx->lock, flags);
		dev_err(ctx->dev, "Mailbox operation timed out\n");
		rc = -EIO;
		goto err;
	}
	spin_lock_irqsave(&ctx->lock, flags);

	/* Check for invalid data or no device */
	if (SLIMPRO_MSG_TYPE(ctx->sync_msg.msg) == SLIMPRO_MSG_TYPE_ERR_ID ||
	    ctx->sync_msg.msg == 0xffffffff) {
		rc = -ENODEV;
		goto err;
	}

	msg[0] = ctx->sync_msg.msg;
	msg[1] = ctx->sync_msg.param1;
	msg[2] = ctx->sync_msg.param2;

err:
	mbox_chan_txdone(ctx->mbox_chan, 0);
	ctx->resp_pending = false;
	spin_unlock_irqrestore(&ctx->lock, flags);
	return rc;
}

static int xgene_hwmon_rd(struct xgene_hwmon_dev *ctx, u32 *msg)
{
	unsigned long flags;
	int rc;

	spin_lock_irqsave(&ctx->lock, flags);
	if (ctx->resp_pending) {
		spin_unlock_irqrestore(&ctx->lock, flags);
		return -EAGAIN;
	}

	init_completion(&ctx->rd_complete);
	ctx->resp_pending = true;
	rc = mbox_send_message(ctx->mbox_chan, msg);
	if (rc < 0) {
		dev_err(ctx->dev, "Mailbox send error %d\n", rc);
		goto err;
	}
	spin_unlock_irqrestore(&ctx->lock, flags);

	if (!wait_for_completion_timeout(&ctx->rd_complete,
					 msecs_to_jiffies(MBOX_OP_TIMEOUTMS))) {
		spin_lock_irqsave(&ctx->lock, flags);
		dev_err(ctx->dev, "Mailbox operation timed out\n");
		rc = -EIO;
		goto err;
	}
	spin_lock_irqsave(&ctx->lock, flags);

	/* Check for invalid data or no device */
	if (SLIMPRO_MSG_TYPE(ctx->sync_msg.msg) == SLIMPRO_MSG_TYPE_ERR_ID ||
	    ctx->sync_msg.msg == 0xffffffff) {
		rc = -ENODEV;
		goto err;
	}

	msg[0] = ctx->sync_msg.msg;
	msg[1] = ctx->sync_msg.param1;
	msg[2] = ctx->sync_msg.param2;

err:
	ctx->resp_pending = false;
	spin_unlock_irqrestore(&ctx->lock, flags);
	return rc;
}

static int xgene_hwmon_reg_map_rd(struct xgene_hwmon_dev *ctx, u32 addr,
				  u32 *data)
{
	u32 msg[3];
	int rc;

	msg[0] = SLIMPRO_SENSOR_READ_MSG;
	msg[1] = SLIMPRO_SENSOR_READ_ENCODE_ADDR(addr);
	msg[2] = 0;

	if (ACPI_COMPANION(ctx->dev))
		rc = xgene_hwmon_pcc_rd(ctx, msg);
	else
		rc = xgene_hwmon_rd(ctx, msg);
	if (rc < 0) {
		/* To support compatibility with firmware, return 0 */
		dev_err(ctx->dev, "SLIMpro register 0x%02X read error %d\n",
			addr, rc);
		*data = 0;
		return rc;
	}
	*data = msg[1];

	return rc;
}

static int xgene_hwmon_get_notification_msg(struct xgene_hwmon_dev *ctx,
					    u32 *amsg)
{
	u32 msg[3];
	int rc;

	msg[0] = SLIMPRO_TPC_ENCODE_MSG(SLIMPRO_PWRMGMT_SUBTYPE_TPC,
					SLIMPRO_TPC_GET_ALARM, 0);
	msg[1] = 0;
	msg[2] = 0;

	rc = xgene_hwmon_pcc_rd(ctx, msg);
	if (rc < 0) {
		dev_err(ctx->dev, "PCC Alarm read error %d\n", rc);
		return rc;
	}
	amsg[0] = msg[0];
	amsg[1] = msg[1];
	amsg[2] = msg[2];

	return rc;
}

static int xgene_hwmon_get_cpu_pwr(struct xgene_hwmon_dev *ctx, u32 *val)
{
	return xgene_hwmon_reg_map_rd(ctx, PMD_PWR_MW_REG, val);
}

static int xgene_hwmon_get_io_pwr(struct xgene_hwmon_dev *ctx, u32 *val)
{
	return xgene_hwmon_reg_map_rd(ctx, SOC_PWR_REG, val);
}

static int xgene_hwmon_get_soc_power(struct xgene_hwmon_dev *ctx, u32 *val)
{
	u32 pmd_vrm_power;
	u32 io_vrm_power;
	int rc;

	rc = xgene_hwmon_get_cpu_pwr(ctx, &pmd_vrm_power);
	if (rc < 0)
		return rc;

	rc = xgene_hwmon_get_io_pwr(ctx, &io_vrm_power);
	if (rc < 0)
		return rc;

	*val = pmd_vrm_power + WATT_TO_mWATT(io_vrm_power);

	return 0;
}

static int xgene_hwmon_get_temp(struct xgene_hwmon_dev *ctx, u32 *val)
{
	return xgene_hwmon_reg_map_rd(ctx, SOC_TEMP_REG, val);
}

/*
 * Sensor temperature/power functions
 */
static const char * const sensor_temp_input_names[] = {
	[SOC_TEMP] = "SoC Temperature",
};

static const char * const sensor_pwr_input_names[] = {
	[CPU_POWER] = "CPU's power",
	[IO_POWER] = "IO's power",
	[SOC_POWER] = "SoC power"
};

static ssize_t xgene_hwmon_show_name(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "APM X-Gene\n");
}

static ssize_t xgene_hwmon_show_temp(struct device *dev,
				     struct device_attribute *attr,
				     char *buf)
{
	struct xgene_hwmon_dev *ctx = dev_get_drvdata(dev);
	u32 val;
	int rc;

	rc = xgene_hwmon_get_temp(ctx, &val);
	if (rc < 0)
		return rc;

	val = CELSIUS_TO_mCELSIUS(val);

	return snprintf(buf, PAGE_SIZE, "%d\n", val);
}

static ssize_t xgene_hwmon_show_temp_label(struct device *dev,
					   struct device_attribute *attr,
					   char *buf)
{
	int channel = to_sensor_dev_attr(attr)->index - 1;

	return snprintf(buf, PAGE_SIZE, "%s\n",
			sensor_temp_input_names[channel]);
}

static ssize_t xgene_hwmon_show_temp_critical_alarm(struct device *dev,
						    struct device_attribute *devattr,
						    char *buf)
{
	struct xgene_hwmon_dev *ctx = dev_get_drvdata(dev);

	return snprintf(buf, PAGE_SIZE, "%d\n", ctx->temp_critical_alarm);
}

static ssize_t xgene_hwmon_show_pwr_label(struct device *dev,
					  struct device_attribute *attr,
					  char *buf)
{
	int channel = to_sensor_dev_attr(attr)->index - 1;

	return snprintf(buf, PAGE_SIZE, "%s\n",
			sensor_pwr_input_names[channel]);
}

static ssize_t xgene_hwmon_show_pwr(struct device *dev,
				    struct device_attribute *attr,
				    char *buf)
{
	struct xgene_hwmon_dev *ctx = dev_get_drvdata(dev);
	int channel = to_sensor_dev_attr(attr)->index - 1;
	u32 val;
	int rc;

	switch (channel) {
	case CPU_POWER:
		rc = xgene_hwmon_get_cpu_pwr(ctx, &val);
		break;
	case IO_POWER:
		rc = xgene_hwmon_get_io_pwr(ctx, &val);
		break;
	case SOC_POWER:
		rc = xgene_hwmon_get_soc_power(ctx, &val);
		break;
	default:
		rc = -EINVAL;
		break;
	}
	if (rc < 0)
		return rc;

	if (channel == IO_POWER)
		val = WATT_TO_uWATT(val);
	else
		val = mWATT_TO_uWATT(val);

	return snprintf(buf, PAGE_SIZE, "%d\n", val);
}

/* Chip name, required by hwmon */
static DEVICE_ATTR(name, S_IRUGO, xgene_hwmon_show_name, NULL);

#define SENSOR_ATTR_TEMP(index) \
	static SENSOR_DEVICE_ATTR(temp##index##_label, S_IRUGO, \
				  xgene_hwmon_show_temp_label, NULL, index); \
	static SENSOR_DEVICE_ATTR(temp##index##_input, S_IRUGO, \
				  xgene_hwmon_show_temp, NULL, index); \
	static SENSOR_DEVICE_ATTR(temp##index##_critical_alarm, S_IRUGO, \
				  xgene_hwmon_show_temp_critical_alarm, NULL, \
				  index);
#define SENSOR_ATTR_PWR(index) \
	static SENSOR_DEVICE_ATTR(power##index##_label, S_IRUGO, \
				  xgene_hwmon_show_pwr_label, NULL, index); \
	static SENSOR_DEVICE_ATTR(power##index##_input, S_IRUGO, \
				  xgene_hwmon_show_pwr, NULL, index);

#define APM_TEMP_SENSOR_ATTR(index) \
	&sensor_dev_attr_temp##index##_input.dev_attr.attr, \
	&sensor_dev_attr_temp##index##_label.dev_attr.attr, \
	&sensor_dev_attr_temp##index##_critical_alarm.dev_attr.attr
#define APM_PWR_SENSOR_ATTR(index) \
	&sensor_dev_attr_power##index##_input.dev_attr.attr, \
	&sensor_dev_attr_power##index##_label.dev_attr.attr

SENSOR_ATTR_TEMP(1); /* SoC temperature */
SENSOR_ATTR_PWR(1);  /* CPU power */
SENSOR_ATTR_PWR(2);  /* IO power */
SENSOR_ATTR_PWR(3);  /* SoC power */

static struct attribute *xgene_hwmon_attributes[] = {
	&dev_attr_name.attr,
	APM_TEMP_SENSOR_ATTR(1),
	APM_PWR_SENSOR_ATTR(1),
	APM_PWR_SENSOR_ATTR(2),
	APM_PWR_SENSOR_ATTR(3),
	NULL,
};

static const struct attribute_group xgene_hwmon_attr_group = {
	.attrs	= xgene_hwmon_attributes,
};

static int xgene_hwmon_tpc_alarm(struct xgene_hwmon_dev *ctx,
				 struct slimpro_resp_msg *amsg)
{
	char name[40];

	ctx->temp_critical_alarm = amsg->param2 ? true : false;
	snprintf(name, sizeof(name), "temp%d_critical_alarm", SOC_TEMP + 1);
	sysfs_notify(&ctx->dev->kobj, NULL, name);
	dev_alert(ctx->dev, "SoC temperature alarm at %d degree\n",
		  amsg->param1);
	return 0;
}

static void xgene_hwmon_process_pwrmsg(struct xgene_hwmon_dev *ctx,
				       struct slimpro_resp_msg *amsg)
{
	switch (SLIMPRO_MSG_SUBTYPE(amsg->msg)) {
	case SLIMPRO_PWRMGMT_SUBTYPE_TPC:
		switch (SLIMPRO_TPC_CMD(amsg->msg)) {
		case SLIMPRO_TPC_ALARM:
			xgene_hwmon_tpc_alarm(ctx, amsg);
			break;
		default:
			dev_warn(ctx->dev,
				 "Un-supported TPC message received 0x%08X\n",
				 amsg->msg);
			break;
		}
		break;
	default:
		dev_warn(ctx->dev, "Un-supported message received 0x%08X\n",
			 amsg->msg);
		break;
	}
}

/*
 * This function is called to process async work queue
 */
static void xgene_hwmon_evt_work(struct work_struct *work)
{
	struct slimpro_resp_msg amsg;
	struct xgene_hwmon_dev *ctx;
	int ret;

	ctx = container_of(work, struct xgene_hwmon_dev, workq);
	while (kfifo_out_spinlocked(&ctx->async_msg_fifo, &amsg,
				    sizeof(struct slimpro_resp_msg),
				    &ctx->lock)) {
		/*
		 * If PCC, send a consumer command to Platform to get info
		 * If Slimpro Mailbox, get message from specific FIFO
		 */
		if (ACPI_COMPANION(ctx->dev)) {
			ret = xgene_hwmon_get_notification_msg(ctx,
							       (u32 *)&amsg);
			if (ret < 0)
				continue;
		}
		switch (SLIMPRO_MSG_TYPE(amsg.msg)) {
		case SLIMPRO_MSG_TYPE_PWRMGMT_ID:
			xgene_hwmon_process_pwrmsg(ctx, &amsg);
			break;
		default:
			dev_warn(ctx->dev,
				 "Invalid mailbox msg received 0x%08X 0x%08X 0x%08X\n",
				 amsg.msg, amsg.param1, amsg.param2);
			break;
		}
	}
}

/*
 * This function is called when the SLIMpro/PCC Mailbox received a message
 */
static void xgene_hwmon_rx_cb(struct mbox_client *cl, void *msg)
{
	struct xgene_hwmon_dev *ctx = to_xgene_hwmon_dev(cl);
	struct acpi_pcct_shared_memory *generic_comm_base = ctx->pcc_comm_addr;
	struct slimpro_resp_msg amsg;

	/* If PCC mailbox controller, get msg from shared memory */
	if (ACPI_COMPANION(ctx->dev)) {
		msg = generic_comm_base + 1;
		/* Check if platform sends interrupt */
		if (!xgene_word_tst_and_clr(&generic_comm_base->status,
					    PCCS_SCI_DOORBEL))
			return;
	}

	/*
	 * Response message format:
	 * msg[0] is the return code of the operation
	 * msg[1] is the first parameter word
	 * msg[2] is the second parameter word
	 *
	 * As message only supports dword size, just assign it.
	 */

	/* Check for sync query */
	if (ctx->resp_pending &&
	    ((SLIMPRO_MSG_TYPE(((u32 *) msg)[0]) == SLIMPRO_MSG_TYPE_ERR_ID) ||
	     (SLIMPRO_MSG_TYPE(((u32 *) msg)[0]) == SLIMPRO_MSG_TYPE_DBG_ID &&
	      SLIMPRO_MSG_SUBTYPE(((u32 *) msg)[0]) == SLIMPRO_DBG_SUBTYPE_SENSOR_READ) ||
	     (SLIMPRO_MSG_TYPE(((u32 *) msg)[0]) == SLIMPRO_MSG_TYPE_PWRMGMT_ID &&
	      SLIMPRO_MSG_SUBTYPE(((u32 *) msg)[0]) == SLIMPRO_PWRMGMT_SUBTYPE_TPC &&
	      SLIMPRO_TPC_CMD(((u32 *) msg)[0]) == SLIMPRO_TPC_ALARM))) {
		if (ACPI_COMPANION(ctx->dev)) {
			/* Check if platform completes command */
			if (!xgene_word_tst_and_clr(&generic_comm_base->status,
						    PCCS_CMD_COMPLETE))
				goto notify;
		}
		ctx->sync_msg.msg = ((u32 *) msg)[0];
		ctx->sync_msg.param1 = ((u32 *) msg)[1];
		ctx->sync_msg.param2 = ((u32 *) msg)[2];

		/* Operation waiting for response */
		complete(&ctx->rd_complete);

		return;
	}

	/*
	 * If PCC, platform notifies interrupt to OSPM.
	 * OPSM schedules a consumer command to get this information
	 * in a workqueue. Platform must wait until OSPM has issued
	 * a consumer command that serves this notification.
	 */
notify:
	if (ACPI_COMPANION(ctx->dev)) {
		/* Check and clear Platform Notification bit */
		if (!xgene_word_tst_and_clr(&generic_comm_base->status,
					    PCCS_PLATFORM_NOTIFICATION))
			return;
	} else {
		amsg.msg   = ((u32 *) msg)[0];
		amsg.param1 = ((u32 *) msg)[1];
		amsg.param2 = ((u32 *) msg)[2];
	}

	/* Enqueue to the FIFO */
	kfifo_in_spinlocked(&ctx->async_msg_fifo, &amsg,
			    sizeof(struct slimpro_resp_msg), &ctx->lock);
	/* Schedule the bottom handler */
	schedule_work(&ctx->workq);
}

static void xgene_hwmon_tx_done(struct mbox_client *cl, void *msg, int ret)
{
	if (ret) {
		dev_dbg(cl->dev, "TX did not complete: CMD sent:%x, ret:%d\n",
			*(u16 *) msg, ret);
	} else {
		dev_dbg(cl->dev, "TX completed. CMD sent:%x, ret:%d\n",
			*(u16 *) msg, ret);
	}
}

static int __init xgene_hwmon_probe(struct platform_device *pdev)
{
	struct xgene_hwmon_dev *ctx;
	struct mbox_client *cl;
	int rc;

	ctx = devm_kzalloc(&pdev->dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->dev = &pdev->dev;
	platform_set_drvdata(pdev, ctx);
	cl = &ctx->mbox_client;

	/* Request mailbox channel */
	cl->dev = &pdev->dev;
	cl->rx_callback = xgene_hwmon_rx_cb;
	cl->tx_done = xgene_hwmon_tx_done;
	cl->tx_block = false;
	cl->tx_tout = MBOX_OP_TIMEOUTMS;
	cl->knows_txdone = false;
	if (!ACPI_COMPANION(cl->dev)) {
		ctx->mbox_chan = mbox_request_channel(cl, MBOX_HWMON_INDEX);
		if (IS_ERR(ctx->mbox_chan)) {
			dev_err(&pdev->dev,
				"SLIMpro mailbox channel request failed\n");
			return PTR_ERR(ctx->mbox_chan);
		}
	} else {
		struct acpi_pcct_hw_reduced *cppc_ss;

		ctx->mbox_chan =
			pcc_mbox_request_channel(cl, SLIMPRO_MSG_PCC_SUBSPACE);
		if (IS_ERR(ctx->mbox_chan)) {
			dev_err(&pdev->dev,
				"PPC channel request failed\n");
			return PTR_ERR(ctx->mbox_chan);
		}

		/*
		 * The PCC mailbox controller driver should
		 * have parsed the PCCT (global table of all
		 * PCC channels) and stored pointers to the
		 * subspace communication region in con_priv.
		 */
		cppc_ss = ctx->mbox_chan->con_priv;
		if (!cppc_ss) {
			dev_err(&pdev->dev, "PPC subspace not found\n");
			rc = -ENODEV;
			goto out_mbox_free;
		}

		if (!ctx->mbox_chan->mbox->txdone_irq) {
			dev_err(&pdev->dev, "PCC IRQ not supported\n");
			rc = -ENODEV;
			goto out_mbox_free;
		}

		/*
		 * This is the shared communication region
		 * for the OS and Platform to communicate over.
		 */
		ctx->comm_base_addr = cppc_ss->base_address;
		if (ctx->comm_base_addr) {
			ctx->pcc_comm_addr =
					acpi_os_ioremap(ctx->comm_base_addr,
							cppc_ss->length);
		} else {
			dev_err(&pdev->dev, "Failed to get PCC comm region\n");
			rc = -ENODEV;
			goto out_mbox_free;
		}

		if (!ctx->pcc_comm_addr) {
			dev_err(&pdev->dev,
				"Failed to ioremap PCC comm region\n");
			rc = -ENOMEM;
			goto out_mbox_free;
		}

		/*
		 * cppc_ss->latency is just a Nominal value. In reality
		 * the remote processor could be much slower to reply.
		 * So add an arbitrary amount of wait on top of Nominal.
		 */
		ctx->usecs_lat = PCC_NUM_RETRIES * cppc_ss->latency;
	}

	spin_lock_init(&ctx->lock);

	rc = kfifo_alloc(&ctx->async_msg_fifo,
			 sizeof(struct slimpro_resp_msg) * ASYNC_MSG_FIFO_SIZE,
			 GFP_KERNEL);
	if (rc)
		goto out_mbox_free;

	INIT_WORK(&ctx->workq, xgene_hwmon_evt_work);

	/* Hook up sysfs for sensor monitor */
	rc = sysfs_create_group(&pdev->dev.kobj, &xgene_hwmon_attr_group);
	if (rc) {
		dev_err(&pdev->dev, "Fail to create sysfs\n");
		goto out_kfifo_free;
	}

	ctx->hwmon_dev = hwmon_device_register(ctx->dev);
	if (IS_ERR(ctx->hwmon_dev)) {
		dev_err(&pdev->dev, "Failed to register HW monitor device\n");
		rc = PTR_ERR(ctx->hwmon_dev);
		goto out;
	}

	dev_info(&pdev->dev, "APM X-Gene SoC HW monitor driver registered\n");

	return rc;

out:
	sysfs_remove_group(&pdev->dev.kobj, &xgene_hwmon_attr_group);
out_kfifo_free:
	kfifo_free(&ctx->async_msg_fifo);
out_mbox_free:
	mbox_free_channel(ctx->mbox_chan);

	return rc;
}

static int xgene_hwmon_remove(struct platform_device *pdev)
{
	struct xgene_hwmon_dev *ctx = platform_get_drvdata(pdev);

	hwmon_device_unregister(ctx->hwmon_dev);
	sysfs_remove_group(&pdev->dev.kobj, &xgene_hwmon_attr_group);
	kfifo_free(&ctx->async_msg_fifo);
	mbox_free_channel(ctx->mbox_chan);

	return 0;
}

#ifdef CONFIG_ACPI
static const struct acpi_device_id xgene_hwmon_acpi_match[] = {
	{"APMC0D29", 0},
	{},
};
MODULE_DEVICE_TABLE(acpi, xgene_hwmon_acpi_match);
#endif

static const struct of_device_id xgene_hwmon_of_match[] = {
	{.compatible = "apm,xgene-slimpro-hwmon"},
	{}
};
MODULE_DEVICE_TABLE(of, xgene_hwmon_of_match);

static struct platform_driver xgene_hwmon_driver __refdata = {
	.probe = xgene_hwmon_probe,
	.remove = xgene_hwmon_remove,
	.driver = {
		.name = "xgene-slimpro-hwmon",
		.of_match_table = xgene_hwmon_of_match,
		.acpi_match_table = ACPI_PTR(xgene_hwmon_acpi_match),
	},
};
module_platform_driver(xgene_hwmon_driver);

MODULE_DESCRIPTION("APM X-Gene SoC hardware monitor");
MODULE_LICENSE("GPL");
