#ifndef _KS7010_SDIO_H
#define _KS7010_SDIO_H

#include "common.h"

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
#define	ROM_FILE "ks7010sd.rom"

int ks7010_sdio_tx(struct ks7010 *ks, u8 *data, size_t size);

#endif	/* _KS7010_SDIO_H */
