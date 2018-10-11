// SPDX-License-Identifier: GPL-2.0

/*
 * Copyright (C) 2018, Opengear
 *
 * AMD Family 16h Hudson FCH SPI flash driver.
 *
 * When the FCH is strapped to SPI boot ROM mode 'SPIROM'
 * the FCH will do a flash auto-probe and self-configure
 * for read operations to the ROM address range(s).
 * For any command outside of read/write (chip erase, etc)
 * you need to go through the alternate program method.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/errno.h>
#include <linux/pci.h>
#include <linux/mutex.h>
#include <linux/iopoll.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/partitions.h>
#include <linux/mtd/spi-nor.h>

/* FCH Device LPC Bridge Configuration Registers */
#define PCI_DEVICE_ID_AMD_FCH_LPC_BRIDGE	0x780E

#define FCH_PCI_CONTROL				0x40
#define FCH_INTEGRATED_EC_PRESENT		0x80
#define FCH_EC_SEM				0x40
#define FCH_BIOS_SEM				0x20
#define FCH_LEGACY_DMA				0x04

#define FCH_ROM_ADDR_RANGE_2			0x6C

#define FCH_SPI_BASE_ADDR			0xA0
#define FCH_SPI_BASE_ADDR_MASK			0xFFFFFFC0
#define FCH_SPI_ROUTE_TPM_SPI			0x08
#define FCH_SPI_ROM_ENABLE			0x02

/* up through FIFO [C6:80] */
#define SPI_IO_REGION_LEN			256

/* SPI Registers, the labels come from the BKDG */
#define SPI_CNTRL0				0x00
#define SPI_CNTRL0_FIFO_PTR_CLEAR		0x00100000
#define SPI_CNTRL0_FIFO_PTR_CLEAR_MASK		0xFFEFFFFF
#define SPI_CNTRL0_SPI_ARB_ENABLE		0x00080000
#define SPI_CNTRL0_SPI_ARB_ENABLE_MASK		0xFFF7FFFF

#define ALT_SPI_CS				0x1D
#define ALT_SPI_CS_MASK				0xFC
#define ALT_SPI_CS_WR_BUF_EN			0x04

#define SPI100_ENABLE				0x20
#define SPI100_SPEED_CONFIG			0x22


/* SPI control shadow registers */
#define CMD_CODE				0x45

#define CMD_TRIGGER				0x47
#define CMD_TRIGGER_EXECUTE			0x80

#define TX_BYTE_COUNT				0x48

#define RX_BYTE_COUNT				0x4B

#define SPI_STATUS				0x4C
#define SPI_STATUS_BUSY_MASK			0x80000000

#define SPI_FIFO				0x80

static const struct pci_device_id amd_fch_lpc_pci_device_ids[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_AMD, PCI_DEVICE_ID_AMD_FCH_LPC_BRIDGE) },
	{}
};
MODULE_DEVICE_TABLE(pci, amd_fch_lpc_pci_device_ids);

static short norm_speed = -1;
module_param(norm_speed, short, 0444);
MODULE_PARM_DESC(norm_speed, "Specify SPI speed for normal read.  This sets NormSpeedNew[3:0] from BKDG. -1 means use existing (defaut).");

static short fast_speed = -1;
module_param(fast_speed, short, 0444);
MODULE_PARM_DESC(fast_speed, "Specify SPI speed for fast/dual/quad read.  This sets FastSpeedNew[3:0] from BKDG. -1 means use existing (defaut).");

static short alt_speed = -1;
module_param(alt_speed, short, 0444);
MODULE_PARM_DESC(alt_speed, "Specify alternate command SPI speed.  This sets AltSpeedNew[3:0] from BKDG. -1 means use existing (defaut).");

static int read_mode = -1;
module_param(read_mode, int, 0444);
MODULE_PARM_DESC(read_mode, "Specify SPI read settings.  This sets SpiReadMode[2:0] from BKDG. -1 means use existing (defaut).");

static char *flash_name;
module_param(flash_name, charp, 0444);
MODULE_PARM_DESC(flash_name, "Specify flash type name to spi_nor_scan(). Default (null) is auto-probe from JEDEC ID.");

static short chip_select = -1;
module_param(chip_select, short, 0444);
MODULE_PARM_DESC(chip_select, "Specify the alternate SPI CS# [0-3]. -1 means use existing (defaut).");

static char *part_name = "BIOS";
module_param(part_name, charp, 0444);
MODULE_PARM_DESC(part_name, "MTD partition label for SPIROM region.");

static short write_buffer_enable = -1;
module_param(write_buffer_enable, short, 0444);
MODULE_PARM_DESC(write_buffer_enable, "Enable write buffer.  This sets WriteBufferEn from BKDG.  0 means disable, >0 means enable, <0 means use existing (default)");

static short mac_arb_enable = -1;
module_param(mac_arb_enable, short, 0444);
MODULE_PARM_DESC(write_buffer_enable, "Enable MAC arbitration.  This sets SpiArbEnable from BKDG.  0 means disable, >0 means enable, <0 means use existing (default)");


static bool accelerated_rd = true;
module_param(accelerated_rd, bool, 0444);
MODULE_PARM_DESC(hw_write, "Have read requests go via flash MMIO address space. This is a performance enhancement.");

/*
 * The SPIROM interface only supports 1 flash chip so that's all the driver
 * supports.  Theoretically you could access up to 3 others via alt command
 * and SPI_ALT_CS but that's a future expansion and likely not ever to be
 * actually needed.
 */
struct amd_spirom {
	void __iomem *spi;
	void __iomem *rom;
	struct spi_nor nor;
	/*
	 * pre-calculated delays for reg_xfer (up to 8 bytes data + opcode)
	 * note that the delay includes the opcode byte and the value is double
	 * because reg_xfer is using poll_timeout which actually quarters the
	 * value for usleep_range.
	 */
	u32 reg_delay_us[9];
	u16 spi_alt_speed;
	bool mtd_registered;
};

/* note we're assuming no dual/quad here */
static u32 amd_spirom_get_usecs_per_bytes(u16 speed_config, u32 bytes)
{
	static const u32 hz[6] = {	(u32)(6666 * 1000),
					(u32)(3333 * 1000),
					(u32)(2222 * 1000),
					(u32)(1666 * 1000),
					(u32)(100000 * 1000),
					(u32)(800 * 1000) };

	return mult_frac(bytes * 8, USEC_PER_SEC, hz[speed_config]);
}

static int amd_spirom_reg_xfer(struct amd_spirom *spirom, u8 opcode,
				u8 rx_len, u8 tx_len, u8 *rx_buf, u8 *tx_buf)
{
	u32 delay_us;
	u32 total_len;
	u32 cntrl0;
	u8 sts;
	int rc;
	uint i;

	total_len = tx_len + rx_len + 1;

	/* transfer FIFO is actually 70 bytes long so that's the hard limit */

	/*
	 * no irq here so just need to wait
	 *
	 * poll_timeout will actually quarter the delay so we'll double the
	 * expected range and should then hit on the second or third iteration
	 *
	 * for reg_read/write transactions we can use the pre-calculated value
	 *
	 * Note we need to +1 for the opcode.
	 */


	if (total_len <= 9)
		delay_us = spirom->reg_delay_us[total_len - 1];
	else if (total_len <= 71)
		delay_us = amd_spirom_get_usecs_per_bytes(spirom->spi_alt_speed,
							  total_len << 1);
	else
		return -EINVAL;


	iowrite8(opcode, spirom->spi + CMD_CODE);

	iowrite8(tx_len, spirom->spi + TX_BYTE_COUNT);
	iowrite8(rx_len, spirom->spi + RX_BYTE_COUNT);

	/* reset the transfer fifo */
	cntrl0 = ioread32(spirom->spi + SPI_CNTRL0);
	cntrl0 &= SPI_CNTRL0_FIFO_PTR_CLEAR_MASK;
	cntrl0 |= SPI_CNTRL0_FIFO_PTR_CLEAR;
	iowrite32(cntrl0, spirom->spi + SPI_CNTRL0);

	/* fill the FIFO */
	for (i = 0; i < (uint)tx_len; i++)
		iowrite8(tx_buf[i], spirom->spi + SPI_FIFO + i);

	/* release the hounds... */
	iowrite8(CMD_TRIGGER_EXECUTE, spirom->spi + CMD_TRIGGER);

	rc = readb_poll_timeout(spirom->spi + SPI_STATUS, sts,
				!(sts & SPI_STATUS_BUSY_MASK),
				delay_us, 4 * delay_us);

	if (!rc) {
		/* drain the FIFO */
		for (i = 0; i < (uint)rx_len; i++)
			rx_buf[i] = ioread8(spirom->spi + SPI_FIFO + tx_len + i);
	}

	return rc;
}

static int amd_spirom_prepare(struct spi_nor *nor, enum spi_nor_ops ops)
{
	/* get the semaphore */
	struct pci_dev *pcidev = to_pci_dev(nor->dev);
	u32 pci_control;
	int rc = -ETIMEDOUT;
	int count;

	/*
	 * procedure from BKDG is to wait for EC_SEM to be 0, then write 1 to
	 * BIOS_SEM, then read back to verify the 1 has been set by HW to grant
	 * ownership
	 *
	 * this is only necessary when the IMC is active
	 */
	for (count = 0; count < 100 && rc; count++) {
		pci_read_config_dword(pcidev, FCH_PCI_CONTROL, &pci_control);
		if (pci_control & FCH_EC_SEM)
			msleep(50);
		else {
			if (pci_control & FCH_BIOS_SEM)
				rc = 0;
			else
				pci_write_config_dword(pcidev, FCH_PCI_CONTROL,
						       pci_control | FCH_BIOS_SEM);
		}
	}

	return rc;
}

static void amd_spirom_unprepare(struct spi_nor *nor, enum spi_nor_ops ops)
{
	/* we assume we own the semaphore at this point */
	struct pci_dev *pcidev = to_pci_dev(nor->dev);
	u32 pci_control;

	pci_read_config_dword(pcidev, FCH_PCI_CONTROL, &pci_control);

	pci_control &= FCH_LEGACY_DMA;

	pci_write_config_dword(pcidev, FCH_PCI_CONTROL, pci_control);
}

static int amd_spirom_read_reg(struct spi_nor *nor, u8 opcode, u8 *buf, int len)
{
	struct amd_spirom *spirom = nor->priv;
	int rc;

	dev_dbg(nor->dev, "read_reg: op: 0x%02x  len: %d\n", opcode, len);

	rc = amd_spirom_reg_xfer(spirom, opcode, len, 0, buf, NULL);
	if (!rc)
		dev_dbg(nor->dev, "read_reg: %*ph\n", len, buf);
	else
		dev_dbg(nor->dev, "read_reg: failed: %d\n", rc);

	return rc;
}

static int amd_spirom_write_reg(struct spi_nor *nor, u8 opcode, u8 *buf, int len)
{
	struct amd_spirom *spirom = nor->priv;
	int rc;

	dev_dbg(nor->dev, "write_reg: op: 0x%02x  len: %d  data: %*ph\n",
		opcode, len, len, buf);

	rc = amd_spirom_reg_xfer(spirom, opcode, 0, len, NULL, buf);

	if (rc)
		dev_dbg(nor->dev, "write_reg: failed: %d\n", rc);

	return rc;
}

/*
 * fallback read op based on explicit read command
 * ideally the HW io will be used but this needs to be available if it's not
 * strapped, can't get the resources, etc.
 * spi_nor_read() will loop on the actual bytes read so we can just limit
 * ourselves to the 64-byte FIFO size.
 */
static ssize_t amd_spirom_read(struct spi_nor *nor, loff_t from,
			       size_t len, u_char *read_buf)
{
	struct amd_spirom *spirom = nor->priv;
	u32 *pbuf32;
	size_t head;
	size_t tail;
	size_t left;
	ssize_t rc;
	size_t cur;
	u8 *p_addr;
	u8 rd_len;
	size_t i;
	u32 addr;

	dev_dbg(nor->dev, "read: from: %lld  len: %zu\n", from, len);

	/*
	 * We only allow READ in the HWCAPS so most read operations should come
	 * with that opcode.  However, the SPI-NOR layer can swap out the read
	 * opcode with something else (i.e. Read SFDP) which must be handled via
	 * alt command and not the ROM IO
	 */
	if (spirom->rom && (nor->read_opcode == SPINOR_OP_READ)) {
		/*
		 * no need for status/busy as the HW controller will deal with
		 * that, but memcpy_fromio can make out-of-order fetches so need
		 * to do this explicitly
		 */
		left = len;
		head = from & 3;
		left -= head;
		tail = left & 3;
		left -= tail;
		cur = 0;

		for (i = 0; i < head; i++, cur++)
			read_buf[cur] = ioread8(spirom->rom + from + cur);

		pbuf32 = (u32 *)(read_buf + cur);
		left /= 4;

		for (i = 0; i < left; i++, cur += 4, pbuf32++)
			*pbuf32 = ioread32(spirom->rom + from + cur);

		for (i = 0; i < tail; i++, cur++)
			read_buf[cur] = ioread8(spirom->rom + from + cur);

		rc = (ssize_t)len;
	} else {

		addr = cpu_to_be32((u32)(from));
		p_addr = (u8 *)(&addr);

		rd_len = len > 64 ? 64 : (u8)len;

		if (nor->addr_width == 3)
			p_addr++;

		rc = amd_spirom_reg_xfer(spirom, nor->read_opcode, rd_len,
					 nor->addr_width, read_buf, p_addr);

		if (!rc)
			rc = (ssize_t)rd_len;
	}

	return rc;
}

static ssize_t amd_spirom_write(struct spi_nor *nor, loff_t to,
				size_t len, const u_char *write_buf)
{
	/*
	 * spi_nor_write() will have already segmented this into pages
	 * and we will have overridden the flash page size to 64 (or less)
	 * for the FIFO size so just need to serialize into one byte stream
	 * for the addr + data.
	 * Note Page Program takes a BE address.
	 * Supposedly the ROM IO space should be able to deal with writes but
	 * cant get it to work in practice.
	 */
	struct amd_spirom *spirom = nor->priv;
	u8 buf[4 + 64];
	u8 *p_buf = buf;
	u32 addr;
	int rc;

	dev_dbg(nor->dev, "write: to: %lld  len: %zu\n", to, len);

	if (len > 64)
		return -EINVAL;

	addr = cpu_to_be32((u32)(to));

	memcpy(buf, &addr, 4);

	memcpy(&buf[4], write_buf, len);

	if (nor->addr_width == 3)
		p_buf++;

	rc = amd_spirom_reg_xfer(spirom, nor->program_opcode, 0,
				 len + nor->addr_width, NULL, p_buf);

	return !rc ? (ssize_t)len : rc;
}

static int amd_spirom_spi_init(struct device *dev, struct amd_spirom *spirom,
			       bool imc_active)
{
	struct spi_nor_hwcaps hwcaps;
	bool update_speed_config = false;
	u32 spi_read_mode;
	u32 spi_cntrl0;
	u16 speed_config;
	u8 cs_val;
	int i;

	/*
	 * Speed and Mode and CS settings should be configured by BIOS
	 * but can be overridden by module param.
	 */

	cs_val = ioread8(spirom->spi + ALT_SPI_CS);
	if (chip_select >= 0) {
		if (chip_select < 4) {
			iowrite8((cs_val & ALT_SPI_CS_MASK) | (u8)(chip_select),
				 spirom->spi + ALT_SPI_CS);

			dev_info(dev, "updated CS from: %d to: %d\n",
				cs_val & 0xF, chip_select);

		} else
			dev_err(dev, "invalid chip_select value: %d\n",
				chip_select);
	} else
		chip_select = (short)(cs_val & 0x3);

	if (write_buffer_enable >= 0) {
		cs_val = ioread8(spirom->spi + ALT_SPI_CS);
		if (write_buffer_enable) {
			iowrite8(cs_val | ALT_SPI_CS_WR_BUF_EN,
				 spirom->spi + ALT_SPI_CS);
			dev_info(dev, "enabled write buffer\n");
		} else {
			iowrite8(cs_val & ~ALT_SPI_CS_WR_BUF_EN,
				 spirom->spi + ALT_SPI_CS);
			dev_info(dev, "disabled write buffer\n");
		}
	} else
		write_buffer_enable = (cs_val & ALT_SPI_CS_WR_BUF_EN) >> 2;

	speed_config = ioread16(spirom->spi + SPI100_SPEED_CONFIG);
	dev_info(dev, "SPI100 speed config: 0x%04x\n", speed_config);

	if (alt_speed >= 0) {
		if (alt_speed < 6) {
			speed_config &= 0xFF0F;
			speed_config |= alt_speed << 4;
			update_speed_config = true;
		} else
			dev_err(dev, "invalid alt_speed value: %d\n",
				alt_speed);
	}

	if (norm_speed >= 0) {
		if (norm_speed < 6) {
			speed_config &= 0x0FFF;
			speed_config |= norm_speed << 12;
			update_speed_config = true;
		} else
			dev_err(dev, "invalid norm_speed value: %d\n",
				norm_speed);
	}

	if (fast_speed >= 0) {
		if (fast_speed < 6) {
			speed_config &= 0xF0FF;
			speed_config |= fast_speed << 8;
			update_speed_config = true;
		} else
			dev_err(dev, "invalid fast_speed value: %d\n",
				fast_speed);
	}

	if (update_speed_config) {
		dev_info(dev, "updated SPI100 speed config: 0x%04x\n",
			 speed_config);
		/* do we need to enable 100 mode? */
		for (i = 0; i < 4; i++) {
			if (((speed_config >> i) & 0xF) == 4) {
				iowrite8(1, spirom->spi + SPI100_ENABLE);
				dev_info(dev, "SPI100 enabled\n");
				break;
			}
		}
		iowrite16(speed_config, spirom->spi + SPI100_SPEED_CONFIG);
	}

	/* AltSpeed is [7:4] */
	spirom->spi_alt_speed = (speed_config >> 4) & 0xF;

	/* which mode are we in, and therefore which reg to read? */
	spi_cntrl0 = ioread32(spirom->spi + SPI_CNTRL0);
	dev_info(dev, "SPI CNTRL0: 0x%08x\n", spi_cntrl0);

	if (mac_arb_enable >= 0) {
		if (mac_arb_enable) {
			spi_cntrl0 |= SPI_CNTRL0_SPI_ARB_ENABLE;
			dev_info(dev, "enabled MAC arbritration\n");
		} else {
			spi_cntrl0 &= SPI_CNTRL0_SPI_ARB_ENABLE;
			dev_info(dev, "disabled MAC arbitration\n");
		}
	} else
		mac_arb_enable = (spi_cntrl0 & SPI_CNTRL0_SPI_ARB_ENABLE) >> 19;
	/*
	 * this is a 3 bit mashup with bits [2:1] being [30:29]
	 * and bit [0] being [18] in the register
	 */
	spi_read_mode  = (spi_cntrl0 >> 28) & 0x6;
	spi_read_mode |= (spi_cntrl0 & BIT(18)) ? 1 : 0;
	if (read_mode >= 0) {
		if (read_mode < 8) {
			spi_read_mode = read_mode;
			spi_cntrl0 &= 0x17F80000;
			spi_cntrl0 |= (read_mode & 0x6) << 29;
			spi_cntrl0 |= (read_mode & 0x1) << 18;
			iowrite32(spi_cntrl0, spirom->spi + SPI_CNTRL0);
			dev_info(dev, "updated SPI_CNTRL0: 0x%08x\n",
				 spi_cntrl0);
		} else
			dev_err(dev, "invalid read_mode value %d\n", read_mode);
	}

	/* update the params */
	read_mode = spi_read_mode;
	alt_speed = spirom->spi_alt_speed;
	norm_speed = (speed_config >> 12) & 0xF;
	fast_speed = (speed_config >> 8) & 0xF;

	for (i = 0; i < 10; i++)
		spirom->reg_delay_us[i - 1] = amd_spirom_get_usecs_per_bytes(alt_speed,
									     i << 1);

	/* as far as the spi-nor layer is concerned we can only do READ */
	hwcaps.mask = SNOR_HWCAPS_READ | SNOR_HWCAPS_PP;

	spirom->nor.dev = dev;
	spirom->nor.priv = spirom;

	spirom->nor.read_reg = amd_spirom_read_reg;
	spirom->nor.write_reg = amd_spirom_write_reg;
	spirom->nor.read = amd_spirom_read;
	spirom->nor.write = amd_spirom_write;

	/*
	 * for now the un/prepare functions only deal with IMC so can global
	 * enable/disable here.  if it turns out later we need other
	 * functionality we'll have to filter IMC presence in the actual
	 * routines.
	 */
	if (imc_active) {
		spirom->nor.prepare = amd_spirom_prepare;
		spirom->nor.unprepare = amd_spirom_unprepare;
	}

	return spi_nor_scan(&spirom->nor, flash_name, &hwcaps);
}

static int amd_spirom_mtd_init(struct device *dev, struct amd_spirom *spirom)
{
	struct mtd_partition part;
	int rc;

	/* need to define a basic partition for the region */

	memset(&part, 0, sizeof(part));

	/* Eventually add a CBFS region parser? */

	part.name = part_name;
	part.size = MTDPART_SIZ_FULL;

	/*
	 * need to override the flash page size to our 64-byte FIFO
	 * limit so we can let the spi-nor layer take care of the
	 * segmentation for us
	 */
	if (spirom->nor.page_size > 64) {
		spirom->nor.page_size = 64;
		spirom->nor.mtd.writebufsize = 64;
	}

	rc = mtd_device_parse_register(&spirom->nor.mtd, NULL, NULL, &part, 1);

	if (rc)
		dev_err(dev, "failed MTD device register: %d\n", rc);

	spirom->mtd_registered = !rc;

	return rc;
}

static bool amd_spirom_imc_enabled(struct pci_dev *pcidev)
{
	bool imc_enabled = false;
	void __iomem *acpi_misc;
	u32 imc_present;
	u8 imc_active;

	pci_read_config_dword(pcidev, FCH_PCI_CONTROL, &imc_present);
	if (imc_present & FCH_INTEGRATED_EC_PRESENT) {
		/* this is the hard-coded AcpiMmio Misc region address */
		acpi_misc = ioremap_nocache(0xFED80E00, 256);
		if (acpi_misc) {
			imc_active = ioread8(acpi_misc + 0x80);
			imc_enabled = imc_active & 0x04;
			iounmap(acpi_misc);
			if (imc_enabled)
				dev_info(&pcidev->dev, "IMC is enabled\n");
		} else {
			dev_warn(&pcidev->dev, "failed to map AcpiMmio Misc region, assuming IMC is enabled\n");
			imc_enabled = true;
		}
	}

	return imc_enabled;
}

static int amd_spirom_pci_probe(struct pci_dev *pcidev,
				const struct pci_device_id *id)
{
	struct cpuinfo_x86 *cpu = &boot_cpu_data;
	struct device *dev = &pcidev->dev;
	struct amd_spirom *spirom;
	struct resource *res;
	u32 spi_base_addr;
	u32 rom_addr_range;
	u32 rom_addr_start;
	u32 rom_addr_end;
	u32 rom_len;
	bool imc_active;
	int rc;

	/*
	 * Shouldn't get here without an AMD vendor code so ignore that,
	 * but there are slight variations in the family/model implementations
	 * that we need to be aware of.
	 * Right now we only support Family 16h, all models seem to work the
	 * same.
	 * Is there a device/function version number somewhere?  That would be
	 * better to switch off of but can't seem to find anything suitable in
	 * the BKDG.
	 */
	if (cpu->x86 != 0x16) {
		dev_err(dev,
			"unsupported CPU family: 0x%02x, only support 16h\n",
			cpu->x86);
		return -ENODEV;
	}

	rc = pcim_enable_device(pcidev);
	if (rc) {
		dev_err(dev, "failed pci enable: %d\n", rc);
		return rc;
	}

	pci_set_master(pcidev);

	spirom = devm_kzalloc(dev, sizeof(*spirom), GFP_KERNEL);
	if (!spirom) {
		rc = -ENOMEM;
		dev_err(dev, "failed spirom alloc\n");
		goto err_disable;
	}

	pci_set_drvdata(pcidev, spirom);

	pci_read_config_dword(pcidev, FCH_SPI_BASE_ADDR, &spi_base_addr);

	dev_info(dev, "SPI base addr: 0x%08x\n", spi_base_addr);

	/*
	 * set up the SPI region
	 * the coreboot Hudson ACPI configuration sets this as a PNP0C02 BAR0
	 * resource, which then gets claimed by the system driver. Problem is
	 * that there's a bug there where they use the SPI_Base_Addr field
	 * without masking the RouteTpm2Spi and SpiRomEnable bits so the whole
	 * region can be incorrectly offset, which can then fail the resource
	 * requests because of the bad overlap.  There doesn't seem to be any
	 * ideal way to fix it in an external module so if detected just warn.
	 */

	res = devm_request_mem_region(dev,
				      spi_base_addr & FCH_SPI_BASE_ADDR_MASK,
				      SPI_IO_REGION_LEN, "amd-spirom-spi");
	if (!res)
		dev_warn(dev, "cannot claim SPI region, this is likely a harmless bug in BIOS and can usually be ignored\n");

	spirom->spi = devm_ioremap_nocache(dev,
					   spi_base_addr & FCH_SPI_BASE_ADDR_MASK,
					   SPI_IO_REGION_LEN);
	if (!spirom->spi) {
		rc = -ENOMEM;
		dev_err(dev, "failed to remap SPI region\n");
		goto err_disable;
	}

	imc_active = amd_spirom_imc_enabled(pcidev);

	rc = amd_spirom_spi_init(dev, spirom, imc_active);
	if (rc)
		goto err_disable;

	/*
	 * look to see if we can use the HW IO support
	 * if the SPIROM interface is strapped then HW has mapped the SPI flash
	 * to the ROM regsions 1 and 2, which are the BIOS regions
	 * we want to use ROM ADDR 2 as that's the 4GB - size range
	 * but the controller assumes a 24-bit flash so only enable in that case
	 */

	if (accelerated_rd && (spirom->nor.addr_width == 3) &&
	    (spi_base_addr & FCH_SPI_ROM_ENABLE)) {

		pci_read_config_dword(pcidev, FCH_ROM_ADDR_RANGE_2,
				      &rom_addr_range);

		dev_info(dev, "ROM address range 2: 0x%08x\n", rom_addr_range);

		rom_addr_start = (rom_addr_range & 0xFFFF) << 16;
		rom_addr_end = ((rom_addr_range & 0xFFFF0000) | 0xFFFF);
		rom_len = rom_addr_end - rom_addr_start + 1;

		dev_info(dev, "ROM base: 0x%08x  ROM len: %d\n", rom_addr_start,
			 rom_len);

		/*
		 * ok now try to request the ROM regions.
		 * maybe do a pci_quirk with this at some point to reserve
		 * early?
		 */

		res = devm_request_mem_region(dev, rom_addr_start, rom_len,
						"amd-spirom-rom");
		if (res) {
			spirom->rom = devm_ioremap_nocache(dev, res->start,
							   rom_len);
			if (!spirom->rom)
				dev_warn(dev, "failed to map ROM region, cannot accelerate read\n");
		} else
			dev_warn(dev, "failed to request ROM region, cannot accelerate read\n");
	}


	rc = amd_spirom_mtd_init(dev, spirom);
	if (rc)
		goto err_disable;

	dev_info(dev, "enabled\n");

	return 0;

err_disable:
	pci_disable_device(pcidev);
	return rc;
}

static void amd_spirom_pci_remove(struct pci_dev *pcidev)
{
	struct amd_spirom *spirom = pci_get_drvdata(pcidev);

	if (spirom && spirom->mtd_registered) {
		dev_dbg(&pcidev->dev, "mtd unregister\n");
		mtd_device_unregister(&spirom->nor.mtd);
	}

	dev_info(&pcidev->dev, "exit\n");

	/* devres should clean up everything else */

	pci_disable_device(pcidev);
}

static struct pci_driver amd_spirom_pci_driver = {
	.name =	"amd-spirom",
	.id_table = amd_fch_lpc_pci_device_ids,
	.probe =	amd_spirom_pci_probe,
	.remove =	amd_spirom_pci_remove,

	/*
	 * no need to worry about power ops here,
	 * this whole interface is idle until explicit request
	 */
};

module_pci_driver(amd_spirom_pci_driver);

MODULE_DESCRIPTION("MTD SPI-NOR driver for AMD Hudson FCH SPIROM");
MODULE_AUTHOR("Brett Grandbois <brett.grandbois@opengear.com>");
MODULE_LICENSE("GPL");
