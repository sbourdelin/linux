// SPDX-License-Identifier: GPL-2.0
/*
 * USBSSP device controller driver
 *
 * Copyright (C) 2018 Cadence.
 *
 * Author: Pawel Laszczak
 * Some code borrowed from the Linux XHCI driver.
 */

/* Up to 16 ms to halt an DC */
#define USBSSP_MAX_HALT_USEC		(16*1000)

/* DC not running - set to 1 when run/stop bit is cleared. */
#define USBSSP_STS_HALT			(1<<0)

/* HCCPARAMS offset from PCI base address */
#define USBSSP_HCC_PARAMS_OFFSET	0x10
/* HCCPARAMS contains the first extended capability pointer */
#define USBSSP_HCC_EXT_CAPS(p)	(((p)>>16)&0xffff)

/* Command and Status registers offset from the Operational Registers address */
#define USBSSP_CMD_OFFSET		0x00
#define USBSSP_STS_OFFSET		0x04

/* Capability Register */
/* bits 7:0 - how long is the Capabilities register */
#define USBSSP_HC_LENGTH(p)		(((p)>>00)&0x00ff)

/* Extended capability register fields */
#define USBSSP_EXT_CAPS_ID(p)		(((p)>>0)&0xff)
#define USBSSP_EXT_CAPS_NEXT(p)		(((p)>>8)&0xff)
#define	v_EXT_CAPS_VAL(p)		((p)>>16)
/* Extended capability IDs - ID 0 reserved */
#define USBSSP_EXT_CAPS_PROTOCOL		2

/* USB 2.0 hardware LMP capability*/
#define USBSSP_HLC			(1 << 19)
#define USBSSP_BLC			(1 << 20)

/* command register values to disable interrupts and halt the DC */
/* start/stop DC execution - do not write unless DC is halted*/
#define USBSSP_CMD_RUN			(1 << 0)
/* Event Interrupt Enable - get irq when EINT bit is set in USBSTS register */
#define USBSSP_CMD_EIE			(1 << 2)
/* Host System Error Interrupt Enable - get irq when HSEIE bit set in USBSTS */
#define USBSSP_CMD_HSEIE		(1 << 3)
/* Enable Wrap Event - '1' means DC generates an event when MFINDEX wraps. */
#define USBSSP_CMD_EWE			(1 << 10)

#define USBSSP_IRQS	(USBSSP_CMD_EIE | USBSSP_CMD_HSEIE | USBSSP_CMD_EWE)

/* true: Controller Not Ready to accept doorbell or op reg writes after reset */
#define USBSSP_STS_CNR			(1 << 11)

#include <linux/io.h>

/**
 * Find the offset of the extended capabilities with capability ID id.
 *
 * @base	PCI MMIO registers base address.
 * @start	address at which to start looking, (0 or HCC_PARAMS to start at
 *		beginning of list)
 * @id		Extended capability ID to search for.
 *
 * Returns the offset of the next matching extended capability structure.
 * Some capabilities can occur several times,
 * e.g., the USBSSP_EXT_CAPS_PROTOCOL, and this provides a way to find them all.
 */

static inline int usbssp_find_next_ext_cap(
		void __iomem *base, u32 start, int id)
{
	u32 val;
	u32 next;
	u32 offset;

	offset = start;
	if (!start || start == USBSSP_HCC_PARAMS_OFFSET) {
		val = readl(base + USBSSP_HCC_PARAMS_OFFSET);
		if (val == ~0)
			return 0;
		offset = USBSSP_HCC_EXT_CAPS(val) << 2;
		if (!offset)
			return 0;
	};
	do {
		val = readl(base + offset);
		if (val == ~0)
			return 0;
		if (USBSSP_EXT_CAPS_ID(val) == id && offset != start)
			return offset;

		next = USBSSP_EXT_CAPS_NEXT(val);
		offset += next << 2;
	} while (next);

	return 0;
}
