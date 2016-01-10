#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/cdev.h>
#include <linux/uaccess.h>
#include <linux/device.h>
#include <linux/spi/spi.h>

#include "linux_wlan_common.h"
#include "linux_wlan_spi.h"

#define USE_SPI_DMA     0       /* johnny add */

#ifdef WILC_ASIC_A0
 #if defined(PLAT_PANDA_ES_OMAP4460)
  #define MIN_SPEED 12000000
  #define MAX_SPEED 24000000
 #elif defined(PLAT_WMS8304)
  #define MIN_SPEED 12000000
  #define MAX_SPEED 24000000 /* 4000000 */
 #elif defined(CUSTOMER_PLATFORM)
/*
  TODO : define Clock speed under 48M.
 *
 * ex)
 * #define MIN_SPEED 24000000
 * #define MAX_SPEED 48000000
 */
 #else
  #define MIN_SPEED 24000000
  #define MAX_SPEED 48000000
 #endif
#else /* WILC_ASIC_A0 */
/* Limit clk to 6MHz on FPGA. */
 #define MIN_SPEED 6000000
 #define MAX_SPEED 6000000
#endif /* WILC_ASIC_A0 */

static u32 SPEED = MIN_SPEED;

struct spi_device *wilc_spi_dev;
void linux_spi_deinit(void *vp);

static int __init wilc_bus_probe(struct spi_device *spi)
{

	PRINT_D(BUS_DBG, "spiModalias: %s\n", spi->modalias);
	PRINT_D(BUS_DBG, "spiMax-Speed: %d\n", spi->max_speed_hz);
	wilc_spi_dev = spi;

	printk("Driver Initializing success\n");
	return 0;
}

static int __exit wilc_bus_remove(struct spi_device *spi)
{

	return 0;
}

#ifdef CONFIG_OF
static const struct of_device_id wilc1000_of_match[] = {
	{ .compatible = "atmel,wilc_spi", },
	{}
};
MODULE_DEVICE_TABLE(of, wilc1000_of_match);
#endif

struct spi_driver wilc_bus __refdata = {
	.driver = {
		.name = MODALIAS,
#ifdef CONFIG_OF
		.of_match_table = wilc1000_of_match,
#endif
	},
	.probe =  wilc_bus_probe,
	.remove = __exit_p(wilc_bus_remove),
};


void linux_spi_deinit(void *vp)
{

	spi_unregister_driver(&wilc_bus);

	SPEED = MIN_SPEED;
	PRINT_ER("@@@@@@@@@@@@ restore SPI speed to %d @@@@@@@@@\n", SPEED);

}



int linux_spi_init(void *vp)
{
	int ret = 1;
	static int called;


	if (called == 0) {
		called++;
		ret = spi_register_driver(&wilc_bus);
	}

	/* change return value to match WILC interface */
	(ret < 0) ? (ret = 0) : (ret = 1);

	return ret;
}

static void linux_spi_msg_init(struct spi_message *msg, struct spi_transfer *tr,
			       u32 len, u8 *tx, u8 *rx)
{
	spi_message_init(msg);
	memset(tr, 0, sizeof(*tr));

	msg->spi = wilc_spi_dev;
	msg->is_dma_mapped = USE_SPI_DMA;

	tr->tx_buf = tx;
	tr->rx_buf = rx;

	tr->len = len;
	tr->speed_hz = SPEED;
	tr->bits_per_word = 8;

	spi_message_add_tail(tr, msg);
}

#if defined(PLAT_WMS8304)
#define TXRX_PHASE_SIZE (4096)
#endif

#if defined(TXRX_PHASE_SIZE)

int linux_spi_write(u8 *b, u32 len)
{
	int ret;
	struct spi_message msg;
	struct spi_transfer tr;

	if (len > 0 && b != NULL) {
		int i = 0;
		int blk = len / TXRX_PHASE_SIZE;
		int remainder = len % TXRX_PHASE_SIZE;

		if (blk) {
			while (i < blk)	{
				linux_spi_msg_init(&msg, &tr, TXRX_PHASE_SIZE,
						   b + (i * TXRX_PHASE_SIZE),
						   NULL);

				ret = spi_sync(wilc_spi_dev, &msg);
				if (ret < 0) {
					PRINT_ER("SPI transaction failed\n");
				}
				i++;

			}
		}
		if (remainder) {
			linux_spi_msg_init(&msg, &tr, remainder,
					   b + (blk * TXRX_PHASE_SIZE),
					   NULL);

			ret = spi_sync(wilc_spi_dev, &msg);
			if (ret < 0) {
				PRINT_ER("SPI transaction failed\n");
			}
		}
	} else {
		PRINT_ER("can't write data with the following length: %d\n", len);
		PRINT_ER("FAILED due to NULL buffer or ZERO length check the following length: %d\n", len);
		ret = -1;
	}

	/* change return value to match WILC interface */
	(ret < 0) ? (ret = 0) : (ret = 1);

	return ret;

}

#else
int linux_spi_write(u8 *b, u32 len)
{

	int ret;
	struct spi_message msg;
	struct spi_transfer tr;

	if (len > 0 && b != NULL) {
		linux_spi_msg_init(&msg, &tr, len, b, NULL);

		ret = spi_sync(wilc_spi_dev, &msg);
		if (ret < 0) {
			PRINT_ER("SPI transaction failed\n");
		}

	} else {
		PRINT_ER("can't write data with the following length: %d\n", len);
		PRINT_ER("FAILED due to NULL buffer or ZERO length check the following length: %d\n", len);
		ret = -1;
	}

	/* change return value to match WILC interface */
	(ret < 0) ? (ret = 0) : (ret = 1);


	return ret;
}

#endif

#if defined(TXRX_PHASE_SIZE)

int linux_spi_read(u8 *rb, u32 rlen)
{
	int ret;
	struct spi_message msg;
	struct spi_transfer tr;

	if (rlen > 0) {
		int i = 0;

		int blk = rlen / TXRX_PHASE_SIZE;
		int remainder = rlen % TXRX_PHASE_SIZE;

		if (blk) {
			while (i < blk)	{
				linux_spi_msg_init(&msg, &tr, TXRX_PHASE_SIZE,
						   NULL,
						   rb + (i * TXRX_PHASE_SIZE));

				ret = spi_sync(wilc_spi_dev, &msg);
				if (ret < 0) {
					PRINT_ER("SPI transaction failed\n");
				}
				i++;
			}
		}
		if (remainder) {
			linux_spi_msg_init(&msg, &tr, remainder, NULL,
					   rb + (blk * TXRX_PHASE_SIZE));

			ret = spi_sync(wilc_spi_dev, &msg);
			if (ret < 0) {
				PRINT_ER("SPI transaction failed\n");
			}
		}
	} else {
		PRINT_ER("can't read data with the following length: %u\n", rlen);
		ret = -1;
	}
	/* change return value to match WILC interface */
	(ret < 0) ? (ret = 0) : (ret = 1);

	return ret;
}

#else
int linux_spi_read(u8 *rb, u32 rlen)
{

	int ret;
	struct spi_message msg;
	struct spi_transfer tr;

	if (rlen > 0) {
		linux_spi_msg_init(&msg, &tr, rlen, NULL, rb);

		ret = spi_sync(wilc_spi_dev, &msg);
		if (ret < 0) {
			PRINT_ER("SPI transaction failed\n");
		}
	} else {
		PRINT_ER("can't read data with the following length: %u\n", rlen);
		ret = -1;
	}
	/* change return value to match WILC interface */
	(ret < 0) ? (ret = 0) : (ret = 1);

	return ret;
}

#endif

int linux_spi_write_read(u8 *wb, u8 *rb, u32 rlen)
{
	int ret;
	struct spi_message msg;
	struct spi_transfer tr;

	if (!rlen) {
		PRINT_ER("Zero length read/write.\n");
		return 0;
	}

	if (!wb || !rb) {
		PRINT_ER("Read or write buffer NULL.\n");
		return 0;
	}

	linux_spi_msg_init(&msg, &tr, rlen, wb, rb);
	ret = spi_sync(wilc_spi_dev, &msg);
	if (ret < 0)
		PRINT_ER("SPI sync failed and returned %d.\n", ret);

	/* change return value to match WILC interface */
	(ret < 0) ? (ret = 0) : (ret = 1);
	return ret;
}

int linux_spi_set_max_speed(void)
{
	SPEED = MAX_SPEED;

	PRINT_INFO(BUS_DBG, "@@@@@@@@@@@@ change SPI speed to %d @@@@@@@@@\n", SPEED);
	return 1;
}
