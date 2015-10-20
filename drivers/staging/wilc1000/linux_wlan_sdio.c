#include "wilc_wfi_netdevice.h"
#include "wilc_wfi_netdevice.h"

#include <linux/mmc/sdio_func.h>
#include <linux/mmc/card.h>
#include <linux/mmc/sdio_ids.h>
#include <linux/mmc/sdio.h>
#include <linux/mmc/host.h>
#include <linux/of_gpio.h>



#define SDIO_MODALIAS "wilc1000_sdio"

#if defined(CUSTOMER_PLATFORM)
/* TODO : User have to stable bus clock as user's environment. */
 #ifdef MAX_BUS_SPEED
 #define MAX_SPEED MAX_BUS_SPEED
 #else
 #define MAX_SPEED 50000000
 #endif
#else
 #define MAX_SPEED (6 * 1000000) /* Max 50M */
#endif


static struct sdio_func *wilc1000_sdio_func;
static unsigned int sdio_default_speed;

#define SDIO_VENDOR_ID_WILC 0x0296
#define SDIO_DEVICE_ID_WILC 0x5347

static const struct sdio_device_id wilc_sdio_ids[] = {
	{ SDIO_DEVICE(SDIO_VENDOR_ID_WILC, SDIO_DEVICE_ID_WILC) },
	{ },
};


static void wilc_sdio_interrupt(struct sdio_func *func)
{
	sdio_release_host(func);
	wilc_handle_isr();
	sdio_claim_host(func);
}

static int wilc1000_sdio_cmd52(sdio_cmd52_t *cmd)
{
	struct sdio_func *func = container_of(wilc1000_dev->dev, struct sdio_func, dev);
	int ret;
	u8 data;

	sdio_claim_host(func);

	func->num = cmd->function;
	if (cmd->read_write) {  /* write */
		if (cmd->raw) {
			sdio_writeb(func, cmd->data, cmd->address, &ret);
			data = sdio_readb(func, cmd->address, &ret);
			cmd->data = data;
		} else {
			sdio_writeb(func, cmd->data, cmd->address, &ret);
		}
	} else {        /* read */
		data = sdio_readb(func, cmd->address, &ret);
		cmd->data = data;
	}

	sdio_release_host(func);

	if (ret < 0) {
		PRINT_ER("wilc_sdio_cmd52..failed, err(%d)\n", ret);
		return 0;
	}
	return 1;
}


static int wilc1000_sdio_cmd53(sdio_cmd53_t *cmd)
{
	struct sdio_func *func = container_of(wilc1000_dev->dev, struct sdio_func, dev);
	int size, ret;

	sdio_claim_host(func);

	func->num = cmd->function;
	func->cur_blksize = cmd->block_size;
	if (cmd->block_mode)
		size = cmd->count * cmd->block_size;
	else
		size = cmd->count;

	if (cmd->read_write) {  /* write */
		ret = sdio_memcpy_toio(func, cmd->address, (void *)cmd->buffer, size);
	} else {        /* read */
		ret = sdio_memcpy_fromio(func, (void *)cmd->buffer, cmd->address,  size);
	}

	sdio_release_host(func);


	if (ret < 0) {
		PRINT_ER("wilc_sdio_cmd53..failed, err(%d)\n", ret);
		return 0;
	}

	return 1;
}

static const struct wilc1000_ops wilc1000_sdio_ops;

#ifdef COMPLEMENT_BOOT
/* FIXME: remove all of COMPLEMENT_BOOT */

static struct sdio_driver wilc_bus;
static volatile int wilc1000_probe;

#define READY_CHECK_THRESHOLD		30
static u8 wilc1000_prepare_11b_core(struct wilc *nic)
{
	u8 trials = 0;

	while ((wilc1000_core_11b_ready() && (READY_CHECK_THRESHOLD > (trials++)))) {
		PRINT_D(INIT_DBG, "11b core not ready yet: %u\n", trials);
		wilc_wlan_cleanup();
		wilc_wlan_global_reset();
		sdio_unregister_driver(&wilc_bus);

		sdio_register_driver(&wilc_bus);

		while (!wilc1000_probe)
			msleep(100);
		wilc1000_probe = 0;
		wilc1000_dev->dev = &wilc1000_sdio_func->dev;
		nic->ops = &wilc1000_sdio_ops;
		wilc_wlan_init(nic);
	}

	if (READY_CHECK_THRESHOLD <= trials)
		return 1;
	else
		return 0;

}

static int repeat_power_cycle(perInterface_wlan_t *nic)
{
	int ret = 0;
	sdio_unregister_driver(&wilc_bus);

	sdio_register_driver(&wilc_bus);

	/* msleep(1000); */
	while (!wilc1000_probe)
		msleep(100);
	wilc1000_probe = 0;
	wilc1000_dev->dev = &wilc1000_sdio_func->dev;
	wilc1000_dev->ops = &wilc1000_sdio_ops;
	ret = wilc_wlan_init(wilc1000_dev);

	wilc1000_dev->mac_status = WILC_MAC_STATUS_INIT;
	if (wilc1000_dev->gpio < 0)
		wilc1000_dev->ops->enable_interrupt(wilc1000_dev);

	if (wilc1000_wlan_get_firmware(nic)) {
		PRINT_ER("Can't get firmware\n");
		ret = -1;
		goto __fail__;
	}

	/*Download firmware*/
	ret = wilc1000_firmware_download(wilc1000_dev);
	if (ret < 0) {
		PRINT_ER("Failed to download firmware\n");
		goto __fail__;
	}
	/* Start firmware*/
	ret = wilc1000_start_firmware(nic);
	if (ret < 0)
		PRINT_ER("Failed to start firmware\n");
__fail__:
	return ret;
}
#endif

static int linux_sdio_probe(struct sdio_func *func, const struct sdio_device_id *id)
{
	int gpio;

	PRINT_D(INIT_DBG, "probe function\n");

#ifdef COMPLEMENT_BOOT
	if (wilc1000_sdio_func != NULL) {
		wilc1000_sdio_func = func;
		wilc1000_probe = 1;
		PRINT_D(INIT_DBG, "wilc1000_sdio_func isn't NULL\n");
		return 0;
	}
#endif

	gpio = -1;
	if (IS_ENABLED(CONFIG_WILC1000_HW_OOB_INTR)) {
		gpio = of_get_gpio(func->dev.of_node, 0);
		if (gpio < 0)
			gpio = GPIO_NUM;
	}

	PRINT_D(INIT_DBG, "Initializing netdev\n");
	wilc1000_sdio_func = func;
	if (wilc_netdev_init(&func->dev, &wilc1000_sdio_ops,
	    &wilc1000_hif_sdio, gpio)) {
		PRINT_ER("Couldn't initialize netdev\n");
		return -1;
	}
	wilc1000_dev->dev = &wilc1000_sdio_func->dev;

	printk("Driver Initializing success\n");
	return 0;
}

static void linux_sdio_remove(struct sdio_func *func)
{
	/**
	 *      TODO
	 **/

}

static struct sdio_driver wilc_bus = {
	.name		= SDIO_MODALIAS,
	.id_table	= wilc_sdio_ids,
	.probe		= linux_sdio_probe,
	.remove		= linux_sdio_remove,
};

static int wilc1000_sdio_enable_interrupt(struct wilc *dev)
{
	struct sdio_func *func = container_of(dev->dev, struct sdio_func, dev);
	int ret = 0;

	sdio_claim_host(func);
	ret = sdio_claim_irq(func, wilc_sdio_interrupt);
	sdio_release_host(func);

	if (ret < 0) {
		PRINT_ER("can't claim sdio_irq, err(%d)\n", ret);
		ret = -EIO;
	}
	return ret;
}

static void wilc1000_sdio_disable_interrupt(struct wilc *dev)
{
	struct sdio_func *func = container_of(dev->dev, struct sdio_func, dev);
	int ret;

	PRINT_D(INIT_DBG, "wilc1000_sdio_disable_interrupt IN\n");

	sdio_claim_host(func);
	ret = sdio_release_irq(func);
	if (ret < 0) {
		PRINT_ER("can't release sdio_irq, err(%d)\n", ret);
	}
	sdio_release_host(func);

	PRINT_D(INIT_DBG, "wilc1000_sdio_disable_interrupt OUT\n");
}

static int linux_sdio_set_speed(int speed)
{
	struct mmc_ios ios;

	sdio_claim_host(wilc1000_sdio_func);

	memcpy((void *)&ios, (void *)&wilc1000_sdio_func->card->host->ios, sizeof(struct mmc_ios));
	wilc1000_sdio_func->card->host->ios.clock = speed;
	ios.clock = speed;
	wilc1000_sdio_func->card->host->ops->set_ios(wilc1000_sdio_func->card->host, &ios);
	sdio_release_host(wilc1000_sdio_func);
	PRINT_INFO(INIT_DBG, "@@@@@@@@@@@@ change SDIO speed to %d @@@@@@@@@\n", speed);

	return 1;
}

static int linux_sdio_get_speed(void)
{
	return wilc1000_sdio_func->card->host->ios.clock;
}

static int wilc1000_sdio_init(void *pv)
{

	/**
	 *      TODO :
	 **/


	sdio_default_speed = linux_sdio_get_speed();
	return 1;
}

static void wilc1000_sdio_deinit(void *pv)
{

	/**
	 *      TODO :
	 **/


	sdio_unregister_driver(&wilc_bus);
}

static int wilc1000_sdio_set_max_speed(void)
{
	return linux_sdio_set_speed(MAX_SPEED);
}

static int wilc1000_sdio_set_default_speed(void)
{
	return linux_sdio_set_speed(sdio_default_speed);
}

static const struct wilc1000_ops wilc1000_sdio_ops = {
	.io_type = HIF_SDIO,
	.io_init = wilc1000_sdio_init,
	.io_deinit = wilc1000_sdio_deinit,
#ifdef COMPLEMENT_BOOT
	.repeat_power_cycle = repeat_power_cycle,
	.prepare_11b_core = wilc1000_prepare_11b_core,
#endif
	.enable_interrupt = wilc1000_sdio_enable_interrupt,
	.disable_interrupt = wilc1000_sdio_disable_interrupt,
	.u.sdio.sdio_cmd52 = wilc1000_sdio_cmd52,
	.u.sdio.sdio_cmd53 = wilc1000_sdio_cmd53,
	.u.sdio.sdio_set_max_speed = wilc1000_sdio_set_max_speed,
	.u.sdio.sdio_set_default_speed = wilc1000_sdio_set_default_speed,
};

static int __init init_wilc_sdio_driver(void)
{
	wilc1000_init_driver();
	return sdio_register_driver(&wilc_bus);
}
late_initcall(init_wilc_sdio_driver);

static void __exit exit_wilc_sdio_driver(void)
{
	if (wilc1000_dev)
		wilc_netdev_free(wilc1000_dev);
	sdio_unregister_driver(&wilc_bus);
	wilc1000_exit_driver();
}
module_exit(exit_wilc_sdio_driver);
