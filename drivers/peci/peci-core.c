// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2018 Intel Corporation

#include <linux/crc8.h>
#include <linux/fs.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/peci.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

/* Device Specific Completion Code (CC) Definition */
#define DEV_PECI_CC_RETRY_ERR_MASK  0xf0
#define DEV_PECI_CC_SUCCESS         0x40
#define DEV_PECI_CC_TIMEOUT         0x80
#define DEV_PECI_CC_OUT_OF_RESOURCE 0x81
#define DEV_PECI_CC_INVALID_REQ     0x90

/* Skylake EDS says to retry for 250ms */
#define DEV_PECI_RETRY_TIME_MS     250
#define DEV_PECI_RETRY_BIT         0x01

#define GET_TEMP_WR_LEN   1
#define GET_TEMP_RD_LEN   2
#define GET_TEMP_PECI_CMD 0x01

#define GET_DIB_WR_LEN   1
#define GET_DIB_RD_LEN   8
#define GET_DIB_PECI_CMD 0xf7

#define RDPKGCFG_WRITE_LEN     5
#define RDPKGCFG_READ_LEN_BASE 1
#define RDPKGCFG_PECI_CMD      0xa1

#define WRPKGCFG_WRITE_LEN_BASE 6
#define WRPKGCFG_READ_LEN       1
#define WRPKGCFG_PECI_CMD       0xa5

#define RDIAMSR_WRITE_LEN 5
#define RDIAMSR_READ_LEN  9
#define RDIAMSR_PECI_CMD  0xb1

#define WRIAMSR_PECI_CMD  0xb5

#define RDPCICFG_WRITE_LEN 6
#define RDPCICFG_READ_LEN  5
#define RDPCICFG_PECI_CMD  0x61

#define WRPCICFG_PECI_CMD  0x65

#define RDPCICFGLOCAL_WRITE_LEN     5
#define RDPCICFGLOCAL_READ_LEN_BASE 1
#define RDPCICFGLOCAL_PECI_CMD      0xe1

#define WRPCICFGLOCAL_WRITE_LEN_BASE 6
#define WRPCICFGLOCAL_READ_LEN       1
#define WRPCICFGLOCAL_PECI_CMD       0xe5

/* CRC8 table for Assure Write Frame Check */
#define PECI_CRC8_POLYNOMIAL 0x07
DECLARE_CRC8_TABLE(peci_crc8_table);

static struct device_type peci_adapter_type;
static struct device_type peci_client_type;

#define PECI_CDEV_MAX 16
static dev_t peci_devt;
static bool is_registered;

static DEFINE_MUTEX(core_lock);
static DEFINE_IDR(peci_adapter_idr);

static ssize_t name_show(struct device *dev,
			 struct device_attribute *attr,
			 char *buf)
{
	return sprintf(buf, "%s\n", dev->type == &peci_client_type ?
		       to_peci_client(dev)->name : to_peci_adapter(dev)->name);
}
static DEVICE_ATTR_RO(name);

static void peci_client_dev_release(struct device *dev)
{
	kfree(to_peci_client(dev));
}

static struct attribute *peci_device_attrs[] = {
	&dev_attr_name.attr,
	NULL
};
ATTRIBUTE_GROUPS(peci_device);

static struct device_type peci_client_type = {
	.groups		= peci_device_groups,
	.release	= peci_client_dev_release,
};

static struct peci_client *peci_verify_client(struct device *dev)
{
	return (dev->type == &peci_client_type)
			? to_peci_client(dev)
			: NULL;
}

static void peci_adapter_dev_release(struct device *dev)
{
	/* do nothing */
}

static struct attribute *peci_adapter_attrs[] = {
	&dev_attr_name.attr,
	NULL
};
ATTRIBUTE_GROUPS(peci_adapter);

static struct device_type peci_adapter_type = {
	.groups		= peci_adapter_groups,
	.release	= peci_adapter_dev_release,
};

static struct peci_adapter *peci_verify_adapter(struct device *dev)
{
	return (dev->type == &peci_adapter_type)
			? to_peci_adapter(dev)
			: NULL;
}

static struct peci_adapter *peci_get_adapter(int nr)
{
	struct peci_adapter *adapter;

	mutex_lock(&core_lock);
	adapter = idr_find(&peci_adapter_idr, nr);
	if (!adapter)
		goto out_unlock;

	if (try_module_get(adapter->owner))
		get_device(&adapter->dev);
	else
		adapter = NULL;

out_unlock:
	mutex_unlock(&core_lock);
	return adapter;
}

static void peci_put_adapter(struct peci_adapter *adapter)
{
	if (!adapter)
		return;

	put_device(&adapter->dev);
	module_put(adapter->owner);
}

static u8 peci_aw_fcs(u8 *data, int len)
{
	return crc8(peci_crc8_table, data, (size_t)len, 0);
}

static int peci_locked_xfer(struct peci_adapter *adapter,
			    struct peci_xfer_msg *msg,
			    bool do_retry,
			    bool has_aw_fcs)
{
	ktime_t start, end;
	s64 elapsed_ms;
	int rc = 0;

	if (!adapter->xfer) {
		dev_dbg(&adapter->dev, "PECI level transfers not supported\n");
		return -ENODEV;
	}

	if (in_atomic() || irqs_disabled()) {
		rt_mutex_trylock(&adapter->bus_lock);
		if (!rc)
			return -EAGAIN; /* PECI activity is ongoing */
	} else {
		rt_mutex_lock(&adapter->bus_lock);
	}

	if (do_retry)
		start = ktime_get();

	do {
		rc = adapter->xfer(adapter, msg);

		if (!do_retry)
			break;

		/* Per the PECI spec, need to retry commands that return 0x8x */
		if (!(!rc && ((msg->rx_buf[0] & DEV_PECI_CC_RETRY_ERR_MASK) ==
			      DEV_PECI_CC_TIMEOUT)))
			break;

		/* Set the retry bit to indicate a retry attempt */
		msg->tx_buf[1] |= DEV_PECI_RETRY_BIT;

		/* Recalculate the AW FCS if it has one */
		if (has_aw_fcs)
			msg->tx_buf[msg->tx_len - 1] = 0x80 ^
						peci_aw_fcs((u8 *)msg,
							    2 + msg->tx_len);

		/* Retry for at least 250ms before returning an error */
		end = ktime_get();
		elapsed_ms = ktime_to_ms(ktime_sub(end, start));
		if (elapsed_ms >= DEV_PECI_RETRY_TIME_MS) {
			dev_dbg(&adapter->dev, "Timeout retrying xfer!\n");
			break;
		}
	} while (true);

	rt_mutex_unlock(&adapter->bus_lock);

	return rc;
}

static int peci_xfer(struct peci_adapter *adapter, struct peci_xfer_msg *msg)
{
	return peci_locked_xfer(adapter, msg, false, false);
}

static int peci_xfer_with_retries(struct peci_adapter *adapter,
				  struct peci_xfer_msg *msg,
				  bool has_aw_fcs)
{
	return peci_locked_xfer(adapter, msg, true, has_aw_fcs);
}

static int peci_scan_cmd_mask(struct peci_adapter *adapter)
{
	struct peci_xfer_msg msg;
	u32 dib;
	int rc = 0;

	/* Update command mask just once */
	if (adapter->cmd_mask & BIT(PECI_CMD_PING))
		return 0;

	msg.addr      = PECI_BASE_ADDR;
	msg.tx_len    = GET_DIB_WR_LEN;
	msg.rx_len    = GET_DIB_RD_LEN;
	msg.tx_buf[0] = GET_DIB_PECI_CMD;

	rc = peci_xfer(adapter, &msg);
	if (rc < 0) {
		dev_dbg(&adapter->dev, "PECI xfer error, rc : %d\n", rc);
		return rc;
	}

	dib = msg.rx_buf[0] | (msg.rx_buf[1] << 8) |
	      (msg.rx_buf[2] << 16) | (msg.rx_buf[3] << 24);

	/* Check special case for Get DIB command */
	if (dib == 0x00) {
		dev_dbg(&adapter->dev, "DIB read as 0x00\n");
		return -1;
	}

	if (!rc) {
		/**
		 * setting up the supporting commands based on minor rev#
		 * see PECI Spec Table 3-1
		 */
		dib = (dib >> 8) & 0xF;

		if (dib >= 0x1) {
			adapter->cmd_mask |= BIT(PECI_CMD_RD_PKG_CFG);
			adapter->cmd_mask |= BIT(PECI_CMD_WR_PKG_CFG);
		}

		if (dib >= 0x2)
			adapter->cmd_mask |= BIT(PECI_CMD_RD_IA_MSR);

		if (dib >= 0x3) {
			adapter->cmd_mask |= BIT(PECI_CMD_RD_PCI_CFG_LOCAL);
			adapter->cmd_mask |= BIT(PECI_CMD_WR_PCI_CFG_LOCAL);
		}

		if (dib >= 0x4)
			adapter->cmd_mask |= BIT(PECI_CMD_RD_PCI_CFG);

		if (dib >= 0x5)
			adapter->cmd_mask |= BIT(PECI_CMD_WR_PCI_CFG);

		if (dib >= 0x6)
			adapter->cmd_mask |= BIT(PECI_CMD_WR_IA_MSR);

		adapter->cmd_mask |= BIT(PECI_CMD_GET_TEMP);
		adapter->cmd_mask |= BIT(PECI_CMD_GET_DIB);
		adapter->cmd_mask |= BIT(PECI_CMD_PING);
	} else {
		dev_dbg(&adapter->dev, "Error reading DIB, rc : %d\n", rc);
	}

	return rc;
}

static int peci_cmd_support(struct peci_adapter *adapter, enum peci_cmd cmd)
{
	if (!(adapter->cmd_mask & BIT(PECI_CMD_PING)) &&
	    peci_scan_cmd_mask(adapter) < 0) {
		dev_dbg(&adapter->dev, "Failed to scan command mask\n");
		return -EIO;
	}

	if (!(adapter->cmd_mask & BIT(cmd))) {
		dev_dbg(&adapter->dev, "Command %d is not supported\n", cmd);
		return -EINVAL;
	}

	return 0;
}

static int peci_ioctl_ping(struct peci_adapter *adapter, void *vmsg)
{
	struct peci_ping_msg *umsg = vmsg;
	struct peci_xfer_msg msg;
	int rc;

	rc = peci_cmd_support(adapter, PECI_CMD_PING);
	if (rc < 0)
		return rc;

	msg.addr   = umsg->addr;
	msg.tx_len = 0;
	msg.rx_len = 0;

	rc = peci_xfer(adapter, &msg);
	if (rc < 0)
		return rc;

	return 0;
}

static int peci_ioctl_get_dib(struct peci_adapter *adapter, void *vmsg)
{
	struct peci_get_dib_msg *umsg = vmsg;
	struct peci_xfer_msg msg;
	int rc;

	rc = peci_cmd_support(adapter, PECI_CMD_GET_DIB);
	if (rc < 0)
		return rc;

	msg.addr      = umsg->addr;
	msg.tx_len    = GET_DIB_WR_LEN;
	msg.rx_len    = GET_DIB_RD_LEN;
	msg.tx_buf[0] = GET_DIB_PECI_CMD;

	rc = peci_xfer(adapter, &msg);
	if (rc < 0)
		return rc;

	umsg->dib = msg.rx_buf[0] | (msg.rx_buf[1] << 8) |
		     (msg.rx_buf[2] << 16) | (msg.rx_buf[3] << 24);

	return 0;
}

static int peci_ioctl_get_temp(struct peci_adapter *adapter, void *vmsg)
{
	struct peci_get_temp_msg *umsg = vmsg;
	struct peci_xfer_msg msg;
	int rc;

	rc = peci_cmd_support(adapter, PECI_CMD_GET_TEMP);
	if (rc < 0)
		return rc;

	msg.addr      = umsg->addr;
	msg.tx_len    = GET_TEMP_WR_LEN;
	msg.rx_len    = GET_TEMP_RD_LEN;
	msg.tx_buf[0] = GET_TEMP_PECI_CMD;

	rc = peci_xfer(adapter, &msg);
	if (rc < 0)
		return rc;

	umsg->temp_raw = msg.rx_buf[0] | (msg.rx_buf[1] << 8);

	return 0;
}

static int peci_ioctl_rd_pkg_cfg(struct peci_adapter *adapter, void *vmsg)
{
	struct peci_rd_pkg_cfg_msg *umsg = vmsg;
	struct peci_xfer_msg msg;
	int rc = 0;

	/* Per the PECI spec, the read length must be a byte, word, or dword */
	if (umsg->rx_len != 1 && umsg->rx_len != 2 && umsg->rx_len != 4) {
		dev_dbg(&adapter->dev, "Invalid read length, rx_len: %d\n",
			umsg->rx_len);
		return -EINVAL;
	}

	rc = peci_cmd_support(adapter, PECI_CMD_RD_PKG_CFG);
	if (rc < 0)
		return rc;

	msg.addr = umsg->addr;
	msg.tx_len = RDPKGCFG_WRITE_LEN;
	/* read lengths of 1 and 2 result in an error, so only use 4 for now */
	msg.rx_len = RDPKGCFG_READ_LEN_BASE + umsg->rx_len;
	msg.tx_buf[0] = RDPKGCFG_PECI_CMD;
	msg.tx_buf[1] = 0x00;         /* request byte for Host ID / Retry bit */
				      /* Host ID is 0 for PECI 3.0 */
	msg.tx_buf[2] = umsg->index;            /* RdPkgConfig index */
	msg.tx_buf[3] = (u8)umsg->param;        /* LSB - Config parameter */
	msg.tx_buf[4] = (u8)(umsg->param >> 8); /* MSB - Config parameter */

	rc = peci_xfer_with_retries(adapter, &msg, false);
	if (rc || msg.rx_buf[0] != DEV_PECI_CC_SUCCESS) {
		dev_dbg(&adapter->dev, "xfer error, rc : %d\n", rc);
		return -EIO;
	}

	memcpy(umsg->pkg_config, &msg.rx_buf[1], umsg->rx_len);

	return rc;
}

static int peci_ioctl_wr_pkg_cfg(struct peci_adapter *adapter, void *vmsg)
{
	struct peci_wr_pkg_cfg_msg *umsg = vmsg;
	struct peci_xfer_msg msg;
	int rc = 0, i;

	/* Per the PECI spec, the write length must be a dword */
	if (umsg->tx_len != 4) {
		dev_dbg(&adapter->dev, "Invalid write length, tx_len: %d\n",
			umsg->tx_len);
		return -EINVAL;
	}

	rc = peci_cmd_support(adapter, PECI_CMD_WR_PKG_CFG);
	if (rc < 0)
		return rc;

	msg.addr = umsg->addr;
	msg.tx_len = WRPKGCFG_WRITE_LEN_BASE + umsg->tx_len;
	/* read lengths of 1 and 2 result in an error, so only use 4 for now */
	msg.rx_len = WRPKGCFG_READ_LEN;
	msg.tx_buf[0] = WRPKGCFG_PECI_CMD;
	msg.tx_buf[1] = 0x00;         /* request byte for Host ID / Retry bit */
				      /* Host ID is 0 for PECI 3.0 */
	msg.tx_buf[2] = umsg->index;            /* RdPkgConfig index */
	msg.tx_buf[3] = (u8)umsg->param;        /* LSB - Config parameter */
	msg.tx_buf[4] = (u8)(umsg->param >> 8); /* MSB - Config parameter */
	for (i = 0; i < umsg->tx_len; i++)
		msg.tx_buf[5 + i] = (u8)(umsg->value >> (i << 3));

	/* Add an Assure Write Frame Check Sequence byte */
	msg.tx_buf[5 + i] = 0x80 ^
			    peci_aw_fcs((u8 *)&msg, 8 + umsg->tx_len);

	rc = peci_xfer_with_retries(adapter, &msg, true);
	if (rc || msg.rx_buf[0] != DEV_PECI_CC_SUCCESS) {
		dev_dbg(&adapter->dev, "xfer error, rc : %d\n", rc);
		return -EIO;
	}

	return rc;
}

static int peci_ioctl_rd_ia_msr(struct peci_adapter *adapter, void *vmsg)
{
	struct peci_rd_ia_msr_msg *umsg = vmsg;
	struct peci_xfer_msg msg;
	int rc = 0;

	rc = peci_cmd_support(adapter, PECI_CMD_RD_IA_MSR);
	if (rc < 0)
		return rc;

	msg.addr = umsg->addr;
	msg.tx_len = RDIAMSR_WRITE_LEN;
	msg.rx_len = RDIAMSR_READ_LEN;
	msg.tx_buf[0] = RDIAMSR_PECI_CMD;
	msg.tx_buf[1] = 0x00;
	msg.tx_buf[2] = umsg->thread_id;
	msg.tx_buf[3] = (u8)umsg->address;
	msg.tx_buf[4] = (u8)(umsg->address >> 8);

	rc = peci_xfer_with_retries(adapter, &msg, false);
	if (rc || msg.rx_buf[0] != DEV_PECI_CC_SUCCESS) {
		dev_dbg(&adapter->dev, "xfer error, rc : %d\n", rc);
		return -EIO;
	}

	memcpy(&umsg->value, &msg.rx_buf[1], sizeof(uint64_t));

	return rc;
}

static int peci_ioctl_rd_pci_cfg(struct peci_adapter *adapter, void *vmsg)
{
	struct peci_rd_pci_cfg_msg *umsg = vmsg;
	struct peci_xfer_msg msg;
	u32 address;
	int rc = 0;

	rc = peci_cmd_support(adapter, PECI_CMD_RD_PCI_CFG);
	if (rc < 0)
		return rc;

	address = umsg->reg;                  /* [11:0]  - Register */
	address |= (u32)umsg->function << 12; /* [14:12] - Function */
	address |= (u32)umsg->device << 15;   /* [19:15] - Device   */
	address |= (u32)umsg->bus << 20;      /* [27:20] - Bus      */
					      /* [31:28] - Reserved */
	msg.addr = umsg->addr;
	msg.tx_len = RDPCICFG_WRITE_LEN;
	msg.rx_len = RDPCICFG_READ_LEN;
	msg.tx_buf[0] = RDPCICFG_PECI_CMD;
	msg.tx_buf[1] = 0x00;         /* request byte for Host ID / Retry bit */
				      /* Host ID is 0 for PECI 3.0 */
	msg.tx_buf[2] = (u8)address;         /* LSB - PCI Config Address */
	msg.tx_buf[3] = (u8)(address >> 8);  /* PCI Config Address */
	msg.tx_buf[4] = (u8)(address >> 16); /* PCI Config Address */
	msg.tx_buf[5] = (u8)(address >> 24); /* MSB - PCI Config Address */

	rc = peci_xfer_with_retries(adapter, &msg, false);
	if (rc || msg.rx_buf[0] != DEV_PECI_CC_SUCCESS) {
		dev_dbg(&adapter->dev, "xfer error, rc : %d\n", rc);
		return -EIO;
	}

	memcpy(umsg->pci_config, &msg.rx_buf[1], 4);

	return rc;
}

static int peci_ioctl_rd_pci_cfg_local(struct peci_adapter *adapter, void *vmsg)
{
	struct peci_rd_pci_cfg_local_msg *umsg = vmsg;
	struct peci_xfer_msg msg;
	u32 address;
	int rc = 0;

	/* Per the PECI spec, the read length must be a byte, word, or dword */
	if (umsg->rx_len != 1 && umsg->rx_len != 2 && umsg->rx_len != 4) {
		dev_dbg(&adapter->dev, "Invalid read length, rx_len: %d\n",
			umsg->rx_len);
		return -EINVAL;
	}

	rc = peci_cmd_support(adapter, PECI_CMD_RD_PCI_CFG_LOCAL);
	if (rc < 0)
		return rc;

	address = umsg->reg;                  /* [11:0]  - Register */
	address |= (u32)umsg->function << 12; /* [14:12] - Function */
	address |= (u32)umsg->device << 15;   /* [19:15] - Device   */
	address |= (u32)umsg->bus << 20;      /* [23:20] - Bus      */

	msg.addr = umsg->addr;
	msg.tx_len = RDPCICFGLOCAL_WRITE_LEN;
	msg.rx_len = RDPCICFGLOCAL_READ_LEN_BASE + umsg->rx_len;
	msg.tx_buf[0] = RDPCICFGLOCAL_PECI_CMD;
	msg.tx_buf[1] = 0x00;         /* request byte for Host ID / Retry bit */
				      /* Host ID is 0 for PECI 3.0 */
	msg.tx_buf[2] = (u8)address;       /* LSB - PCI Configuration Address */
	msg.tx_buf[3] = (u8)(address >> 8);  /* PCI Configuration Address */
	msg.tx_buf[4] = (u8)(address >> 16); /* PCI Configuration Address */

	rc = peci_xfer_with_retries(adapter, &msg, false);
	if (rc || msg.rx_buf[0] != DEV_PECI_CC_SUCCESS) {
		dev_dbg(&adapter->dev, "xfer error, rc : %d\n", rc);
		return -EIO;
	}

	memcpy(umsg->pci_config, &msg.rx_buf[1], umsg->rx_len);

	return rc;
}

static int peci_ioctl_wr_pci_cfg_local(struct peci_adapter *adapter, void *vmsg)
{
	struct peci_wr_pci_cfg_local_msg *umsg = vmsg;
	struct peci_xfer_msg msg;
	u32 address;
	int rc = 0, i;

	/* Per the PECI spec, the write length must be a byte, word, or dword */
	if (umsg->tx_len != 1 && umsg->tx_len != 2 && umsg->tx_len != 4) {
		dev_dbg(&adapter->dev, "Invalid write length, tx_len: %d\n",
			umsg->tx_len);
		return -EINVAL;
	}

	rc = peci_cmd_support(adapter, PECI_CMD_RD_PCI_CFG_LOCAL);
	if (rc < 0)
		return rc;

	address = umsg->reg;                  /* [11:0]  - Register */
	address |= (u32)umsg->function << 12; /* [14:12] - Function */
	address |= (u32)umsg->device << 15;   /* [19:15] - Device   */
	address |= (u32)umsg->bus << 20;      /* [23:20] - Bus      */

	msg.addr = umsg->addr;
	msg.tx_len = WRPCICFGLOCAL_WRITE_LEN_BASE + umsg->tx_len;
	msg.rx_len = WRPCICFGLOCAL_READ_LEN;
	msg.tx_buf[0] = WRPCICFGLOCAL_PECI_CMD;
	msg.tx_buf[1] = 0x00;         /* request byte for Host ID / Retry bit */
				      /* Host ID is 0 for PECI 3.0 */
	msg.tx_buf[2] = (u8)address;       /* LSB - PCI Configuration Address */
	msg.tx_buf[3] = (u8)(address >> 8);  /* PCI Configuration Address */
	msg.tx_buf[4] = (u8)(address >> 16); /* PCI Configuration Address */
	for (i = 0; i < umsg->tx_len; i++)
		msg.tx_buf[5 + i] = (u8)(umsg->value >> (i << 3));

	/* Add an Assure Write Frame Check Sequence byte */
	msg.tx_buf[5 + i] = 0x80 ^
			    peci_aw_fcs((u8 *)&msg, 8 + umsg->tx_len);

	rc = peci_xfer_with_retries(adapter, &msg, true);
	if (rc || msg.rx_buf[0] != DEV_PECI_CC_SUCCESS) {
		dev_dbg(&adapter->dev, "xfer error, rc : %d\n", rc);
		return -EIO;
	}

	return rc;
}

typedef int (*peci_ioctl_fn_type)(struct peci_adapter *, void *);

static peci_ioctl_fn_type peci_ioctl_fn[PECI_CMD_MAX] = {
	NULL, /* Reserved */
	peci_ioctl_ping,
	peci_ioctl_get_dib,
	peci_ioctl_get_temp,
	peci_ioctl_rd_pkg_cfg,
	peci_ioctl_wr_pkg_cfg,
	peci_ioctl_rd_ia_msr,
	NULL, /* Reserved */
	peci_ioctl_rd_pci_cfg,
	NULL, /* Reserved */
	peci_ioctl_rd_pci_cfg_local,
	peci_ioctl_wr_pci_cfg_local,
};

int peci_command(struct peci_adapter *adapter, enum peci_cmd cmd, void *vmsg)
{
	int ret = 0;

	if (cmd >= PECI_CMD_MAX)
		return -EINVAL;

	dev_dbg(&adapter->dev, "%s, cmd=0x%02x\n", __func__, cmd);

	if (!peci_ioctl_fn[cmd])
		return -EINVAL;

	ret = peci_ioctl_fn[cmd](adapter, vmsg);

	return ret;
}
EXPORT_SYMBOL_GPL(peci_command);

static long peci_ioctl(struct file *file, unsigned int iocmd, unsigned long arg)
{
	struct peci_adapter *adapter = file->private_data;
	void __user *argp = (void __user *)arg;
	unsigned int msg_len;
	enum peci_cmd cmd;
	u8 *msg;
	int rc = 0;

	dev_dbg(&adapter->dev, "ioctl, cmd=0x%x, arg=0x%lx\n", iocmd, arg);

	switch (iocmd) {
	case PECI_IOC_PING:
	case PECI_IOC_GET_DIB:
	case PECI_IOC_GET_TEMP:
	case PECI_IOC_RD_PKG_CFG:
	case PECI_IOC_WR_PKG_CFG:
	case PECI_IOC_RD_IA_MSR:
	case PECI_IOC_RD_PCI_CFG:
	case PECI_IOC_RD_PCI_CFG_LOCAL:
	case PECI_IOC_WR_PCI_CFG_LOCAL:
		cmd = _IOC_TYPE(iocmd) - PECI_IOC_BASE;
		msg_len = _IOC_SIZE(iocmd);
		break;

	default:
		dev_dbg(&adapter->dev, "Invalid ioctl cmd : 0x%x\n", iocmd);
		return -EINVAL;
	}

	if (!msg_len)
		return -EINVAL;

	msg = memdup_user(argp, msg_len);
	if (IS_ERR(msg))
		return PTR_ERR(msg);

	rc = peci_command(adapter, cmd, msg);

	if (!rc && copy_to_user(argp, msg, msg_len))
		rc = -EFAULT;

	kfree(msg);
	return (long)rc;
}

static int peci_open(struct inode *inode, struct file *file)
{
	unsigned int minor = iminor(inode);
	struct peci_adapter *adapter;

	adapter = peci_get_adapter(minor);
	if (!adapter)
		return -ENODEV;

	file->private_data = adapter;

	return 0;
}

static int peci_release(struct inode *inode, struct file *file)
{
	struct peci_adapter *adapter = file->private_data;

	peci_put_adapter(adapter);
	file->private_data = NULL;

	return 0;
}

static const struct file_operations peci_fops = {
	.owner          = THIS_MODULE,
	.unlocked_ioctl = peci_ioctl,
	.open           = peci_open,
	.release        = peci_release,
};

static int peci_detect(struct peci_adapter *adapter, u8 addr)
{
	struct peci_xfer_msg msg;
	int rc;

	rc = peci_cmd_support(adapter, PECI_CMD_PING);
	if (rc < 0)
		return rc;

	msg.addr   = addr;
	msg.tx_len = 0;
	msg.rx_len = 0;

	rc = peci_xfer(adapter, &msg);
	if (rc < 0)
		return rc;

	return 0;
}

#if IS_ENABLED(CONFIG_OF)
static const struct of_device_id *
peci_of_match_device(const struct of_device_id *matches,
		     struct peci_client *client)
{
	if (!(client && matches))
		return NULL;

	return of_match_device(matches, &client->dev);
}
#endif

const struct peci_device_id *peci_match_id(const struct peci_device_id *id,
					   struct peci_client *client)
{
	if (!(id && client))
		return NULL;

	while (id->name[0]) {
		if (strcmp(client->name, id->name) == 0)
			return id;
		id++;
	}

	return NULL;
}

static int peci_device_match(struct device *dev, struct device_driver *drv)
{
	struct peci_client *client = peci_verify_client(dev);
	struct peci_driver *driver;

	/* Attempt an OF style match */
	if (peci_of_match_device(drv->of_match_table, client))
		return 1;

	driver = to_peci_driver(drv);

	if (peci_match_id(driver->id_table, client))
		return 1;

	return 0;
}

static int peci_device_probe(struct device *dev)
{
	struct peci_client	*client = peci_verify_client(dev);
	struct peci_driver	*driver;
	int status = -EINVAL;

	if (!client)
		return 0;

	if (!peci_of_match_device(dev->driver->of_match_table, client))
		return -ENODEV;

	dev_dbg(dev, "%s: name:%s\n", __func__, client->name);

	driver = to_peci_driver(dev->driver);
	if (driver->probe)
		status = driver->probe(client);

	return status;
}

static int peci_device_remove(struct device *dev)
{
	struct peci_client	*client = peci_verify_client(dev);
	struct peci_driver	*driver;
	int status = 0;

	if (!client || !dev->driver)
		return 0;

	driver = to_peci_driver(dev->driver);
	if (driver->remove) {
		dev_dbg(dev, "%s: name:%s\n", __func__, client->name);
		status = driver->remove(client);
	}

	return status;
}

static void peci_device_shutdown(struct device *dev)
{
	struct peci_client *client = peci_verify_client(dev);
	struct peci_driver *driver;

	if (!client || !dev->driver)
		return;

	dev_dbg(dev, "%s: name:%s\n", __func__, client->name);

	driver = to_peci_driver(dev->driver);
	if (driver->shutdown)
		driver->shutdown(client);
}

static struct bus_type peci_bus_type = {
	.name		= "peci",
	.match		= peci_device_match,
	.probe		= peci_device_probe,
	.remove		= peci_device_remove,
	.shutdown	= peci_device_shutdown,
};

static void peci_unregister_device(struct peci_client *client)
{
	if (client->dev.of_node)
		of_node_clear_flag(client->dev.of_node, OF_POPULATED);

	device_unregister(&client->dev);
}

static int peci_check_addr_validity(u8 addr)
{
	if (addr < PECI_BASE_ADDR && addr > PECI_BASE_ADDR + PECI_OFFSET_MAX)
		return -EINVAL;

	return 0;
}

static int peci_check_addr_busy(struct device *dev, void *addrp)
{
	struct peci_client *client = peci_verify_client(dev);
	u8 addr = *(u8 *)addrp;

	if (client && client->addr == addr)
		return -EBUSY;

	return 0;
}

static struct peci_client *peci_new_device(struct peci_adapter *adapter,
					   struct peci_board_info const *info)
{
	struct peci_client *client;
	int rc;

	client = kzalloc(sizeof(*client), GFP_KERNEL);
	if (!client)
		return NULL;

	client->adapter = adapter;
	client->addr = info->addr;
	strlcpy(client->name, info->type, sizeof(client->name));

	rc = peci_check_addr_validity(client->addr);
	if (rc) {
		dev_err(&adapter->dev, "Invalid PECI CPU address 0x%02hx\n",
			client->addr);
		goto err_free_client_silent;
	}

	/* Check for address business */
	rc = device_for_each_child(&adapter->dev, &client->addr,
				   peci_check_addr_busy);
	if (rc)
		goto err_free_client;

	/* Check client's online status */
	rc = peci_detect(adapter, client->addr);
	if (rc)
		goto err_free_client;

	client->dev.parent = &client->adapter->dev;
	client->dev.bus = &peci_bus_type;
	client->dev.type = &peci_client_type;
	client->dev.of_node = info->of_node;
	dev_set_name(&client->dev, "%d-%02x", adapter->nr, client->addr);
	rc = device_register(&client->dev);
	if (rc)
		goto err_free_client;

	dev_dbg(&adapter->dev, "client [%s] registered with bus id %s\n",
		client->name, dev_name(&client->dev));

	return client;

err_free_client:
	dev_err(&adapter->dev,
		"Failed to register peci client %s at 0x%02x (%d)\n",
		client->name, client->addr, rc);
err_free_client_silent:
	kfree(client);
	return NULL;
}

#if IS_ENABLED(CONFIG_OF)
static struct peci_client *peci_of_register_device(struct peci_adapter *adapter,
						   struct device_node *node)
{
	struct peci_client *result;
	struct peci_board_info info = {};
	const __be32 *addr_be;
	u32 addr;
	int len;

	dev_dbg(&adapter->dev, "register %s\n", node->full_name);

	if (of_modalias_node(node, info.type, sizeof(info.type)) < 0) {
		dev_err(&adapter->dev, "modalias failure on %s\n",
			node->full_name);
		return ERR_PTR(-EINVAL);
	}

	addr_be = of_get_property(node, "reg", &len);
	if (!addr_be || len < sizeof(*addr_be)) {
		dev_err(&adapter->dev, "invalid reg on %s\n",
			node->full_name);
		return ERR_PTR(-EINVAL);
	}

	addr = be32_to_cpup(addr_be);

	if (peci_check_addr_validity(addr)) {
		dev_err(&adapter->dev, "invalid addr=%x on %s\n",
			addr, node->full_name);
		return ERR_PTR(-EINVAL);
	}

	info.addr = addr;
	info.of_node = of_node_get(node);

	result = peci_new_device(adapter, &info);
	if (!result)
		result = ERR_PTR(-EINVAL);

	of_node_put(node);
	return result;
}

static void peci_of_register_devices(struct peci_adapter *adapter)
{
	struct device_node *bus, *node;
	struct peci_client *client;

	/* Only register child devices if the adapter has a node pointer set */
	if (!adapter->dev.of_node)
		return;

	bus = of_get_child_by_name(adapter->dev.of_node, "peci-bus");
	if (!bus)
		bus = of_node_get(adapter->dev.of_node);

	for_each_available_child_of_node(bus, node) {
		if (of_node_test_and_set_flag(node, OF_POPULATED))
			continue;

		client = peci_of_register_device(adapter, node);
		if (IS_ERR(client)) {
			dev_warn(&adapter->dev,
				 "Failed to create PECI device for %s\n",
				 node->full_name);
			of_node_clear_flag(node, OF_POPULATED);
		}
	}

	of_node_put(bus);
}

static int peci_of_match_node(struct device *dev, void *data)
{
	return dev->of_node == data;
}

/* must call put_device() when done with returned peci_client device */
static struct peci_client *peci_of_find_device(struct device_node *node)
{
	struct device *dev;
	struct peci_client *client;

	dev = bus_find_device(&peci_bus_type, NULL, node, peci_of_match_node);
	if (!dev)
		return NULL;

	client = peci_verify_client(dev);
	if (!client)
		put_device(dev);

	return client;
}

/* must call put_device() when done with returned peci_adapter device */
static struct peci_adapter *peci_of_find_adapter(struct device_node *node)
{
	struct device *dev;
	struct peci_adapter *adapter;

	dev = bus_find_device(&peci_bus_type, NULL, node, peci_of_match_node);
	if (!dev)
		return NULL;

	adapter = peci_verify_adapter(dev);
	if (!adapter)
		put_device(dev);

	return adapter;
}
#else
static void peci_of_register_devices(struct peci_adapter *adapter) { }
#endif /* CONFIG_OF */

#if IS_ENABLED(CONFIG_OF_DYNAMIC)
static int peci_of_notify(struct notifier_block *nb,
			  unsigned long action,
			  void *arg)
{
	struct of_reconfig_data *rd = arg;
	struct peci_adapter *adapter;
	struct peci_client *client;

	switch (of_reconfig_get_state_change(action, rd)) {
	case OF_RECONFIG_CHANGE_ADD:
		adapter = peci_of_find_adapter(rd->dn->parent);
		if (!adapter)
			return NOTIFY_OK;	/* not for us */

		if (of_node_test_and_set_flag(rd->dn, OF_POPULATED)) {
			put_device(&adapter->dev);
			return NOTIFY_OK;
		}

		client = peci_of_register_device(adapter, rd->dn);
		put_device(&adapter->dev);

		if (IS_ERR(client)) {
			dev_err(&adapter->dev,
				"failed to create client for '%s'\n",
				rd->dn->full_name);
			of_node_clear_flag(rd->dn, OF_POPULATED);
			return notifier_from_errno(PTR_ERR(client));
		}
		break;
	case OF_RECONFIG_CHANGE_REMOVE:
		/* already depopulated? */
		if (!of_node_check_flag(rd->dn, OF_POPULATED))
			return NOTIFY_OK;

		/* find our device by node */
		client = peci_of_find_device(rd->dn);
		if (!client)
			return NOTIFY_OK;	/* no? not meant for us */

		/* unregister takes one ref away */
		peci_unregister_device(client);

		/* and put the reference of the find */
		put_device(&client->dev);
		break;
	}

	return NOTIFY_OK;
}

static struct notifier_block peci_of_notifier = {
	.notifier_call = peci_of_notify,
};
#else
extern struct notifier_block peci_of_notifier;
#endif /* CONFIG_OF_DYNAMIC */

static int peci_register_adapter(struct peci_adapter *adapter)
{
	int res = -EINVAL;

	/* Can't register until after driver model init */
	if (WARN_ON(!is_registered)) {
		res = -EAGAIN;
		goto err_free_idr;
	}

	if (WARN(!adapter->name[0], "peci adapter has no name"))
		goto err_free_idr;

	rt_mutex_init(&adapter->bus_lock);

	dev_set_name(&adapter->dev, "peci%d", adapter->nr);
	adapter->dev.bus = &peci_bus_type;
	adapter->dev.type = &peci_adapter_type;
	device_initialize(&adapter->dev);

	/* cdev */
	cdev_init(&adapter->cdev, &peci_fops);
	adapter->cdev.owner = THIS_MODULE;
	adapter->cdev.kobj.parent = &adapter->dev.kobj;
	adapter->dev.devt = MKDEV(MAJOR(peci_devt), adapter->nr);
	res = cdev_add(&adapter->cdev, adapter->dev.devt, 1);
	if (res) {
		pr_err("adapter '%s': can't add cdev (%d)\n",
		       adapter->name, res);
		goto err_free_idr;
	}
	res = device_add(&adapter->dev);
	if (res) {
		pr_err("adapter '%s': can't add device (%d)\n",
		       adapter->name, res);
		goto err_del_cdev;
	}

	dev_dbg(&adapter->dev, "adapter [%s] registered\n", adapter->name);

	/* create pre-declared device nodes */
	peci_of_register_devices(adapter);

	return 0;

err_del_cdev:
	cdev_del(&adapter->cdev);
err_free_idr:
	mutex_lock(&core_lock);
	idr_remove(&peci_adapter_idr, adapter->nr);
	mutex_unlock(&core_lock);
	return res;
}

static int peci_add_numbered_adapter(struct peci_adapter *adapter)
{
	int id;

	mutex_lock(&core_lock);
	id = idr_alloc(&peci_adapter_idr, adapter,
		       adapter->nr, adapter->nr + 1, GFP_KERNEL);
	mutex_unlock(&core_lock);
	if (WARN(id < 0, "couldn't get idr"))
		return id == -ENOSPC ? -EBUSY : id;

	return peci_register_adapter(adapter);
}

int peci_add_adapter(struct peci_adapter *adapter)
{
	struct device *dev = &adapter->dev;
	int id;

	if (dev->of_node) {
		id = of_alias_get_id(dev->of_node, "peci");
		if (id >= 0) {
			adapter->nr = id;
			return peci_add_numbered_adapter(adapter);
		}
	}

	mutex_lock(&core_lock);
	id = idr_alloc(&peci_adapter_idr, adapter, 0, 0, GFP_KERNEL);
	mutex_unlock(&core_lock);
	if (WARN(id < 0, "couldn't get idr"))
		return id;

	adapter->nr = id;

	return peci_register_adapter(adapter);
}
EXPORT_SYMBOL_GPL(peci_add_adapter);

static int peci_unregister_client(struct device *dev, void *dummy)
{
	struct peci_client *client = peci_verify_client(dev);

	if (client)
		peci_unregister_device(client);

	return 0;
}

void peci_del_adapter(struct peci_adapter *adapter)
{
	struct peci_adapter *found;

	/* First make sure that this adapter was ever added */
	mutex_lock(&core_lock);
	found = idr_find(&peci_adapter_idr, adapter->nr);
	mutex_unlock(&core_lock);

	if (found != adapter)
		return;

	/**
	 * Detach any active clients. This can't fail, thus we do not
	 * check the returned value.
	 */
	device_for_each_child(&adapter->dev, NULL, peci_unregister_client);

	/* device name is gone after device_unregister */
	dev_dbg(&adapter->dev, "adapter [%s] unregistered\n", adapter->name);

	device_unregister(&adapter->dev);

	/* free cdev */
	cdev_del(&adapter->cdev);

	/* free bus id */
	mutex_lock(&core_lock);
	idr_remove(&peci_adapter_idr, adapter->nr);
	mutex_unlock(&core_lock);
}
EXPORT_SYMBOL_GPL(peci_del_adapter);

/**
 * A peci_driver is used with one or more peci_client (device) nodes to access
 * peci clients, on a bus instance associated with some peci_adapter.
 */
int peci_register_driver(struct module *owner, struct peci_driver *driver)
{
	int res;

	/* Can't register until after driver model init */
	if (WARN_ON(!is_registered))
		return -EAGAIN;

	/* add the driver to the list of peci drivers in the driver core */
	driver->driver.owner = owner;
	driver->driver.bus = &peci_bus_type;

	/**
	 * When registration returns, the driver core
	 * will have called probe() for all matching-but-unbound devices.
	 */
	res = driver_register(&driver->driver);
	if (res)
		return res;

	pr_debug("driver [%s] registered\n", driver->driver.name);

	return 0;
}
EXPORT_SYMBOL_GPL(peci_register_driver);

void peci_del_driver(struct peci_driver *driver)
{
	driver_unregister(&driver->driver);
	pr_debug("driver [%s] unregistered\n", driver->driver.name);
}
EXPORT_SYMBOL_GPL(peci_del_driver);

static int __init peci_init(void)
{
	int ret;

	ret = bus_register(&peci_bus_type);
	if (ret < 0) {
		pr_err("peci: Failed to register PECI bus type!\n");
		return ret;
	}

	ret = alloc_chrdev_region(&peci_devt, 0, PECI_CDEV_MAX, "peci");
	if (ret < 0) {
		pr_err("peci: Failed to allocate chr dev region!\n");
		bus_unregister(&peci_bus_type);
		return ret;
	}

	crc8_populate_msb(peci_crc8_table, PECI_CRC8_POLYNOMIAL);

	if (IS_ENABLED(CONFIG_OF_DYNAMIC))
		WARN_ON(of_reconfig_notifier_register(&peci_of_notifier));

	is_registered = true;

	return 0;
}

static void __exit peci_exit(void)
{
	if (IS_ENABLED(CONFIG_OF_DYNAMIC))
		WARN_ON(of_reconfig_notifier_unregister(&peci_of_notifier));

	unregister_chrdev_region(peci_devt, PECI_CDEV_MAX);
	bus_unregister(&peci_bus_type);
}

postcore_initcall(peci_init);
module_exit(peci_exit);

MODULE_AUTHOR("Jae Hyun Yoo <jae.hyun.yoo@linux.intel.com>");
MODULE_DESCRIPTION("PECI bus core module");
MODULE_LICENSE("GPL v2");
