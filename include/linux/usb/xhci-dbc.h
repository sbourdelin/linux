/*
 * xHCI debug capability driver
 *
 * Copyright (C) 2015 Intel Corporation
 *
 * Author: Lu Baolu <baolu.lu@linux.intel.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#ifndef __LINUX_XHCI_DBC_H
#define __LINUX_XHCI_DBC_H

#include <linux/types.h>
#include <linux/usb/ch9.h>

/**
 * struct xdbc_regs - xHCI Debug Capability Register interface.
 */
struct xdbc_regs {
	__le32	capability;
	__le32	doorbell;
	__le32	ersts;		/* Event Ring Segment Table Size*/
	__le32	rvd0;		/* 0c~0f reserved bits */
	__le64	erstba;		/* Event Ring Segment Table Base Address */
	__le64	erdp;		/* Event Ring Dequeue Pointer */
	__le32	control;
#define	DEBUG_MAX_BURST(p)	(((p) >> 16) & 0xff)
#define	CTRL_DCR		(1 << 0)	/* DbC Run */
#define	CTRL_PED		(1 << 1)	/* Port Enable/Disable */
#define	CTRL_HOT		(1 << 2)	/* Halt Out TR */
#define	CTRL_HIT		(1 << 3)	/* Halt In TR */
#define	CTRL_DRC		(1 << 4)	/* DbC run change */
#define	CTRL_DCE		(1 << 31)	/* DbC enable */
#define	CTRL_LSE		(1 << 1)
	__le32	status;
#define	DCST_DPN(p)		(((p) >> 24) & 0xff)
	__le32	portsc;		/* Port status and control */
#define	PORTSC_CCS		(1 << 0)
#define	PORTSC_CSC		(1 << 17)
#define	PORTSC_PRC		(1 << 21)
#define	PORTSC_PLC		(1 << 22)
#define	PORTSC_CEC		(1 << 23)
	__le32	rvd1;		/* 2b~28 reserved bits */
	__le64	dccp;		/* Debug Capability Context Pointer */
	__le32	devinfo1;	/* Device Descriptor Info Register 1 */
	__le32	devinfo2;	/* Device Descriptor Info Register 2 */
};

/*
 * xHCI Debug Capability data structures
 */
struct xdbc_trb {
	__le32 field[4];
};

struct xdbc_erst_entry {
	__le64	seg_addr;
	__le32	seg_size;
	__le32	rsvd;
};

struct xdbc_info_context {
	__le64	string0;
	__le64	manufacture;
	__le64	product;
	__le64	serial;
	__le32	length;
	__le32	rsvdz[7];
};

struct xdbc_ep_context {
	__le32	ep_info1;
	__le32	ep_info2;
	__le64	deq;
	__le32	tx_info;
	__le32	rsvd0[11];
};

struct xdbc_context {
	struct xdbc_info_context	info;
	struct xdbc_ep_context		out;
	struct xdbc_ep_context		in;
};

#define	XDBC_INFO_CONTEXT_SIZE		48

#define	XDBC_MAX_STRING_LENGTH		64
#define	XDBC_STRING_MANUFACTURE		"Linux"
#define	XDBC_STRING_PRODUCT		"Remote GDB"
#define	XDBC_STRING_SERIAL		"0001"
struct xdbc_strings {
	char	string0[XDBC_MAX_STRING_LENGTH];
	char	manufacture[XDBC_MAX_STRING_LENGTH];
	char	product[XDBC_MAX_STRING_LENGTH];
	char	serial[XDBC_MAX_STRING_LENGTH];
};

/*
 * software state structure
 */
struct xdbc_segment {
	struct xdbc_trb		*trbs;
	dma_addr_t		dma;
};

#define	XDBC_TRBS_PER_SEGMENT	256

struct xdbc_ring {
	struct xdbc_segment	*segment;
	struct xdbc_trb		*enqueue;
	struct xdbc_trb		*dequeue;
	u32			cycle_state;
};

enum xdbc_page_type {
	XDBC_PAGE_EVENT,
	XDBC_PAGE_TXIN,
	XDBC_PAGE_TXOUT,
	XDBC_PAGE_TABLE,
	XDBC_PAGE_BUFFER,
};

enum xdbc_ep_state {
	EP_DISABLED,
	EP_RUNNING,
	EP_HALTED,
};
#define	XDBC_EPID_OUT	2
#define	XDBC_EPID_IN	1

struct xdbc_state {
	/* pci device info*/
	u32		bus;
	u32		dev;
	u32		func;
	u8		bar;
	u16		vendor;
	u16		device;
	void __iomem	*xhci_base;
	size_t		xhci_length;
#define	XDBC_PCI_MAX_BUSES		256
#define	XDBC_PCI_MAX_DEVICES		32
#define	XDBC_PCI_MAX_FUNCTION		8

	/* DbC register base */
	struct		xdbc_regs __iomem *xdbc_reg;

	/* DbC table page */
	dma_addr_t	table_dma;
	void		*table_base;

#define	XDBC_TABLE_ENTRY_SIZE		64
#define	XDBC_ERST_ENTRY_NUM		1
#define	XDBC_DBCC_ENTRY_NUM		3
#define	XDBC_STRING_ENTRY_NUM		4

	/* event ring segment table */
	dma_addr_t	erst_dma;
	size_t		erst_size;
	void		*erst_base;

	/* event ring segments */
	struct xdbc_ring	evt_ring;
	struct xdbc_segment	evt_seg;

	/* debug capability contexts */
	dma_addr_t	dbcc_dma;
	size_t		dbcc_size;
	void		*dbcc_base;

	/* descriptor strings */
	dma_addr_t	string_dma;
	size_t		string_size;
	void		*string_base;

	/* bulk OUT endpoint */
	struct xdbc_ring	out_ring;
	struct xdbc_segment	out_seg;
	void			*out_buf;
	dma_addr_t		out_dma;
	struct xdbc_trb		*out_pending;		/* IN */
	size_t			out_length;		/* IN */
	u32			out_complete;		/* OUT */
	size_t			out_complete_length;	/* OUT */
	enum xdbc_ep_state	out_ep_state;

	/* bulk IN endpoint */
	struct xdbc_ring	in_ring;
	struct xdbc_segment	in_seg;
	void			*in_buf;
	dma_addr_t		in_dma;
	struct xdbc_trb		*in_pending;		/* IN */
	size_t			in_length;		/* IN */
	u32			in_complete;		/* OUT */
	size_t			in_complete_length;	/* OUT */
	enum xdbc_ep_state	in_ep_state;

	/* atomic flags */
	unsigned long		atomic_flags;
#define	XDBC_ATOMIC_BULKOUT	0
#define	XDBC_ATOMIC_BULKIN	1
#define	XDBC_ATOMIC_EVENT	2
};

#define	XDBC_MAX_PACKET		1024
#define	XDBC_LOOPS		1000

/* door bell target */
#define	OUT_EP_DOORBELL		0
#define	IN_EP_DOORBELL		1
#define	DOOR_BELL_TARGET(p)	(((p) & 0xff) << 8)

#define	xdbc_read64(regs)	xhci_read_64(NULL, (regs))
#define	xdbc_write64(val, regs)	xhci_write_64(NULL, (val), (regs))

#ifdef CONFIG_EARLY_PRINTK_XDBC
extern int early_xdbc_init(char *s);
extern struct console early_xdbc_console;
#endif /* CONFIG_EARLY_PRINTK_XDBC */

#endif /* __LINUX_XHCI_DBC_H */
