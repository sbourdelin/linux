#include <linux/module.h>
#include <linux/mmc/card.h>
#include <linux/mmc/mmc.h>
#include <linux/mmc/host.h>
#include <linux/mmc/sdio_func.h>
#include <linux/mmc/sdio_ids.h>
#include <linux/mmc/sdio.h>
#include <linux/mmc/sd.h>

#include "ks7010.h"
#include "sdio.h"

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

	u8 *fw;
	size_t fw_size;

	char fw_version[ETHTOOL_FWVERS_LEN];
	size_t fw_version_len;
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
		ks_err("sdio read byte failed %d", ret);
	sdio_release_host(func);

	return ret;
}

/**
 * ks7010_sdio_read() - Read data from SDIO device.
 * @ks: The ks7010 device.
 * @addr: SDIO device address to read from.
 * @buf: Buffer to read data into.
 * @len: Number of bytes to read.
 */
static int ks7010_sdio_read(struct ks7010 *ks, int addr, void *buf, size_t len)
{
	struct sdio_func *func = ks_to_func(ks);
	int ret;

	sdio_claim_host(func);
	ret = sdio_memcpy_fromio(func, buf, addr, len);
	if (ret)
		ks_err("sdio read failed %d", ret);
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
		ks_err("sdio write byte failed %d", ret);
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
		ks_err("sdio write failed %d", ret);
	sdio_release_host(func);

	return ret;
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
	ks_debug("not implemented yet");
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
		ks_err("set sdio block size %d failed: %d)\n",
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
		ks_err("failed to init ks_sdio: %d", ret);
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
		ks_err("failed to config ks_sdio: %d", ret);
		goto err_ks_destroy;
	}

	ret = ks7010_init(ks);
	if (ret) {
		ks_err("failed to init ks7010");
		goto err_ks_destroy; /* FIXME undo ks7010_sdio_config() */
	}

	ret = ks7010_sdio_enable_interrupts(ks);
	if (ret) {
		ks_err("failed to enable interrupts");
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
		ks_err("failed to register sdio driver: %d", ret);

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
