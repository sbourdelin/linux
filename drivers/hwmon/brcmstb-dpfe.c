/*
 * Temperature sensor driver for Broadcom set top box SoCs
 *
 * Copyright (c) 2017 Broadcom
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation version 2.
 *
 * This program is distributed "as is" WITHOUT ANY WARRANTY of any
 * kind, whether express or implied; without even the implied warranty
 * of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>

#define DRVNAME			"brcmstb-dpfe"
#define FIRMWARE_NAME		"dpfe.bin"
#define DT_COMPAT_DMEM		"brcm,dpfe-dmem"
#define DT_COMPAT_IMEM		"brcm,dpfe-imem"

/* DCPU register offsets */
#define REG_DCPU_RESET		0x0
#define REG_TO_DCPU_MBOX	0x10
#define REG_TO_HOST_MBOX	0x14

/* Message RAM */
#define DCPU_MSG_RAM(x)		(0x100 + (x) * sizeof(u32))

/* DRAM Info Offsets & Masks */
#define DRAM_INFO_INTERVAL	0x0
#define DRAM_INFO_MR4		0x4
#define DRAM_INFO_ERROR		0x8
#define DRAM_INFO_MASK		0xff

/* DRAM MR4 Offsets & Masks */
#define DRAM_MR4_REFRESH	0x0	/* Refresh rate */
#define DRAM_MR4_SR_ABORT	0x3	/* Self Refresh Abort */
#define DRAM_MR4_PPRE		0x4	/* Post-package repair entry/exit */
#define DRAM_MR4_TH_OFFS	0x5	/* Thermal Offset; vendor specific */
#define DRAM_MR4_TUF		0x7	/* Temperature Update Flag */

#define DRAM_MR4_REFRESH_MASK	0x7
#define DRAM_MR4_SR_ABORT_MASK	0x1
#define DRAM_MR4_PPRE_MASK	0x1
#define DRAM_MR4_TH_OFFS_MASK	0x3
#define DRAM_MR4_TUF_MASK	0x1

/* DRAM Vendor Offsets & Masks */
#define DRAM_VENDOR_MR5		0x0
#define DRAM_VENDOR_MR6		0x4
#define DRAM_VENDOR_MR7		0x8
#define DRAM_VENDOR_MR8		0xc
#define DRAM_VENDOR_ERROR	0x10
#define DRAM_VENDOR_MASK	0xff

/* Reset register bits & masks */
#define DCPU_RESET_SHIFT	0x0
#define DCPU_RESET_MASK		0x1
#define DCPU_CLK_DISABLE_SHIFT	0x2

/* DCPU return codes */
#define DCPU_RET_SUCCESS	0x00000001
#define DCPU_RET_ERR_HEADER	0x80000001
#define DCPU_RET_ERR_INVAL	0x80000002
#define DCPU_RET_ERR_CHKSUM	0x80000004
#define DCPU_RET_ERR_OTHER	0x80000008

/* Firmware magic */
#define DPFE_BE_MAGIC		0xfe1010fe
#define DPFE_LE_MAGIC		0xfe0101fe

/* Error codes */
#define ERR_INVALID_MAGIC	-1
#define ERR_INVALID_SIZE	-2
#define ERR_INVALID_CHKSUM	-3

/* Message types */
#define DPFE_MSG_TYPE_COMMAND	1
#define DPFE_MSG_TYPE_RESPONSE	2

#define DELAY_LOOP_MAX		200000

enum dpfe_msg_fields {
	MSG_HEADER,
	MSG_COMMAND,
	MSG_ARG_COUNT,
	MSG_ARG0,
	MSG_CHKSUM,
	MSG_FIELD_MAX /* Last entry */
};

enum dpfe_commands {
	DPFE_CMD_GET_INFO,
	DPFE_CMD_GET_REFRESH,
	DPFE_CMD_GET_VENDOR,
	DPFE_CMD_MAX /* Last entry */
};

struct dpfe_msg {
	u32 header;
	u32 command;
	u32 arg_count;
	u32 arg0;
	u32 chksum; /* This is the sum of all other entries. */
};

/*
 * Format of the binary firmware file:
 *
 *   entry
 *      0    header
 *              value:  0xfe0101fe  <== little endian
 *                      0xfe1010fe  <== big endian
 *      1    sequence:
 *              [31:16] total segments on this build
 *              [15:0]  this segment sequence.
 *      2    FW version
 *      3    IMEM byte size
 *      4    DMEM byte size
 *           IMEM
 *           DMEM
 *      last checksum ==> sum of everything
 */
struct dpfe_firmware_header {
	u32 magic;
	u32 sequence;
	u32 version;
	u32 imem_size;
	u32 dmem_size;
};

/* Things we only need during initialization. */
struct init_data {
	void __iomem *dmem;
	void __iomem *imem;
	unsigned int dmem_len;
	unsigned int imem_len;
	unsigned int chksum;
	bool is_big_endian;
};

/* Things we need for as long as we are active. */
struct private_data {
	void __iomem *regs;
	void __iomem *dmem;
	struct mutex lock;
};

/* List of supported firmware commands */
const u32 dpfe_commands[DPFE_CMD_MAX][MSG_FIELD_MAX] = {
	[DPFE_CMD_GET_INFO] = {
		[MSG_HEADER] = DPFE_MSG_TYPE_COMMAND,
		[MSG_COMMAND] = 1,
		[MSG_ARG_COUNT] = 1,
		[MSG_ARG0] = 1,
		[MSG_CHKSUM] = 4,
	},
	[DPFE_CMD_GET_REFRESH] = {
		[MSG_HEADER] = DPFE_MSG_TYPE_COMMAND,
		[MSG_COMMAND] = 2,
		[MSG_ARG_COUNT] = 1,
		[MSG_ARG0] = 1,
		[MSG_CHKSUM] = 5,
	},
	[DPFE_CMD_GET_VENDOR] = {
		[MSG_HEADER] = DPFE_MSG_TYPE_COMMAND,
		[MSG_COMMAND] = 2,
		[MSG_ARG_COUNT] = 1,
		[MSG_ARG0] = 2,
		[MSG_CHKSUM] = 6,
	},
};

static u32 dpfe_readl(const void __iomem *addr)
{
	return le32_to_cpu(readl_relaxed(addr));
}

static void dpfe_writel(u32 value, void __iomem *addr)
{
	writel_relaxed(cpu_to_le32(value), addr);
}

static void __iomem *__map_region(const char *name)
{
	struct device_node *np;
	void __iomem *ptr;

	np = of_find_compatible_node(NULL, NULL, name);
	if (!np)
		return NULL;

	ptr = of_iomap(np, 0);
	of_node_put(np);

	return ptr;
}

static void __disable_dcpu(void __iomem *regs)
{
	u32 val;

	/* Check if DCPU is running */
	val = dpfe_readl(regs + REG_DCPU_RESET);
	if (!(val & DCPU_RESET_MASK)) {
		/* Put DCPU in reset */
		val |= (1 << DCPU_RESET_SHIFT);
		dpfe_writel(val, regs + REG_DCPU_RESET);
	}
}

static void __enable_dcpu(void __iomem *regs)
{
	u32 val;

	/* Clear mailbox registers. */
	dpfe_writel(0, regs + REG_TO_DCPU_MBOX);
	dpfe_writel(0, regs + REG_TO_HOST_MBOX);

	/* Disable DCPU clock gating */
	val = dpfe_readl(regs + REG_DCPU_RESET);
	val &= ~(1 << DCPU_CLK_DISABLE_SHIFT);
	dpfe_writel(val, regs + REG_DCPU_RESET);

	/* Take DCPU out of reset */
	val = dpfe_readl(regs + REG_DCPU_RESET);
	val &= ~(1 << DCPU_RESET_SHIFT);
	dpfe_writel(val, regs + REG_DCPU_RESET);
}

static unsigned int get_msg_chksum(const u32 msg[])
{
	unsigned int sum = 0;
	unsigned int i;

	/* Don't include the last field in the checksum. */
	for (i = 0; i < MSG_FIELD_MAX - 1; i++)
		sum += msg[i];

	return sum;
}

static int __send_command(struct private_data *priv, unsigned int cmd,
			  u32 result[])
{
	const u32 *msg = dpfe_commands[cmd];
	void __iomem *regs = priv->regs;
	unsigned int i, chksum;
	int ret = 0;
	u32 resp;

	if (cmd >= DPFE_CMD_MAX)
		return -1;

	mutex_lock(&priv->lock);

	/* Write command and arguments to message area */
	for (i = 0; i < MSG_FIELD_MAX; i++)
		dpfe_writel(msg[i], regs + DCPU_MSG_RAM(i));

	/* Tell DCPU there is a command waiting */
	dpfe_writel(1, regs + REG_TO_DCPU_MBOX);

	/* Wait for DCPU to process the command */
	for (i = 0; i < DELAY_LOOP_MAX; i++) {
		/* Read response code */
		resp = dpfe_readl(regs + REG_TO_HOST_MBOX);
		if (resp > 0)
			break;
		udelay(5);
	}
	if (i == DELAY_LOOP_MAX)
		ret = -ETIMEDOUT;

	/* Read response data */
	for (i = 0; i < MSG_FIELD_MAX; i++)
		result[i] = dpfe_readl(regs + DCPU_MSG_RAM(i));

	/* Tell DCPU we are done */
	dpfe_writel(0, regs + REG_TO_HOST_MBOX);

	mutex_unlock(&priv->lock);

	if (!ret) {
		/* Verify response */
		chksum = get_msg_chksum(result);
		if (chksum != result[MSG_CHKSUM])
			ret = -1;
	}

	if (!ret) {
		switch (resp) {
		case DCPU_RET_SUCCESS:
			break;
		case DCPU_RET_ERR_HEADER:
		case DCPU_RET_ERR_INVAL:
		case DCPU_RET_ERR_CHKSUM:
		case DCPU_RET_ERR_OTHER:
			ret = -1;
			break;
		}
	}

	return ret;
}

/* Ensure that the firmware file loaded meets all the requirements. */
static int __verify_firmware(struct init_data *init,
			     const struct firmware *fw)
{
	const struct dpfe_firmware_header *header = (void *)fw->data;
	unsigned int dmem_size, imem_size, total_size;
	bool is_big_endian = false;
	const u32 *chksum;

	if (header->magic == DPFE_BE_MAGIC)
		is_big_endian = true;
	else if (header->magic != DPFE_LE_MAGIC)
		return ERR_INVALID_MAGIC;

	if (is_big_endian) {
		dmem_size = be32_to_cpu(header->dmem_size);
		imem_size = be32_to_cpu(header->imem_size);
	} else {
		dmem_size = header->dmem_size;
		imem_size = header->imem_size;
	}

	/* Data and instruction sections are 32 bit words. */
	if ((dmem_size % sizeof(u32)) != 0 || (imem_size % sizeof(u32)) != 0)
		return ERR_INVALID_SIZE;

	/*
	 * The header + the data section + the instruction section + the
	 * checksum must be equal to the total firmware size.
	 */
	total_size = dmem_size + imem_size + sizeof(*header) + sizeof(*chksum);
	if (total_size != fw->size)
		return ERR_INVALID_SIZE;

	/* The checksum comes at the very end. */
	chksum = (void *)fw->data + sizeof(*header) + dmem_size + imem_size;

	init->is_big_endian = is_big_endian;
	init->dmem_len = dmem_size;
	init->imem_len = imem_size;
	init->chksum = (is_big_endian) ? be32_to_cpu(*chksum) : *chksum;

	return 0;
}

/* Verify checksum by reading back the firmware from co-processor RAM. */
static int __verify_fw_checksum(struct init_data *init,
			     const struct dpfe_firmware_header *header,
			     u32 checksum)
{
	u32 magic, sequence, version, sum;
	u32 __iomem *dmem = init->dmem;
	u32 __iomem *imem = init->imem;
	unsigned int i;

	if (init->is_big_endian) {
		magic = be32_to_cpu(header->magic);
		sequence = be32_to_cpu(header->sequence);
		version = be32_to_cpu(header->version);
	} else {
		magic = header->magic;
		sequence = header->sequence;
		version = header->version;
	}

	sum = magic + sequence + version + init->dmem_len + init->imem_len;

	for (i = 0; i < init->dmem_len / sizeof(u32); i++)
		sum += dpfe_readl(dmem + i);

	for (i = 0; i < init->imem_len / sizeof(u32); i++)
		sum += dpfe_readl(imem + i);

	return (sum == checksum) ? 0 : -1;
}

static int __write_firmware(u32 __iomem *mem, const u32 *fw,
			    unsigned int size, bool is_big_endian)
{
	unsigned int i;

	/* Convert size to 32-bit words. */
	size /= sizeof(u32);

	/* It is recommended to clear the firmware area first. */
	for (i = 0; i < size; i++)
		dpfe_writel(0, mem + i);

	/* Now copy it. */
	if (is_big_endian) {
		for (i = 0; i < size; i++)
			dpfe_writel(be32_to_cpu(fw[i]), mem + i);
	} else {
		for (i = 0; i < size; i++)
			dpfe_writel(fw[i], mem + i);
	}

	return 0;
}

static int brcmstb_hwmon_download_firwmare(struct platform_device *pdev,
					   struct init_data *init)
{
	const struct dpfe_firmware_header *header;
	unsigned int dmem_size, imem_size;
	struct device *dev = &pdev->dev;
	bool is_big_endian = false;
	struct private_data *priv;
	const struct firmware *fw;
	const u32 *dmem, *imem;
	const void *fw_blob;
	int ret;

	ret = request_firmware(&fw, FIRMWARE_NAME, dev);
	/* request_firmware() prints its own error messages. */
	if (ret)
		return ret;

	priv = platform_get_drvdata(pdev);

	ret = __verify_firmware(init, fw);
	if (ret)
		return -EFAULT;

	__disable_dcpu(priv->regs);

	is_big_endian = init->is_big_endian;
	dmem_size = init->dmem_len;
	imem_size = init->imem_len;

	/* At the beginning of the firmware blob is a header. */
	header = (struct dpfe_firmware_header *)fw->data;
	/* Void pointer to the beginning of the actual firmware. */
	fw_blob = fw->data + sizeof(*header);
	/* IMEM comes right after the header. */
	imem = fw_blob;
	/* DMEM follows after IMEM. */
	dmem = fw_blob + imem_size;

	ret = __write_firmware(init->dmem, dmem, dmem_size, is_big_endian);
	if (ret)
		return ret;
	ret = __write_firmware(init->imem, imem, imem_size, is_big_endian);
	if (ret)
		return ret;

	ret = __verify_fw_checksum(init, header, init->chksum);
	if (ret)
		return ret;

	__enable_dcpu(priv->regs);

	return 0;
}

static ssize_t generic_show(unsigned int command, u32 response[],
			    struct device *dev, char *buf)
{
	struct private_data *priv;
	int ret;

	priv = dev_get_drvdata(dev);

	ret = __send_command(priv, command, response);
	if (ret)
		return sprintf(buf, "error %d\n", ret);

	return 0;
}

static ssize_t show_info(struct device *dev, struct device_attribute *devattr,
			 char *buf)
{
	u32 response[MSG_FIELD_MAX];
	unsigned int info;
	int ret;

	ret = generic_show(DPFE_CMD_GET_INFO, response, dev, buf);
	if (ret)
		return ret;

	info = response[MSG_ARG0];

	return sprintf(buf, "%u.%u.%u.%u\n",
		       (info >> 24) & 0xff,
		       (info >> 16) & 0xff,
		       (info >> 8) & 0xff,
		       info & 0xff);
}

static ssize_t show_refresh(struct device *dev,
			    struct device_attribute *devattr, char *buf)
{
	u32 response[MSG_FIELD_MAX];
	void __iomem *info;
	struct private_data *priv;
	unsigned int offset;
	u8 refresh, sr_abort, ppre, thermal_offs, tuf;
	u32 mr4;
	int ret;

	ret = generic_show(DPFE_CMD_GET_REFRESH, response, dev, buf);
	if (ret)
		return ret;

	priv = dev_get_drvdata(dev);
	offset = response[MSG_ARG0];
	info = priv->dmem + offset;

	mr4 = dpfe_readl(info + DRAM_INFO_MR4) & DRAM_INFO_MASK;

	refresh = (mr4 >> DRAM_MR4_REFRESH) & DRAM_MR4_REFRESH_MASK;
	sr_abort = (mr4 >> DRAM_MR4_SR_ABORT) & DRAM_MR4_SR_ABORT_MASK;
	ppre = (mr4 >> DRAM_MR4_PPRE) & DRAM_MR4_PPRE_MASK;
	thermal_offs = (mr4 >> DRAM_MR4_TH_OFFS) & DRAM_MR4_TH_OFFS_MASK;
	tuf = (mr4 >> DRAM_MR4_TUF) & DRAM_MR4_TUF_MASK;

	return sprintf(buf, "%#x %#x %#x %#x %#x %#x %#x\n",
		       dpfe_readl(info + DRAM_INFO_INTERVAL),
		       refresh, sr_abort, ppre, thermal_offs, tuf,
		       dpfe_readl(info + DRAM_INFO_ERROR));
}

static ssize_t store_refresh(struct device *dev, struct device_attribute *attr,
			  const char *buf, size_t count)
{
	u32 response[MSG_FIELD_MAX];
	struct private_data *priv;
	void __iomem *info;
	unsigned int offset;
	unsigned long val;
	int ret;

	if (kstrtoul(buf, 0, &val) < 0)
		return -EINVAL;

	priv = dev_get_drvdata(dev);

	ret = __send_command(priv, DPFE_CMD_GET_REFRESH, response);
	if (ret)
		return ret;

	offset = response[MSG_ARG0];
	info = priv->dmem + offset + DRAM_MR4_REFRESH;
	dpfe_writel(val, info);

	return count;
}


static ssize_t show_vendor(struct device *dev, struct device_attribute *devattr,
			 char *buf)
{
	u32 response[MSG_FIELD_MAX];
	struct private_data *priv;
	void __iomem *info;
	unsigned int offset;
	int ret;

	ret = generic_show(DPFE_CMD_GET_VENDOR, response, dev, buf);
	if (ret)
		return ret;

	offset = response[MSG_ARG0];
	priv = dev_get_drvdata(dev);
	info = priv->dmem + offset;

	return sprintf(buf, "%#x %#x %#x %#x %#x\n",
		       dpfe_readl(info + DRAM_VENDOR_MR5) & DRAM_VENDOR_MASK,
		       dpfe_readl(info + DRAM_VENDOR_MR6) & DRAM_VENDOR_MASK,
		       dpfe_readl(info + DRAM_VENDOR_MR7) & DRAM_VENDOR_MASK,
		       dpfe_readl(info + DRAM_VENDOR_MR8) & DRAM_VENDOR_MASK,
		       dpfe_readl(info + DRAM_VENDOR_ERROR));
}

static SENSOR_DEVICE_ATTR(dpfe_info, 0444, show_info, NULL, 1000);
static SENSOR_DEVICE_ATTR(dpfe_refresh, 0644, show_refresh, store_refresh,
			  1000);
static SENSOR_DEVICE_ATTR(dpfe_vendor, 0444, show_vendor, NULL, 1000);
static struct attribute *dpfe_attrs[] = {
	&sensor_dev_attr_dpfe_info.dev_attr.attr,
	&sensor_dev_attr_dpfe_refresh.dev_attr.attr,
	&sensor_dev_attr_dpfe_vendor.dev_attr.attr,
	NULL
};
ATTRIBUTE_GROUPS(dpfe);

static int brcmstb_hwmon_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device *hwmon_dev = NULL;
	struct private_data *priv;
	struct init_data init;
	struct resource *res;
	int ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	mutex_init(&priv->lock);
	platform_set_drvdata(pdev, priv);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	priv->regs = devm_ioremap_resource(dev, res);
	if (IS_ERR(priv->regs)) {
		dev_err(dev, "couldn't map DT entry brcm,dpfe-cpu\n");
		return -ENODEV;
	}

	init.dmem = __map_region(DT_COMPAT_DMEM);
	if (!init.dmem) {
		dev_err(dev, "Couldn't map %s\n", DT_COMPAT_DMEM);
		return -ENOENT;
	}
	init.imem = __map_region(DT_COMPAT_IMEM);
	if (init.imem) {
		ret = brcmstb_hwmon_download_firwmare(pdev, &init);
	} else {
		ret = -ENOENT;
		dev_err(dev, "Couldn't map %s\n", DT_COMPAT_IMEM);
	}

	/* We don't need IMEM after initialization. */
	iounmap(init.imem);

	if (!ret) {
		hwmon_dev = devm_hwmon_device_register_with_groups(dev,
			"brcmstb_dpfe", priv, dpfe_groups);
		if (IS_ERR(hwmon_dev))
			ret = PTR_ERR(hwmon_dev);
	}

	if (ret) {
		iounmap(init.dmem);
		dev_err(dev, "failed to initialize -- error %d\n", ret);
	} else {
		priv->dmem = init.dmem;
		dev_info(dev, "registered.\n");
	}

	return ret;
}

static int brcmstb_hwmon_remove(struct platform_device *pdev)
{
	struct private_data *priv;

	priv = platform_get_drvdata(pdev);
	iounmap(priv->dmem);

	return 0;
}

static const struct of_device_id brcmstb_hwmon_of_match[] = {
	{ .compatible = "brcm,dpfe-cpu", },
	{}
};
MODULE_DEVICE_TABLE(of, brcmstb_hwmon_of_match);

static struct platform_driver brcmstb_hwmon_driver = {
	.driver	= {
		.name = DRVNAME,
		.of_match_table = brcmstb_hwmon_of_match,
	},
	.probe = brcmstb_hwmon_probe,
	.remove = brcmstb_hwmon_remove,
};

module_platform_driver(brcmstb_hwmon_driver);

MODULE_AUTHOR("Markus Mayer <mmayer@broadcom.com>");
MODULE_DESCRIPTION("BRCMSTB Hardware Monitoring");
MODULE_LICENSE("GPL");
