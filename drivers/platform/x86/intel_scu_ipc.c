/*
 * intel_scu_ipc.c: Driver for the Intel SCU IPC mechanism
 *
 * (C) Copyright 2008-2010,2015 Intel Corporation
 * Author: Sreedhara DS (sreedhara.ds@intel.com)
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; version 2
 * of the License.
 *
 * SCU running in ARC processor communicates with other entity running in IA
 * core through IPC mechanism which in turn messaging between IA core ad SCU.
 * SCU has two IPC mechanism IPC-1 and IPC-2. IPC-1 is used between IA32 and
 * SCU where IPC-2 is used between P-Unit and SCU. This driver delas with
 * IPC-1 Driver provides an API for power control unit registers (e.g. MSIC)
 * along with other APIs.
 */
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/device.h>
#include <linux/pm.h>
#include <linux/pci.h>
#include <linux/interrupt.h>
#include <linux/sfi.h>
#include <linux/regmap.h>
#include <linux/platform_data/x86/intel_ipc_dev.h>
#include <asm/intel-mid.h>
#include <asm/intel_scu_ipc.h>

/* IPC defines the following message types */
#define IPCMSG_WATCHDOG_TIMER 0xF8 /* Set Kernel Watchdog Threshold */
#define IPCMSG_BATTERY        0xEF /* Coulomb Counter Accumulator */
#define IPCMSG_FW_UPDATE      0xFE /* Firmware update */
#define IPCMSG_PCNTRL         0xFF /* Power controller unit read/write */
#define IPCMSG_FW_REVISION    0xF4 /* Get firmware revision */

/* Command id associated with message IPCMSG_PCNTRL */
#define IPC_CMD_PCNTRL_W      0 /* Register write */
#define IPC_CMD_PCNTRL_R      1 /* Register read */
#define IPC_CMD_PCNTRL_M      2 /* Register read-modify-write */

/* IPC dev register offsets */
/*
 * IPC Read Buffer (Read Only):
 * 16 byte buffer for receiving data from SCU, if IPC command
 * processing results in response data
 */
#define IPC_DEV_SCU_RBUF_OFFSET			0x90
#define IPC_DEV_SCU_WRBUF_OFFSET		0x80
#define IPC_DEV_SCU_SPTR_OFFSET			0x08
#define IPC_DEV_SCU_DPTR_OFFSET			0x0C
#define IPC_DEV_SCU_STATUS_OFFSET		0x04

/* IPC dev commands */
/* IPC command register IOC bit */
#define	IPC_DEV_SCU_CMD_MSI			BIT(8)
#define	IPC_DEV_SCU_CMD_STATUS_ERR		BIT(1)
#define	IPC_DEV_SCU_CMD_STATUS_ERR_MASK		GENMASK(7, 0)
#define	IPC_DEV_SCU_CMD_STATUS_BUSY		BIT(0)

/*
 * IPC register summary
 *
 * IPC register blocks are memory mapped at fixed address of PCI BAR 0.
 * To read or write information to the SCU, driver writes to IPC-1 memory
 * mapped registers. The following is the IPC mechanism
 *
 * 1. IA core cDMI interface claims this transaction and converts it to a
 *    Transaction Layer Packet (TLP) message which is sent across the cDMI.
 *
 * 2. South Complex cDMI block receives this message and writes it to
 *    the IPC-1 register block, causing an interrupt to the SCU
 *
 * 3. SCU firmware decodes this interrupt and IPC message and the appropriate
 *    message handler is called within firmware.
 */

#define IPC_WWBUF_SIZE    20		/* IPC Write buffer Size */
#define IPC_RWBUF_SIZE    20		/* IPC Read buffer Size */
#define IPC_IOC	          0x100		/* IPC command register IOC bit */
#define	IPC_CMD_SIZE            16
#define	IPC_CMD_SUBCMD          12
#define	IPC_RWBUF_SIZE_DWORD    5
#define	IPC_WWBUF_SIZE_DWORD    5


#define PCI_DEVICE_ID_LINCROFT		0x082a
#define PCI_DEVICE_ID_PENWELL		0x080e
#define PCI_DEVICE_ID_CLOVERVIEW	0x08ea
#define PCI_DEVICE_ID_TANGIER		0x11a0

/* intel scu ipc driver data */
struct intel_scu_ipc_pdata_t {
	u32 i2c_base;
	u32 i2c_len;
	u8 irq_mode;
};

static const struct intel_scu_ipc_pdata_t intel_scu_ipc_lincroft_pdata = {
	.i2c_base = 0xff12b000,
	.i2c_len = 0x10,
	.irq_mode = 0,
};

/* Penwell and Cloverview */
static const struct intel_scu_ipc_pdata_t intel_scu_ipc_penwell_pdata = {
	.i2c_base = 0xff12b000,
	.i2c_len = 0x10,
	.irq_mode = 1,
};

static const struct intel_scu_ipc_pdata_t intel_scu_ipc_tangier_pdata = {
	.i2c_base  = 0xff00d000,
	.i2c_len = 0x10,
	.irq_mode = 0,
};

struct intel_scu_ipc_dev {
	struct device *dev;
	struct intel_ipc_dev *ipc_dev;
	void __iomem *ipc_base;
	void __iomem *i2c_base;
	struct regmap *ipc_regs;
	struct regmap *i2c_regs;
	u8 irq_mode;
};

static struct regmap_config ipc_regmap_config = {
        .reg_bits = 32,
        .reg_stride = 4,
        .val_bits = 32,
};

static struct regmap_config i2c_regmap_config = {
        .reg_bits = 32,
        .reg_stride = 4,
        .val_bits = 32,
	.fast_io = true,
};

static struct intel_scu_ipc_dev  ipcdev; /* Only one for now */

#define IPC_I2C_CNTRL_ADDR	0
#define I2C_DATA_ADDR		0x04

/* Read/Write power control(PMIC in Langwell, MSIC in PenWell) registers */
static int pwr_reg_rdwr(u16 *addr, u8 *data, u32 count, u32 op, u32 id)
{
	struct intel_scu_ipc_dev *scu = &ipcdev;
	int nc;
	u32 offset = 0;
	int err = -EIO;
	u8 cbuf[IPC_WWBUF_SIZE];
	u32 *wbuf = (u32 *)&cbuf;
	u32 cmd[SCU_PARAM_LEN] = {0};
	/* max rbuf size is 20 bytes */
	u8 rbuf[IPC_RWBUF_SIZE] = {0};
	u32 rbuflen = DIV_ROUND_UP(count, 4);

	memset(cbuf, 0, sizeof(cbuf));

	scu_cmd_init(cmd, op, id);

	for (nc = 0; nc < count; nc++, offset += 2) {
		cbuf[offset] = addr[nc];
		cbuf[offset + 1] = addr[nc] >> 8;
	}

	if (id == IPC_CMD_PCNTRL_R) {
		err = ipc_dev_raw_cmd(scu->ipc_dev, cmd, SCU_PARAM_LEN,
				(u8 *)wbuf, count * 2, (u32 *)rbuf,
				IPC_RWBUF_SIZE_DWORD, 0, 0);
	} else if (id == IPC_CMD_PCNTRL_W) {
		for (nc = 0; nc < count; nc++, offset += 1)
			cbuf[offset] = data[nc];
		err = ipc_dev_raw_cmd(scu->ipc_dev, cmd, SCU_PARAM_LEN,
				(u8 *)wbuf, count * 3, NULL, 0, 0, 0);

	} else if (id == IPC_CMD_PCNTRL_M) {
		cbuf[offset] = data[0];
		cbuf[offset + 1] = data[1];
		err = ipc_dev_raw_cmd(scu->ipc_dev, cmd, SCU_PARAM_LEN,
				(u8 *)wbuf, 4, NULL, 0, 0, 0);
	}

	if (!err && id == IPC_CMD_PCNTRL_R) { /* Read rbuf */
		/* Workaround: values are read as 0 without memcpy_fromio */
		memcpy_fromio(cbuf, scu->ipc_base + 0x90, 16);
		regmap_bulk_read(scu->ipc_regs, IPC_DEV_SCU_RBUF_OFFSET,
					rbuf, rbuflen);
		memcpy(data, rbuf, count);
	}

	return err;
}

/**
 *	intel_scu_ipc_ioread8		-	read a word via the SCU
 *	@addr: register on SCU
 *	@data: return pointer for read byte
 *
 *	Read a single register. Returns 0 on success or an error code. All
 *	locking between SCU accesses is handled for the caller.
 *
 *	This function may sleep.
 */
int intel_scu_ipc_ioread8(u16 addr, u8 *data)
{
	return pwr_reg_rdwr(&addr, data, 1, IPCMSG_PCNTRL, IPC_CMD_PCNTRL_R);
}
EXPORT_SYMBOL(intel_scu_ipc_ioread8);

/**
 *	intel_scu_ipc_ioread16		-	read a word via the SCU
 *	@addr: register on SCU
 *	@data: return pointer for read word
 *
 *	Read a register pair. Returns 0 on success or an error code. All
 *	locking between SCU accesses is handled for the caller.
 *
 *	This function may sleep.
 */
int intel_scu_ipc_ioread16(u16 addr, u16 *data)
{
	u16 x[2] = {addr, addr + 1};
	return pwr_reg_rdwr(x, (u8 *)data, 2, IPCMSG_PCNTRL, IPC_CMD_PCNTRL_R);
}
EXPORT_SYMBOL(intel_scu_ipc_ioread16);

/**
 *	intel_scu_ipc_ioread32		-	read a dword via the SCU
 *	@addr: register on SCU
 *	@data: return pointer for read dword
 *
 *	Read four registers. Returns 0 on success or an error code. All
 *	locking between SCU accesses is handled for the caller.
 *
 *	This function may sleep.
 */
int intel_scu_ipc_ioread32(u16 addr, u32 *data)
{
	u16 x[4] = {addr, addr + 1, addr + 2, addr + 3};
	return pwr_reg_rdwr(x, (u8 *)data, 4, IPCMSG_PCNTRL, IPC_CMD_PCNTRL_R);
}
EXPORT_SYMBOL(intel_scu_ipc_ioread32);

/**
 *	intel_scu_ipc_iowrite8		-	write a byte via the SCU
 *	@addr: register on SCU
 *	@data: byte to write
 *
 *	Write a single register. Returns 0 on success or an error code. All
 *	locking between SCU accesses is handled for the caller.
 *
 *	This function may sleep.
 */
int intel_scu_ipc_iowrite8(u16 addr, u8 data)
{
	return pwr_reg_rdwr(&addr, &data, 1, IPCMSG_PCNTRL, IPC_CMD_PCNTRL_W);
}
EXPORT_SYMBOL(intel_scu_ipc_iowrite8);

/**
 *	intel_scu_ipc_iowrite16		-	write a word via the SCU
 *	@addr: register on SCU
 *	@data: word to write
 *
 *	Write two registers. Returns 0 on success or an error code. All
 *	locking between SCU accesses is handled for the caller.
 *
 *	This function may sleep.
 */
int intel_scu_ipc_iowrite16(u16 addr, u16 data)
{
	u16 x[2] = {addr, addr + 1};
	return pwr_reg_rdwr(x, (u8 *)&data, 2, IPCMSG_PCNTRL, IPC_CMD_PCNTRL_W);
}
EXPORT_SYMBOL(intel_scu_ipc_iowrite16);

/**
 *	intel_scu_ipc_iowrite32		-	write a dword via the SCU
 *	@addr: register on SCU
 *	@data: dword to write
 *
 *	Write four registers. Returns 0 on success or an error code. All
 *	locking between SCU accesses is handled for the caller.
 *
 *	This function may sleep.
 */
int intel_scu_ipc_iowrite32(u16 addr, u32 data)
{
	u16 x[4] = {addr, addr + 1, addr + 2, addr + 3};
	return pwr_reg_rdwr(x, (u8 *)&data, 4, IPCMSG_PCNTRL, IPC_CMD_PCNTRL_W);
}
EXPORT_SYMBOL(intel_scu_ipc_iowrite32);

/**
 *	intel_scu_ipc_readvv		-	read a set of registers
 *	@addr: register list
 *	@data: bytes to return
 *	@len: length of array
 *
 *	Read registers. Returns 0 on success or an error code. All
 *	locking between SCU accesses is handled for the caller.
 *
 *	The largest array length permitted by the hardware is 5 items.
 *
 *	This function may sleep.
 */
int intel_scu_ipc_readv(u16 *addr, u8 *data, int len)
{
	return pwr_reg_rdwr(addr, data, len, IPCMSG_PCNTRL, IPC_CMD_PCNTRL_R);
}
EXPORT_SYMBOL(intel_scu_ipc_readv);

/**
 *	intel_scu_ipc_writev		-	write a set of registers
 *	@addr: register list
 *	@data: bytes to write
 *	@len: length of array
 *
 *	Write registers. Returns 0 on success or an error code. All
 *	locking between SCU accesses is handled for the caller.
 *
 *	The largest array length permitted by the hardware is 5 items.
 *
 *	This function may sleep.
 *
 */
int intel_scu_ipc_writev(u16 *addr, u8 *data, int len)
{
	return pwr_reg_rdwr(addr, data, len, IPCMSG_PCNTRL, IPC_CMD_PCNTRL_W);
}
EXPORT_SYMBOL(intel_scu_ipc_writev);

/**
 *	intel_scu_ipc_update_register	-	r/m/w a register
 *	@addr: register address
 *	@bits: bits to update
 *	@mask: mask of bits to update
 *
 *	Read-modify-write power control unit register. The first data argument
 *	must be register value and second is mask value
 *	mask is a bitmap that indicates which bits to update.
 *	0 = masked. Don't modify this bit, 1 = modify this bit.
 *	returns 0 on success or an error code.
 *
 *	This function may sleep. Locking between SCU accesses is handled
 *	for the caller.
 */
int intel_scu_ipc_update_register(u16 addr, u8 bits, u8 mask)
{
	u8 data[2] = { bits, mask };
	return pwr_reg_rdwr(&addr, data, 1, IPCMSG_PCNTRL, IPC_CMD_PCNTRL_M);
}
EXPORT_SYMBOL(intel_scu_ipc_update_register);

/* I2C commands */
#define IPC_I2C_WRITE 1 /* I2C Write command */
#define IPC_I2C_READ  2 /* I2C Read command */

/**
 *	intel_scu_ipc_i2c_cntrl		-	I2C read/write operations
 *	@addr: I2C address + command bits
 *	@data: data to read/write
 *
 *	Perform an an I2C read/write operation via the SCU. All locking is
 *	handled for the caller. This function may sleep.
 *
 *	Returns an error code or 0 on success.
 *
 *	This has to be in the IPC driver for the locking.
 */
int intel_scu_ipc_i2c_cntrl(u32 addr, u32 *data)
{
	struct intel_scu_ipc_dev *scu = &ipcdev;
	u32 cmd = 0;

	cmd = (addr >> 24) & 0xFF;
	if (cmd == IPC_I2C_READ) {
		regmap_write(scu->i2c_regs, IPC_I2C_CNTRL_ADDR, addr);
		/* Write not getting updated without delay */
		mdelay(1);
		regmap_read(scu->i2c_regs, I2C_DATA_ADDR, data);
	} else if (cmd == IPC_I2C_WRITE) {
		regmap_write(scu->i2c_regs, I2C_DATA_ADDR, *data);
		mdelay(1);
		regmap_write(scu->i2c_regs, IPC_I2C_CNTRL_ADDR, addr);
	} else {
		dev_err(scu->dev,
			"intel_scu_ipc: I2C INVALID_CMD = 0x%x\n", cmd);

		return -EIO;
	}
	return 0;
}
EXPORT_SYMBOL(intel_scu_ipc_i2c_cntrl);

static int pre_simple_cmd_fn(u32 *cmd_list, u32 cmdlen)
{
	if (!cmd_list || cmdlen != SCU_PARAM_LEN)
		return -EINVAL;

	cmd_list[0] |= (cmd_list[1] << IPC_CMD_SUBCMD);

	return 0;
}

static int pre_cmd_fn(u32 *cmd_list, u32 cmdlen, u32 *in, u32 inlen,
		u32 *out, u32 outlen)
{
	int ret;

	if (inlen > IPC_WWBUF_SIZE_DWORD || outlen > IPC_RWBUF_SIZE_DWORD)
		return -EINVAL;

	ret = pre_simple_cmd_fn(cmd_list, cmdlen);
	if (ret < 0)
		return ret;

	cmd_list[0] |= (inlen << IPC_CMD_SIZE);

	return 0;
}

static int pre_raw_cmd_fn(u32 *cmd_list, u32 cmdlen, u8 *in, u32 inlen,
		u32 *out, u32 outlen, u32 dptr, u32 sptr)
{
	int ret;

	if (inlen > IPC_WWBUF_SIZE || outlen > IPC_RWBUF_SIZE_DWORD)
		return -EINVAL;

	ret = pre_simple_cmd_fn(cmd_list, cmdlen);
	if (ret < 0)
		return ret;

	cmd_list[0] |= (inlen << IPC_CMD_SIZE);

	return 0;
}

static int scu_ipc_err_code(int status)
{
	if (status & IPC_DEV_SCU_CMD_STATUS_ERR)
		return (status & IPC_DEV_SCU_CMD_STATUS_ERR_MASK);
	else
		return 0;
}

static int scu_ipc_busy_check(int status)
{
	return status | IPC_DEV_SCU_CMD_STATUS_BUSY;
}

static u32 scu_ipc_enable_msi(u32 cmd)
{
	return cmd | IPC_DEV_SCU_CMD_MSI;
}

static struct intel_ipc_dev *intel_scu_ipc_dev_create(
		struct device *dev,
		void __iomem *base,
		int irq)
{
	struct intel_ipc_dev_ops *ops;
	struct intel_ipc_dev_cfg *cfg;
	struct regmap *ipc_regs;
	struct intel_scu_ipc_dev *scu = dev_get_drvdata(dev);

	cfg = devm_kzalloc(dev, sizeof(*cfg), GFP_KERNEL);
	if (!cfg)
		return ERR_PTR(-ENOMEM);

	ops = devm_kzalloc(dev, sizeof(*ops), GFP_KERNEL);
	if (!ops)
		return ERR_PTR(-ENOMEM);

        ipc_regs = devm_regmap_init_mmio_clk(dev, NULL, base,
			&ipc_regmap_config);
        if (IS_ERR(ipc_regs)) {
                dev_err(dev, "ipc_regs regmap init failed\n");
                return ERR_CAST(ipc_regs);;
        }

	scu->ipc_regs = ipc_regs;

	/* set IPC dev ops */
	ops->to_err_code = scu_ipc_err_code;
	ops->busy_check = scu_ipc_busy_check;
	ops->enable_msi = scu_ipc_enable_msi;
	ops->pre_cmd_fn = pre_cmd_fn;
	ops->pre_raw_cmd_fn = pre_raw_cmd_fn;
	ops->pre_simple_cmd_fn = pre_simple_cmd_fn;

	/* set cfg options */
	if (scu->irq_mode)
		cfg->mode = IPC_DEV_MODE_IRQ;
	else
		cfg->mode = IPC_DEV_MODE_POLLING;

	cfg->chan_type = IPC_CHANNEL_IA_SCU;
	cfg->irq = irq;
	cfg->use_msi = true;
	cfg->support_sptr = true;
	cfg->support_dptr = true;
	cfg->cmd_regs = ipc_regs;
	cfg->data_regs = ipc_regs;
	cfg->wrbuf_reg = IPC_DEV_SCU_WRBUF_OFFSET;
	cfg->rbuf_reg = IPC_DEV_SCU_RBUF_OFFSET;
	cfg->sptr_reg = IPC_DEV_SCU_SPTR_OFFSET;
	cfg->dptr_reg = IPC_DEV_SCU_DPTR_OFFSET;
	cfg->status_reg = IPC_DEV_SCU_STATUS_OFFSET;

	return devm_intel_ipc_dev_create(dev, INTEL_SCU_IPC_DEV, cfg, ops);
}

/**
 *	ipc_probe	-	probe an Intel SCU IPC
 *	@pdev: the PCI device matching
 *	@id: entry in the match table
 *
 *	Enable and install an intel SCU IPC. This appears in the PCI space
 *	but uses some hard coded addresses as well.
 */
static int ipc_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	int err;
	struct intel_scu_ipc_dev *scu = &ipcdev;
	struct intel_scu_ipc_pdata_t *pdata;

	if (scu->dev)		/* We support only one SCU */
		return -EBUSY;

	pdata = (struct intel_scu_ipc_pdata_t *)id->driver_data;
	if (!pdata)
		return -ENODEV;

	scu->irq_mode = pdata->irq_mode;

	err = pcim_enable_device(pdev);
	if (err)
		return err;

	err = pcim_iomap_regions(pdev, 1 << 0, pci_name(pdev));
	if (err)
		return err;

	scu->ipc_base = pcim_iomap_table(pdev)[0];

	scu->i2c_base = devm_ioremap_nocache(&pdev->dev, pdata->i2c_base,
			pdata->i2c_len);
	if (!scu->i2c_base)
		return -ENOMEM;

	pci_set_drvdata(pdev, scu);

        scu->i2c_regs = devm_regmap_init_mmio_clk(&pdev->dev, NULL,
			scu->i2c_base, &i2c_regmap_config);
        if (IS_ERR(scu->i2c_regs)) {
                dev_err(&pdev->dev, "i2c_regs regmap init failed\n");
                return PTR_ERR(scu->i2c_regs);;
        }

	scu->ipc_dev = intel_scu_ipc_dev_create(&pdev->dev, scu->ipc_base,
			pdev->irq);
	if (IS_ERR(scu->ipc_dev)) {
		dev_err(&pdev->dev, "Failed to create SCU IPC device\n");
		return PTR_ERR(scu->ipc_dev);
	}

	/* Assign device at last */
	scu->dev = &pdev->dev;

	intel_scu_devices_create();

	return 0;
}

#define SCU_DEVICE(id, pdata)	{PCI_VDEVICE(INTEL, id), (kernel_ulong_t)&pdata}

static const struct pci_device_id pci_ids[] = {
	SCU_DEVICE(PCI_DEVICE_ID_LINCROFT,	intel_scu_ipc_lincroft_pdata),
	SCU_DEVICE(PCI_DEVICE_ID_PENWELL,	intel_scu_ipc_penwell_pdata),
	SCU_DEVICE(PCI_DEVICE_ID_CLOVERVIEW,	intel_scu_ipc_penwell_pdata),
	SCU_DEVICE(PCI_DEVICE_ID_TANGIER,	intel_scu_ipc_tangier_pdata),
	{}
};

static struct pci_driver ipc_driver = {
	.driver = {
		.suppress_bind_attrs = true,
	},
	.name = "intel_scu_ipc",
	.id_table = pci_ids,
	.probe = ipc_probe,
};
builtin_pci_driver(ipc_driver);
