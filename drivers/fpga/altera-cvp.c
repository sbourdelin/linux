/*
 * FPGA Manager Driver for Altera Arria/Cyclone/Stratix CvP
 *
 * Copyright (C) 2017 DENX Software Engineering
 *
 * Anatolij Gustschin <agust@denx.de>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * Manage Altera FPGA firmware using PCIe CvP.
 * Firmware must be in binary "rbf" format.
 */

#include <linux/delay.h>
#include <linux/fpga/fpga-mgr.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/sizes.h>

#define CVP_BAR		0	/* BAR used for data transfer in memory mode */
#define CVP_DUMMY_WR	244	/* dummy writes to clear CvP state machine */
#define TIMEOUT_IN_US	2000

/* Vendor Specific Extended Capability Offset */
#define VSEC_OFFSET			0x200
#define VSEC_PCIE_EXT_CAP_ID_VAL	0x000b

/* offsets from VSEC_OFFSET */
#define VSEC_PCIE_EXT_CAP_ID		(VSEC_OFFSET + 0x00)	/* 16bit */
#define VSEC_VERSION			0x02		/* 8bit */
#define VSEC_NEXT_CAP_OFF		0x03		/* 8bit */
#define VSEC_ID				0x04		/* 16bit */
#define VSEC_REV			0x06		/* 8bit */
#define VSEC_LENGTH			0x07		/* 8bit */
#define VSEC_ALTERA_MARKER		0x08		/* 32bit */

#define VSEC_CVP_STATUS			(VSEC_OFFSET + 0x1c)	/* 32bit */
#define VSEC_CVP_STATUS_DATA_ENC	BIT(16)	/* is treated as encrypted */
#define VSEC_CVP_STATUS_DATA_COMP	BIT(17)	/* is treated as compressed */
#define VSEC_CVP_STATUS_CFG_RDY		BIT(18)	/* CVP_CONFIG_READY */
#define VSEC_CVP_STATUS_CFG_ERR		BIT(19)	/* CVP_CONFIG_ERROR */
#define VSEC_CVP_STATUS_CVP_EN		BIT(20)	/* ctrl block is enabling CVP */
#define VSEC_CVP_STATUS_USERMODE	BIT(21)	/* USERMODE */
#define VSEC_CVP_STATUS_CFG_DONE	BIT(23)	/* CVP_CONFIG_DONE */
#define VSEC_CVP_STATUS_PLD_CLK_IN_USE	BIT(24)	/* PLD_CLK_IN_USE */

#define VSEC_CVP_MODE_CTRL		(VSEC_OFFSET + 0x20)	/* 32bit */
#define VSEC_CVP_MODE_CTRL_CVP_MODE	BIT(0) /* CVP (1) or normal mode (0) */
#define VSEC_CVP_MODE_CTRL_HIP_CLK_SEL	BIT(1) /* PMA (1) or fabric clock (0) */
#define VSEC_CVP_MODE_CTRL_FULL_CFG	BIT(2) /* CVP_FULLCONFIG */
#define VSEC_CVP_MODE_CTRL_NUMCLKS	(0xff<<8) /* CVP_NUMCLKS */

#define VSEC_CVP_DATA			(VSEC_OFFSET + 0x28)	/* 32bit */
#define VSEC_CVP_PROG_CTRL		(VSEC_OFFSET + 0x2c)	/* 32bit */
#define VSEC_CVP_PROG_CTRL_CONFIG	BIT(0)
#define VSEC_CVP_PROG_CTRL_START_XFER	BIT(1)

#define VSEC_UNCOR_ERR_STATUS		(VSEC_OFFSET + 0x34)	/* 32bit */
#define VSEC_UNCOR_ERR_MASK		(VSEC_OFFSET + 0x38)	/* 32bit */
#define	VSEC_UNCOR_ERR_CVP_CFG_ERR	BIT(5)	/* CVP_CONFIG_ERROR_LATCHED */

#define	DRV_NAME		"altera-cvp"
#define ALTERA_CVP_MGR_NAME	"Altera CvP FPGA Manager"

static int chkcfg; /* use value 1 for debugging only */
module_param(chkcfg, int, 0664);
MODULE_PARM_DESC(chkcfg, "1 - check CvP status, 0 - skip checking (default 0)");

static int numclks = 1; /* default 1 for uncompressed and unencrypted */
module_param(numclks, int, 0664);
MODULE_PARM_DESC(numclks, "Clock to data ratio 1, 4 or 8 (default 1)");

struct altera_cvp_conf {
	struct fpga_manager	*mgr;
	struct pci_dev		*pci_dev;
	void __iomem		*map;
	void			(*write_data)(struct altera_cvp_conf *conf,
					      u32 val);
	char			mgr_name[64];
};

static inline void altera_cvp_chk_numclks(void)
{
	switch (numclks) {
	case 1:
	case 4:
	case 8:
		break;
	default:
		numclks = 1;
		break;
	}
}

static enum fpga_mgr_states altera_cvp_state(struct fpga_manager *mgr)
{
	struct altera_cvp_conf *conf = mgr->priv;
	u32 status;

	pci_read_config_dword(conf->pci_dev, VSEC_CVP_STATUS, &status);

	if (status & VSEC_CVP_STATUS_CFG_DONE)
		return FPGA_MGR_STATE_OPERATING;

	if (status & VSEC_CVP_STATUS_CVP_EN)
		return FPGA_MGR_STATE_POWER_UP;

	return FPGA_MGR_STATE_UNKNOWN;
}

static void altera_cvp_write_data_iomem(struct altera_cvp_conf *conf, u32 val)
{
	writel(val, conf->map);
}

static void altera_cvp_write_data_config(struct altera_cvp_conf *conf, u32 val)
{
	pci_write_config_dword(conf->pci_dev, VSEC_CVP_DATA, val);
}

static void altera_cvp_dummy_write(struct altera_cvp_conf *conf)
{
	u32 val32;
	int i;

	/* set number of CVP clock cycles for every CVP Data Register Write */
	pci_read_config_dword(conf->pci_dev, VSEC_CVP_MODE_CTRL, &val32);
	val32 &= ~VSEC_CVP_MODE_CTRL_NUMCLKS;
	val32 |= 0x01 << 8;	/* 1 clock */
	pci_write_config_dword(conf->pci_dev, VSEC_CVP_MODE_CTRL, val32);

	for (i = 0; i < CVP_DUMMY_WR; i++)
		conf->write_data(conf, 0xdeadbeef);
}

static int altera_cvp_teardown(struct fpga_manager *mgr,
			       struct fpga_image_info *info)
{
	struct altera_cvp_conf *conf = mgr->priv;
	struct pci_dev *pdev = conf->pci_dev;
	int status = 0;
	int delay_us;
	u32 val32;

	/*
	 * STEP 12 - reset START_XFER bit
	 */
	pci_read_config_dword(pdev, VSEC_CVP_PROG_CTRL, &val32);
	val32 &= ~VSEC_CVP_PROG_CTRL_START_XFER;
	pci_write_config_dword(pdev, VSEC_CVP_PROG_CTRL, val32);

	/*
	 * STEP 13 - reset CVP_CONFIG bit
	 */
	val32 &= ~VSEC_CVP_PROG_CTRL_CONFIG;
	pci_write_config_dword(pdev, VSEC_CVP_PROG_CTRL, val32);

	/*
	 * STEP 14
	 * - set CVP_NUMCLKS to 0x01 and then issue CVP_DUMMY_WR dummy
	 *   writes to the HIP
	 */
	altera_cvp_dummy_write(conf); /* from CVP clock to internal clock */

	/*
	 * STEP 15 - poll CVP_CONFIG_READY bit
	 */
	delay_us = 0;
	while (1) {
		pci_read_config_dword(pdev, VSEC_CVP_STATUS, &val32);
		if ((val32 & VSEC_CVP_STATUS_CFG_RDY) == 0)
			break;

		udelay(1);	/* wait 1us */

		if (delay_us++ > TIMEOUT_IN_US) {
			dev_warn(&mgr->dev, "CVP_CONFIG_READY == 0 timeout\n");
			status = -ETIMEDOUT;
			break;
		}
	}

	return status;
}

static int altera_cvp_write_init(struct fpga_manager *mgr,
				 struct fpga_image_info *info,
				 const char *buf, size_t count)
{
	struct altera_cvp_conf *conf = mgr->priv;
	struct pci_dev *pdev = conf->pci_dev;
	int delay_us;
	u32 val32;
	int ret;

	if (info && info->flags & FPGA_MGR_PARTIAL_RECONFIG) {
		dev_err(&mgr->dev, "Partial reconfiguration not supported.\n");
		return -EINVAL;
	}

	/*
	 * STEP 1 - read CVP status and check CVP_EN flag
	 */
	pci_read_config_dword(pdev, VSEC_CVP_STATUS, &val32);
	if (!(val32 & VSEC_CVP_STATUS_CVP_EN)) {
		dev_err(&mgr->dev, "CVP mode off: 0x%04x\n", val32);
		return -ENODEV;
	}

	if (val32 & VSEC_CVP_STATUS_CFG_RDY) {
		dev_warn(&mgr->dev, "CvP already started, teardown first\n");
		ret = altera_cvp_teardown(mgr, info);
		if (ret < 0)
			return ret;
	}

	/*
	 * STEP 2
	 * - set HIP_CLK_SEL and CVP_MODE (must be set in the order mentioned)
	 */
	/* switch from fabric to PMA clock */
	pci_read_config_dword(pdev, VSEC_CVP_MODE_CTRL, &val32);
	val32 |= VSEC_CVP_MODE_CTRL_HIP_CLK_SEL;
	pci_write_config_dword(pdev, VSEC_CVP_MODE_CTRL, val32);

	/* set CVP mode */
	pci_read_config_dword(pdev, VSEC_CVP_MODE_CTRL, &val32);
	val32 |= VSEC_CVP_MODE_CTRL_CVP_MODE;
	pci_write_config_dword(pdev, VSEC_CVP_MODE_CTRL, val32);

	/*
	 * STEP 3
	 * - set CVP_NUMCLKS to 1 and issue CVP_DUMMY_WR dummy writes to the HIP
	 */
	altera_cvp_dummy_write(conf);

	/*
	 * STEP 4 - set CVP_CONFIG bit
	 */
	pci_read_config_dword(pdev, VSEC_CVP_PROG_CTRL, &val32);
	/* request control block to begin transfer using CVP */
	val32 |= VSEC_CVP_PROG_CTRL_CONFIG;
	pci_write_config_dword(pdev, VSEC_CVP_PROG_CTRL, val32);

	/*
	 * STEP 5 - poll CVP_CONFIG READY
	 */
	delay_us = 0;
	while (1) {
		pci_read_config_dword(pdev, VSEC_CVP_STATUS, &val32);
		if ((val32 & VSEC_CVP_STATUS_CFG_RDY) ==
		    VSEC_CVP_STATUS_CFG_RDY)
			break;

		udelay(1); /* wait 1us */

		if (delay_us++ > TIMEOUT_IN_US) {
			dev_warn(&mgr->dev, "CVP_CONFIG_READY == 1 timeout\n");
			return -ETIMEDOUT;
		}
	}

	/*
	 * STEP 6
	 * - set CVP_NUMCLKS to 1 and issue CVP_DUMMY_WR dummy writes to the HIP
	 */
	altera_cvp_dummy_write(conf);	/* from internal clock to CVP clock */

	/*
	 * STEP 7 - set START_XFER
	 */
	pci_read_config_dword(pdev, VSEC_CVP_PROG_CTRL, &val32);
	val32 |= VSEC_CVP_PROG_CTRL_START_XFER;
	pci_write_config_dword(pdev, VSEC_CVP_PROG_CTRL, val32);

	/*
	 * STEP 8 - start transfer (set CVP_NUMCLKS)
	 */
	altera_cvp_chk_numclks();
	pci_read_config_dword(pdev, VSEC_CVP_MODE_CTRL, &val32);
	val32 &= ~VSEC_CVP_MODE_CTRL_NUMCLKS;
	val32 |= numclks << 8; /* bitstream specific clock count */
	pci_write_config_dword(pdev, VSEC_CVP_MODE_CTRL, val32);

	return 0;
}

static inline int altera_cvp_chk_error(struct fpga_manager *mgr, size_t bytes)
{
	struct altera_cvp_conf *conf = mgr->priv;
	u32 val32;

	/*
	 * STEP 10 (optional) - check CVP_CONFIG_ERROR flag
	 */
	pci_read_config_dword(conf->pci_dev, VSEC_CVP_STATUS, &val32);
	if (val32 & VSEC_CVP_STATUS_CFG_ERR) {
		dev_err(&mgr->dev, "CVP_CONFIG_ERROR after %zi bytes!\n",
			bytes);
		return -EPROTO;
	}
	return 0;
}

static int altera_cvp_write(struct fpga_manager *mgr, const char *buf,
			    size_t count)
{
	struct altera_cvp_conf *conf = mgr->priv;
	const u32 *data;
	size_t bytes;
	int status = 0;
	u32 mask;

	/*
	 * STEP 9
	 * - write 32-bit configuration data from RBF file to CVP data register
	 */
	data = (u32 *)buf;
	bytes = count;

	while (bytes >= 4) {
		conf->write_data(conf, *data++);
		bytes -= 4;

		/*
		 * STEP 10 (optional) and STEP 11
		 * - check error flag
		 * - loop until data transfer completed
		 */
		if (chkcfg && !(bytes % SZ_4K)) {
			size_t done = count - bytes;

			status = altera_cvp_chk_error(mgr, done);
			if (status < 0)
				return status;
		}
	}

	switch (bytes) {
	case 3:
		mask = 0x00ffffff;
		break;
	case 2:
		mask = 0x0000ffff;
		break;
	case 1:
		mask = 0x000000ff;
		break;
	case 0:
		mask = 0x00000000;
		break;
	}

	if (mask) {
		conf->write_data(conf, *data & mask);

		if (chkcfg)
			status = altera_cvp_chk_error(mgr, count);
	}

	return status;
}

static int altera_cvp_write_complete(struct fpga_manager *mgr,
				     struct fpga_image_info *info)
{
	struct altera_cvp_conf *conf = mgr->priv;
	struct pci_dev *pdev = conf->pci_dev;
	int status;
	int delay_us;
	u32 status_msk;
	u32 val32;

	status = altera_cvp_teardown(mgr, info);
	if (status < 0)
		return status;

	/*
	 * STEP 16 - check CVP_CONFIG_ERROR_LATCHED bit
	 */
	pci_read_config_dword(pdev, VSEC_UNCOR_ERR_STATUS, &val32);
	if (val32 & VSEC_UNCOR_ERR_CVP_CFG_ERR) {
		dev_err(&mgr->dev, "detected CVP_CONFIG_ERROR_LATCHED!\n");
		return -EFAULT;
	}

	/*
	 * STEP 17 - reset CVP_MODE and HIP_CLK_SEL bit
	 */
	pci_read_config_dword(pdev, VSEC_CVP_MODE_CTRL, &val32);
	val32 &= ~VSEC_CVP_MODE_CTRL_HIP_CLK_SEL;
	val32 &= ~VSEC_CVP_MODE_CTRL_CVP_MODE;
	pci_write_config_dword(pdev, VSEC_CVP_MODE_CTRL, val32);

	/*
	 * STEP 18 - poll PLD_CLK_IN_USE and USER_MODE bits
	 */
	delay_us = 0;
	status_msk = VSEC_CVP_STATUS_PLD_CLK_IN_USE | VSEC_CVP_STATUS_USERMODE;
	while (1) {
		pci_read_config_dword(pdev, VSEC_CVP_STATUS, &val32);
		if ((val32 & status_msk) == status_msk)
			break;

		udelay(1); /* wait 1us */

		if (delay_us++ > TIMEOUT_IN_US) {
			dev_warn(&mgr->dev, "PLD_CLK_IN_USE|USERMODE timeout\n");
			status = -ETIMEDOUT;
			break;
		}
	}

	return status;
}

static const struct fpga_manager_ops altera_cvp_ops = {
	.state		= altera_cvp_state,
	.write_init	= altera_cvp_write_init,
	.write		= altera_cvp_write,
	.write_complete	= altera_cvp_write_complete,
};

static int altera_cvp_probe(struct pci_dev *pdev,
			    const struct pci_device_id *dev_id)
{
	struct altera_cvp_conf *conf;
	u16 cmd, val16;
	int ret;

	pci_read_config_word(pdev, VSEC_PCIE_EXT_CAP_ID, &val16);
	if (val16 != VSEC_PCIE_EXT_CAP_ID_VAL) {
		dev_err(&pdev->dev, "Wrong EXT_CAP_ID value 0x%x\n", val16);
		ret = -ENODEV;
		goto err;
	}

	/* Enable memory BAR access */
	pci_read_config_word(pdev, PCI_COMMAND, &cmd);
	if (!(cmd & PCI_COMMAND_MEMORY)) {
		cmd |= PCI_COMMAND_MEMORY;
		pci_write_config_word(pdev, PCI_COMMAND, cmd);
	}

	conf = devm_kzalloc(&pdev->dev, sizeof(*conf), GFP_KERNEL);
	if (!conf) {
		ret = -ENOMEM;
		goto err;
	}

	conf->pci_dev = pdev;

	if (pci_request_region(pdev, CVP_BAR, "CVP") < 0) {
		dev_err(&pdev->dev, "Requesting CVP BAR region failed\n");
		ret = -ENODEV;
		goto err;
	}

	conf->write_data = altera_cvp_write_data_iomem;

	conf->map = pci_iomap(pdev, CVP_BAR, 0);
	if (!conf->map) {
		dev_warn(&pdev->dev, "Mapping CVP BAR failed\n");
		conf->write_data = altera_cvp_write_data_config;
	}

	snprintf(conf->mgr_name, sizeof(conf->mgr_name), "%s @%02x:%02x.%d",
		 ALTERA_CVP_MGR_NAME, pdev->bus->number,
		 PCI_SLOT(pdev->devfn), PCI_FUNC(pdev->devfn));

	ret = fpga_mgr_register(&pdev->dev, conf->mgr_name,
				&altera_cvp_ops, conf);
	if (ret)
		goto err_unmap;

	conf->mgr = fpga_mgr_get(&pdev->dev);
	if (IS_ERR(conf->mgr)) {
		dev_err(&pdev->dev, "Getting fpga mgr reference failed\n");
		ret = -ENODEV;
		goto err_unreg;
	}
	fpga_mgr_put(conf->mgr);

	return 0;

err_unreg:
	fpga_mgr_unregister(&pdev->dev);
err_unmap:
	pci_iounmap(pdev, conf->map);
	pci_release_region(pdev, CVP_BAR);
err:
	cmd &= ~PCI_COMMAND_MEMORY;
	pci_write_config_word(pdev, PCI_COMMAND, cmd);
	return ret;
}

static void altera_cvp_remove(struct pci_dev *pdev)
{
	struct fpga_manager *mgr = pci_get_drvdata(pdev);
	struct altera_cvp_conf *conf = mgr->priv;
	u16 cmd;

	fpga_mgr_unregister(&pdev->dev);
	pci_iounmap(pdev, conf->map);
	pci_release_region(pdev, CVP_BAR);
	pci_read_config_word(pdev, PCI_COMMAND, &cmd);
	cmd &= ~PCI_COMMAND_MEMORY;
	pci_write_config_word(pdev, PCI_COMMAND, cmd);
}

#define PCI_VENDOR_ID_ALTERA	0x1172

static struct pci_device_id altera_cvp_id_tbl[] = {
	{ PCI_VDEVICE(ALTERA, PCI_ANY_ID) },
	{ }
};

MODULE_DEVICE_TABLE(pci, altera_cvp_id_tbl);

static struct pci_driver altera_cvp_driver = {
	.name   = DRV_NAME,
	.id_table = altera_cvp_id_tbl,
	.probe  = altera_cvp_probe,
	.remove = altera_cvp_remove,
};

static int __init altera_cvp_init(void)
{
	return pci_register_driver(&altera_cvp_driver);
}

static void __exit altera_cvp_exit(void)
{
	pci_unregister_driver(&altera_cvp_driver);
}

module_init(altera_cvp_init);
module_exit(altera_cvp_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Anatolij Gustschin <agust@denx.de>");
MODULE_DESCRIPTION("Module to load Altera FPGA over CvP");
