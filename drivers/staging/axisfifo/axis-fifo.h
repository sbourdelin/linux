/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Xilinx AXIS FIFO: interface to the Xilinx AXI-Stream FIFO IP core
 *
 * Copyright (C) 2018 Jacob Feder
 *
 * Authors:  Jacob Feder <jacobsfeder@gmail.com>
 */

// See Xilinx PG080 document for IP details

#ifndef __AXIS_FIFO_H__
#define __AXIS_FIFO_H__

/* ----------------------------
 *           includes
 * ----------------------------
 */

#include <linux/kernel.h>
#include <linux/mutex.h>
#include <linux/wait.h>
#include <linux/spinlock_types.h>
#include <linux/device.h>
#include <linux/cdev.h>

/* ----------------------------
 *       driver parameters
 * ----------------------------
 */

#define DRIVER_NAME "axis_fifo"

#define READ_BUFF_SIZE 128 /* read buffer length in words */
#define WRITE_BUFF_SIZE 128 /* write buffer length in words */

/* ----------------------------
 *     IP register offsets
 * ----------------------------
 */

#define XLLF_ISR_OFFSET  0x00000000  /* Interrupt Status */
#define XLLF_IER_OFFSET  0x00000004  /* Interrupt Enable */

#define XLLF_TDFR_OFFSET 0x00000008  /* Transmit Reset */
#define XLLF_TDFV_OFFSET 0x0000000c  /* Transmit Vacancy */
#define XLLF_TDFD_OFFSET 0x00000010  /* Transmit Data */
#define XLLF_TLR_OFFSET  0x00000014  /* Transmit Length */

#define XLLF_RDFR_OFFSET 0x00000018  /* Receive Reset */
#define XLLF_RDFO_OFFSET 0x0000001c  /* Receive Occupancy */
#define XLLF_RDFD_OFFSET 0x00000020  /* Receive Data */
#define XLLF_RLR_OFFSET  0x00000024  /* Receive Length */
#define XLLF_SRR_OFFSET  0x00000028  /* Local Link Reset */
#define XLLF_TDR_OFFSET  0x0000002C  /* Transmit Destination */
#define XLLF_RDR_OFFSET  0x00000030  /* Receive Destination */

/* ----------------------------
 *     IP register masks
 * ----------------------------
 */

#define XLLF_INT_RPURE_MASK       0x80000000 /* Receive under-read */
#define XLLF_INT_RPORE_MASK       0x40000000 /* Receive over-read */
#define XLLF_INT_RPUE_MASK        0x20000000 /* Receive underrun (empty) */
#define XLLF_INT_TPOE_MASK        0x10000000 /* Transmit overrun */
#define XLLF_INT_TC_MASK          0x08000000 /* Transmit complete */
#define XLLF_INT_RC_MASK          0x04000000 /* Receive complete */
#define XLLF_INT_TSE_MASK         0x02000000 /* Transmit length mismatch */
#define XLLF_INT_TRC_MASK         0x01000000 /* Transmit reset complete */
#define XLLF_INT_RRC_MASK         0x00800000 /* Receive reset complete */
#define XLLF_INT_TFPF_MASK        0x00400000 /* Tx FIFO Programmable Full */
#define XLLF_INT_TFPE_MASK        0x00200000 /* Tx FIFO Programmable Empty */
#define XLLF_INT_RFPF_MASK        0x00100000 /* Rx FIFO Programmable Full */
#define XLLF_INT_RFPE_MASK        0x00080000 /* Rx FIFO Programmable Empty */
#define XLLF_INT_ALL_MASK         0xfff80000 /* All the ints */
#define XLLF_INT_ERROR_MASK       0xf2000000 /* Error status ints */
#define XLLF_INT_RXERROR_MASK     0xe0000000 /* Receive Error status ints */
#define XLLF_INT_TXERROR_MASK     0x12000000 /* Transmit Error status ints */

// associated with the reset registers
#define XLLF_RDFR_RESET_MASK        0x000000a5 /* receive reset value */
#define XLLF_TDFR_RESET_MASK        0x000000a5 /* Transmit reset value */
#define XLLF_SRR_RESET_MASK         0x000000a5 /* Local Link reset value */

/* ----------------------------
 *            types
 * ----------------------------
 */

struct axis_fifo_local {
	int irq; /* interrupt */
	unsigned int mem_start; /* physical memory start address */
	unsigned int mem_end; /* physical memory end address */
	void __iomem *base_addr; /* kernel space memory */

	unsigned int rx_fifo_depth; /* max words in the receive fifo */
	unsigned int tx_fifo_depth; /* max words in the transmit fifo */
	int has_rx_fifo; /* whether the IP has the rx fifo enabled */
	int has_tx_fifo; /* whether the IP has the tx fifo enabled */

	struct mutex read_mutex;/* prevent multiple processes from reading */
	struct mutex write_mutex; /* prevent multiple processes from writing */
	wait_queue_head_t read_queue; /* wait queue for asynchronos read */
	spinlock_t read_queue_lock; /* lock for reading waitqueue */
	wait_queue_head_t write_queue; /* wait queue for asynchronos write */
	spinlock_t write_queue_lock; /* lock for writing waitqueue */
	unsigned int write_flags; /* write file flags */
	unsigned int read_flags; /* read file flags */

	struct device *os_device; /* device created by OS */
	struct device *device; /* our device */
	unsigned int id; /* our unique id */
	dev_t devt; /* our char device number */
	struct class *driver_class; /* our char device class */
	struct cdev char_device; /* our char device */
};

#endif
