/*
 * Driver for KeyStream wireless LAN cards.
 *
 * Copyright (C) 2005-2008 KeyStream Corp.
 * Copyright (C) 2009 Renesas Technology Corp.
 * Copyright (C) 2017 Tobin C. Harding.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */

#include <linux/module.h>
#include <linux/mmc/card.h>
#include <linux/mmc/mmc.h>
#include <linux/mmc/host.h>
#include <linux/mmc/sdio_func.h>
#include <linux/mmc/sdio_ids.h>
#include <linux/mmc/sdio.h>
#include <linux/mmc/sd.h>
#include <linux/firmware.h>

#include "ks7010.h"
#include "sdio.h"

/*  SDIO KeyStream vendor and device */
#define SDIO_VENDOR_ID_KS_CODE_A	0x005b
#define SDIO_VENDOR_ID_KS_CODE_B	0x0023

/* Older sources suggest earlier versions were named 7910 or 79xx */
#define SDIO_DEVICE_ID_KS_7010		0x7910

#define KS7010_IO_BLOCK_SIZE 512

/* read status register */
#define READ_STATUS_ADDR	0x000000
#define READ_STATUS_BUSY	0
#define READ_STATUS_IDLE	1

/* read index register */
#define READ_INDEX_ADDR		0x000004

/* read data size register */
#define READ_DATA_SIZE_ADDR	0x000008

/* write index register */
#define WRITE_INDEX_ADDR	0x000010

/* write status register */
#define WRITE_STATUS_ADDR	0x00000C
#define WRITE_STATUS_BUSY	0
#define WRITE_STATUS_IDLE	1

/* [write status] / [read data size] register
 * Used for network packets less than 2048 bytes data.
 */
#define WSTATUS_RSIZE_ADDR	0x000014
#define WSTATUS_MASK		0x80
#define RSIZE_MASK		0x7F

/* ARM to SD interrupt enable */
#define INT_ENABLE_ADDR		0x000020
#define INT_DISABLE		0

/* ARM to SD interrupt pending */
#define INT_PENDING_ADDR	0x000024
#define INT_CLEAR		0xFF

/* General Communication Register A */
#define GCR_A_ADDR		0x000028
enum gen_com_reg_a {
	GCR_A_INIT = 0,
	GCR_A_REMAP,
	GCR_A_RUN
};

/* General Communication Register B */
#define GCR_B_ADDR		0x00002C
enum gen_com_reg_b {
	GCR_B_ACTIVE = 0,
	GCR_B_SLEEP
};

#define INT_GCR_B		BIT(7)
#define INT_GCR_A		BIT(6)
#define INT_WRITE_STATUS	BIT(5)
#define INT_WRITE_INDEX		BIT(4)
#define INT_WRITE_SIZE		BIT(3)
#define INT_READ_STATUS		BIT(2)
#define INT_READ_INDEX		BIT(1)
#define INT_READ_SIZE		BIT(0)

/* wake up register */
#define WAKEUP_ADDR		0x008018
#define WAKEUP_REQ		0x5a

/* AHB Data Window  0x010000-0x01FFFF */
#define DATA_WINDOW_ADDR	0x010000
#define DATA_WINDOW_SIZE	(64 * 1024)

#define KS7010_IRAM_ADDR	0x06000000

/**
 * enum ks7010_sdio_state - SDIO device state.
 * @SDIO_DISABLED: SDIO function is disabled.
 * @SDIO_ENABLED: SDIO function is enabled.
 */
enum ks7010_sdio_state {
	SDIO_DISABLED,
	SDIO_ENABLED
};

/**
 * struct ks7010_sdio - SDIO device private data.
 * @func: The SDIO function device.
 * @ks: The ks7010 device.
 * @id: The SDIO device identifier.
 * @state: The SDIO device state, &enum ks7010_sdio_sate.
 * @fw: Firmware for the device.
 * @fw_size: Size of the firmware.
 * @fw_version: Firmware version string.
 * @fw_version_len: Length of firmware version string.
 */
struct ks7010_sdio {
	struct sdio_func *func;
	struct ks7010 *ks;

	const struct sdio_device_id *id;
	enum ks7010_sdio_state state;
};

static struct sdio_func *ks_to_func(struct ks7010 *ks)
{
	struct ks7010_sdio *ks_sdio = ks->priv;

	if (ks_sdio->state != SDIO_ENABLED) {
		ks_debug("sdio_func is not ready");
		return NULL;
	}

	return ks_sdio->func;
}

/**
 * ks7010_sdio_readb() - Read a single byte from SDIO device.
 * @ks: The ks7010 device.
 * @addr: SDIO device address to read from.
 * @byte: Pointer to store byte read.
 */
static int ks7010_sdio_readb(struct ks7010 *ks, int addr, u8 *byte)
{
	struct sdio_func *func = ks_to_func(ks);
	int ret;

	sdio_claim_host(func);
	*byte = sdio_readb(func, addr, &ret);
	if (ret)
		ks_debug("sdio read byte failed %d", ret);
	sdio_release_host(func);

	return ret;
}

/**
 * ks7010_sdio_read() - Read data from SDIO device.
 * @ks: The ks7010 device.
 * @dst: Destination buffer to read data into.
 * @addr: SDIO device address to read from.
 * @count: Number of bytes to read.
 */
static int ks7010_sdio_read(struct ks7010 *ks, u8 *dst, unsigned int addr,
			    size_t count)
{
	struct sdio_func *func = ks_to_func(ks);
	int ret;

	sdio_claim_host(func);
	ret = sdio_memcpy_fromio(func, dst, addr, count);
	if (ret) {
		ks_debug("sdio read failed (%d) from addr: %X count: %zu",
			 ret, addr, count);
	}

	sdio_release_host(func);

	return ret;
}

/**
 * ks7010_sdio_writeb() - Write a single byte to SDIO device.
 * @ks: The ks7010 device.
 * @addr: SDIO device address to write to.
 * @byte: Byte to write.
 */
static int ks7010_sdio_writeb(struct ks7010 *ks, int addr, u8 byte)
{
	struct sdio_func *func = ks_to_func(ks);
	int ret;

	sdio_claim_host(func);
	sdio_writeb(func, byte, addr, &ret);
	if (ret)
		ks_debug("sdio write byte failed %d", ret);
	sdio_release_host(func);

	return ret;
}

/**
 * ks7010_sdio_write() - Write data to SDIO device.
 * @ks: The ks7010 device.
 * @addr: SDIO device address to write to.
 * @buf: Source data buffer.
 * @len: Number of bytes to write.
 */
static int ks7010_sdio_write(struct ks7010 *ks, int addr, void *buf, size_t len)
{
	struct sdio_func *func = ks_to_func(ks);
	int ret;

	sdio_claim_host(func);
	ret = sdio_memcpy_toio(func, addr, buf, len);
	if (ret)
		ks_debug("sdio write failed %d", ret);
	sdio_release_host(func);

	return ret;
}

#define ALL_BITS_CLEAR 0x00

/**
 * ks7010_sdio_read_trx_status_byte() - Tx/rx status information.
 * @ks: The ks7010 device.
 *
 * Reads the appropriate registers on the device, returns information
 * encoded in device specific format. Extract status information using
 * @ks7010_sdio_can_tx() and TODO add rx documentation.
 */
u8 ks7010_sdio_read_trx_status_byte(struct ks7010 *ks)
{
	int ret;
	u8 status;

	ret = ks7010_sdio_readb(ks, WSTATUS_RSIZE_ADDR, &status);
	if (ret)
		return ALL_BITS_CLEAR;

	return status;
}

/**
 * ks7010_sdio_can_tx() - True if device is ready to transmit.
 * @ks: The ks7010 device.
 * @trx_status_byte: Byte returned by @ks7010_sdio_read_trx_status()
 */
bool ks7010_sdio_can_tx(struct ks7010 *ks, u8 trx_status_byte)
{
	return trx_status_byte & WSTATUS_MASK;
}

/**
 * ks7010_sdio_set_read_status_idle() - Set the device read status to idle.
 * @ks: The ks7010 device.
 *
 * Called after rx frame has been read from the device.
 */
void ks7010_sdio_set_read_status_idle(struct ks7010 *ks)
{
	ks7010_sdio_writeb(ks, READ_STATUS_ADDR, READ_STATUS_IDLE);
}

/**
 * ks7010_sdio_tx() - Write tx data to the device.
 * @ks: The ks7010 device.
 * @data: The data to write.
 * @size: Write size, must be aligned.
 */
int ks7010_sdio_tx(struct ks7010 *ks, u8 *data, size_t size)
{
	int ret;

	ret = ks7010_sdio_write(ks, DATA_WINDOW_SIZE, data, size);
	if (ret)
		return ret;

	ret = ks7010_sdio_writeb(ks, WRITE_STATUS_ADDR, WRITE_STATUS_BUSY);
	if (ret)
		return ret;

	return 0;
}

/**
 * ks7010_sdio_rx_read() - Read rx data from the device.
 * @ks: The ks7010 device.
 * @buf: Read destination buf.
 * @size: Number of octets to read, must be aligned.
 */
int ks7010_sdio_rx_read(struct ks7010 *ks, u8 *buf, size_t size)
{
	int ret;

	ret = ks7010_sdio_read(ks, buf, DATA_WINDOW_ADDR, size);
	if (ret)
		return ret;

	return 0;
}

static int ks7010_sdio_enable_interrupts(struct ks7010 *ks)
{
	struct sdio_func *func = ks_to_func(ks);
	u8 byte;
	int ret;

	sdio_claim_host(func);

	ret = ks7010_sdio_writeb(ks, INT_PENDING_ADDR, INT_CLEAR);
	if (ret)
		goto out;

	byte = (INT_GCR_B | INT_READ_STATUS | INT_WRITE_STATUS);
	ret = ks7010_sdio_writeb(ks, INT_ENABLE_ADDR, byte);
out:
	sdio_release_host(func);
	return ret;
}

/**
 * ks7010_sdio_interrupt() - Interrupt handler for device.
 * @func: The SDIO function.
 */
static void ks7010_sdio_interrupt(struct sdio_func *func)
{
	struct ks7010_sdio *ks_sdio;
	struct ks7010 *ks;
	u8 status, byte;
	int ret;

	ks_sdio = sdio_get_drvdata(func);
	ks = ks_sdio->ks;

	ret = ks7010_sdio_readb(ks, INT_PENDING_ADDR, &status);
	if (ret)
		return;

	/* TODO check if device just woke up */

	do {
		ret = ks7010_sdio_readb(ks, WSTATUS_RSIZE_ADDR, &byte);
		if (ret)
			return;

		/* rx frame arrival */
		if (byte & RSIZE_MASK) {
			u16 size = (byte & RSIZE_MASK) << 4;

			ks7010_rx(ks, size);
		}

		/* tx frame transmit complete */
		if (byte & WSTATUS_MASK)
			ks7010_tx_hw(ks);

	/* FIXME why is this done in a loop? */
	} while (byte & RSIZE_MASK);
}

static int ks7010_sdio_update_index(struct ks7010 *ks, u32 index)
{
	/* FIXME SDIO code should not have to know about FIL endianness */
	__le32 di;
	int ret;

	di = cpu_to_le32(index);

	ret = ks7010_sdio_write(ks, WRITE_INDEX_ADDR, &di, sizeof(di));
	if (ret)
		return -EIO;

	ret = ks7010_sdio_write(ks, READ_INDEX_ADDR, &di, sizeof(di));
	if (ret)
		return -EIO;

	return 0;
}

/**
 * ks7010_sdio_fw_is_running() - True if firmware is running.
 * @ks: The ks7010 device.
 */
bool ks7010_sdio_fw_is_running(struct ks7010 *ks)
{
	int ret;
	u8 byte;

	ret = ks7010_sdio_readb(ks, GCR_A_ADDR, &byte);
	if (ret)
		return false;

	if (byte == GCR_A_RUN)
		return true;

	return false;
}

/**
 * ks7010_sdio_upload_fw() - Upload firmware.
 * @ks: The ks7010 device.
 * @fw: Pointer to the firmware data.
 * @fw_size: Size of firmware.
 */
int ks7010_sdio_upload_fw(struct ks7010 *ks, u8 *fw, size_t fw_size)
{
	size_t remaining;
	int offset;
	u8 *buf;
	int ret;

	buf = kmalloc(DATA_WINDOW_SIZE, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	remaining = fw_size;
	offset = 0;

	ks_debug("attempting to upload %zu bytes of firmware", remaining);

	while (remaining > 0) {
		int trf = 0;

		if (remaining > DATA_WINDOW_SIZE) {
			trf = DATA_WINDOW_SIZE;
			remaining -= DATA_WINDOW_SIZE;
		} else {
			trf = remaining;
			remaining = 0;
		}

		ret = ks7010_sdio_update_index(ks, KS7010_IRAM_ADDR + offset);
		if (ret)
			goto free_buf;

		/* upload firmware chunk */
		ret = ks7010_sdio_write(ks, DATA_WINDOW_ADDR, fw + offset, trf);
		if (ret)
			goto free_buf;

		ks_debug("wrote %d bytes to device address: %X with offset %X",
			 trf, DATA_WINDOW_ADDR, offset);

		/* verify chunk transfer */
		ret = ks7010_sdio_read(ks, buf, DATA_WINDOW_ADDR, trf);
		if (ret)
			goto free_buf;

		if (memcmp(buf, fw + offset, trf) != 0) {
			ks_debug("fw upload failed: data compare error");
			ret = -EIO;
			goto free_buf;
		}

		offset += trf;
	}

	ret = ks7010_sdio_writeb(ks, GCR_A_ADDR, GCR_A_REMAP);
	if (ret)
		goto free_buf;

	ret = 0;

free_buf:
	kfree(buf);
	return ret;
}

/* called before ks7010 device is initialized */
static int ks7010_sdio_init(struct ks7010_sdio *ks_sdio,
			    const struct sdio_device_id *id)
{
	struct sdio_func *func = ks_sdio->func;
	int ret = -ENODEV;

	ks_sdio->id = id;

	sdio_claim_host(func);

	ret = sdio_enable_func(func);
	if (ret)
		goto err_release;

	sdio_writeb(func, INT_DISABLE, INT_ENABLE_ADDR, &ret);
	if (ret) {
		ret = -EIO;
		goto err_disable_func;
	}

	sdio_writeb(func, INT_CLEAR, INT_PENDING_ADDR, &ret);
	if (ret) {
		ret = -EIO;
		goto err_disable_func;
	}

	ret = sdio_claim_irq(func, ks7010_sdio_interrupt);
	if (ret)
		goto err_disable_func;

	sdio_release_host(func);

	ks_sdio->state = SDIO_ENABLED;

	return 0;

err_release:
	sdio_release_host(func);
err_disable_func:
	sdio_disable_func(func);

	return ret;
}

static void ks7010_sdio_cleanup(struct ks7010 *ks)
{
	struct sdio_func *func = ks_to_func(ks);

	sdio_claim_host(func);

	sdio_release_irq(func);
	sdio_disable_func(func);

	sdio_release_host(func);
}

static int ks7010_sdio_config(struct ks7010 *ks)
{
	struct sdio_func *func = ks_to_func(ks);
	int ret;

	sdio_claim_host(func);

	/* give us some time to enable, in ms */
	func->enable_timeout = 100;

	ret = sdio_set_block_size(func, KS7010_IO_BLOCK_SIZE);
	if (ret) {
		ks_debug("set sdio block size %d failed: %d)\n",
			 KS7010_IO_BLOCK_SIZE, ret);
		goto out;
	}

out:
	sdio_release_host(func);

	return ret;
}

static int ks7010_sdio_probe(struct sdio_func *func,
			     const struct sdio_device_id *id)
{
	struct ks7010_sdio *ks_sdio;
	struct ks7010 *ks;
	int ret;

	ks_debug("sdio new func %d vendor 0x%x device 0x%x block 0x%x/0x%x",
		 func->num, func->vendor, func->device,
		 func->max_blksize, func->cur_blksize);

	ks_sdio = kzalloc(sizeof(*ks_sdio), GFP_KERNEL);
	if (!ks_sdio)
		return -ENOMEM;

	ks_sdio->state = SDIO_DISABLED;

	ks_sdio->func = func;
	sdio_set_drvdata(func, ks_sdio);

	ret = ks7010_sdio_init(ks_sdio, id);
	if (ret) {
		ks_debug("failed to init ks_sdio: %d", ret);
		goto err_sdio_free;
	}

	ks = ks7010_create(&func->dev);
	if (!ks) {
		ret = -ENOMEM;
		goto err_sdio_cleanup;
	}

	ks_sdio->ks = ks;
	ks->priv = ks_sdio;

	ret = ks7010_sdio_config(ks);
	if (ret) {
		ks_debug("failed to config ks_sdio: %d", ret);
		goto err_ks_destroy;
	}

	ret = ks7010_init(ks);
	if (ret) {
		ks_debug("failed to init ks7010");
		goto err_ks_destroy;
	}

	ret = ks7010_sdio_enable_interrupts(ks);
	if (ret) {
		ks_debug("failed to enable interrupts");
		goto err_ks_cleanup;
	}

	ks->state = KS7010_STATE_READY;
	ks_info("SDIO device successfully probed");

	return 0;

err_ks_cleanup:
	ks7010_cleanup(ks);
err_ks_destroy:
	ks7010_destroy(ks);
err_sdio_cleanup:
	ks7010_sdio_cleanup(ks);
err_sdio_free:
	kfree(ks_sdio);

	return ret;
}

static void ks7010_sdio_remove(struct sdio_func *func)
{
	struct ks7010_sdio *ks_sdio = sdio_get_drvdata(func);
	struct ks7010 *ks = ks_sdio->ks;

	ks_debug("sdio removed func %d vendor 0x%x device 0x%x",
		 func->num, func->vendor, func->device);

	ks7010_destroy(ks);

	ks7010_sdio_cleanup(ks);

	sdio_set_drvdata(func, NULL);
	kfree(ks_sdio);

	ks_info("SDIO device removed");
}

static const struct sdio_device_id ks7010_sdio_ids[] = {
	{SDIO_DEVICE(SDIO_VENDOR_ID_KS_CODE_A, SDIO_DEVICE_ID_KS_7010)},
	{SDIO_DEVICE(SDIO_VENDOR_ID_KS_CODE_B, SDIO_DEVICE_ID_KS_7010)},
	{ /* all zero */ }
};
MODULE_DEVICE_TABLE(sdio, ks7010_sdio_ids);

static struct sdio_driver ks7010_sdio_driver = {
	.name = "ks7010_sdio",
	.id_table = ks7010_sdio_ids,
	.probe = ks7010_sdio_probe,
	.remove = ks7010_sdio_remove,
};

static int __init ks7010_sdio_module_init(void)
{
	int ret;

	ret = sdio_register_driver(&ks7010_sdio_driver);
	if (ret)
		ks_debug("failed to register sdio driver: %d", ret);

	ks_info("module loaded");
	ks_debug("debugging output enabled");

	return ret;
}

static void __exit ks7010_sdio_module_exit(void)
{
	sdio_unregister_driver(&ks7010_sdio_driver);
	ks_info("module unloaded");
}

module_init(ks7010_sdio_module_init);
module_exit(ks7010_sdio_module_exit);

MODULE_AUTHOR("Tobin C. Harding");
MODULE_AUTHOR("Sang Engineering, Qi-Hardware, KeyStream");
MODULE_DESCRIPTION("Driver for KeyStream KS7010 based SDIO cards");
MODULE_LICENSE("GPL");
