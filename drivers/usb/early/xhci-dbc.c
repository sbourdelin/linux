/**
 * xhci-dbc.c - xHCI debug capability driver
 *
 * Copyright (C) 2015 Intel Corporation
 *
 * Author: Lu Baolu <baolu.lu@linux.intel.com>
 * Some code shared with EHCI debug port and xHCI driver.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/console.h>
#include <linux/pci_regs.h>
#include <linux/pci_ids.h>
#include <linux/bootmem.h>
#include <linux/io.h>
#include <asm/pci-direct.h>
#include <asm/fixmap.h>
#include <linux/bcd.h>
#include <linux/export.h>
#include <linux/version.h>
#include <linux/usb/xhci-dbc.h>

#include "../host/xhci.h"

#define	XDBC_PROTOCOL		1	/* GNU Remote Debug Command Set */
#define	XDBC_VENDOR_ID		0x1d6b	/* Linux Foundation 0x1d6b */
#define	XDBC_PRODUCT_ID		0x0004	/* __le16 idProduct; device 0004 */
#define	XDBC_DEVICE_REV		0x0010	/* 0.10 */

static struct xdbc_state xdbc_stat;
static struct xdbc_state *xdbcp = &xdbc_stat;

#ifdef DBC_DEBUG
#define	XDBC_DEBUG_BUF_SIZE	(PAGE_SIZE * 32)
#define	MSG_MAX_LINE		128
static char xdbc_debug_buf[XDBC_DEBUG_BUF_SIZE];
static void xdbc_trace(const char *fmt, ...)
{
	int i, size;
	va_list args;
	static int pos;
	char temp_buf[MSG_MAX_LINE];

	if (pos >= XDBC_DEBUG_BUF_SIZE - 1)
		return;

	memset(temp_buf, 0, MSG_MAX_LINE);
	va_start(args, fmt);
	vsnprintf(temp_buf, MSG_MAX_LINE - 1, fmt, args);
	va_end(args);

	i = 0;
	size = strlen(temp_buf);
	while (i < size) {
		xdbc_debug_buf[pos] = temp_buf[i];
		pos++;
		i++;

		if (pos >= XDBC_DEBUG_BUF_SIZE - 1)
			break;
	}
}

static void xdbc_dump_debug_buffer(void)
{
	int index = 0;
	int count = 0;
	char dump_buf[MSG_MAX_LINE];

	xdbc_trace("The end of DbC trace buffer\n");
	pr_notice("DBC debug buffer:\n");
	memset(dump_buf, 0, MSG_MAX_LINE);

	while (index < XDBC_DEBUG_BUF_SIZE) {
		if (!xdbc_debug_buf[index])
			break;

		if (xdbc_debug_buf[index] == '\n' ||
				count >= MSG_MAX_LINE - 1) {
			pr_notice("DBC: @%08x %s\n", index, dump_buf);
			memset(dump_buf, 0, MSG_MAX_LINE);
			count = 0;
		} else {
			dump_buf[count] = xdbc_debug_buf[index];
			count++;
		}

		index++;
	}
}

static void xdbc_dbg_dump_regs(char *str)
{
	if (!xdbcp->xdbc_reg) {
		xdbc_trace("register not mapped\n");
		return;
	}

	xdbc_trace("XDBC registers: %s\n", str);
	xdbc_trace("  Capability: %08x\n",
			readl(&xdbcp->xdbc_reg->capability));
	xdbc_trace("  Door bell: %08x\n",
			readl(&xdbcp->xdbc_reg->doorbell));
	xdbc_trace("  Event Ring Segment Table Size: %08x\n",
			readl(&xdbcp->xdbc_reg->ersts));
	xdbc_trace("  Event Ring Segment Table Base Address: %16llx\n",
			xdbc_read64(&xdbcp->xdbc_reg->erstba));
	xdbc_trace("  Event Ring Dequeue Pointer: %16llx\n",
			xdbc_read64(&xdbcp->xdbc_reg->erdp));
	xdbc_trace("  Port status and control: %08x\n",
			readl(&xdbcp->xdbc_reg->portsc));
	xdbc_trace("  Debug Capability Context Pointer: %16llx\n",
			xdbc_read64(&xdbcp->xdbc_reg->dccp));
	xdbc_trace("  Device Descriptor Info Register 1: %08x\n",
			readl(&xdbcp->xdbc_reg->devinfo1));
	xdbc_trace("  Device Descriptor Info Register 2: %08x\n",
			readl(&xdbcp->xdbc_reg->devinfo2));
}

static void xdbc_dbg_dump_info_context(char *str)
{
	int i;
	u64 addr;
	u32 *field;

	if (!xdbcp->dbcc_base)
		return;

	xdbc_trace("%s:\n", str);

	field = (u32 *)xdbcp->dbcc_base;
	addr = xdbcp->dbcc_dma;
	for (i = 0; i < XDBC_INFO_CONTEXT_SIZE;) {
		xdbc_trace("@%016llx %08x %08x %08x %08x\n",
			addr,
			le32_to_cpu(field[i]),
			le32_to_cpu(field[i + 1]),
			le32_to_cpu(field[i + 2]),
			le32_to_cpu(field[i + 3]));
		addr += 16;
		i += 4;
	}
}

static void xdbc_dbg_dump_erst(char *str)
{
	int i;
	u64 addr = xdbcp->erst_dma;
	struct xdbc_erst_entry *entry;

	if (!xdbcp->erst_base)
		return;

	xdbc_trace("%s\n", str);

	for (i = 0; i < xdbcp->erst_size / sizeof(*entry); i++) {
		entry = (struct xdbc_erst_entry *)xdbcp->erst_base + i;
		xdbc_trace("@%016llx %08x %08x %08x %08x\n",
			addr,
			lower_32_bits(le64_to_cpu(entry->seg_addr)),
			upper_32_bits(le64_to_cpu(entry->seg_addr)),
			le32_to_cpu(entry->seg_size),
			le32_to_cpu(entry->rsvd));
		addr += sizeof(*entry);
	}
}

static void xdbc_dbg_dump_segment(struct xdbc_segment *seg, char *str)
{
	int i;
	u64 addr = seg->dma;
	struct xdbc_trb *trb;

	if (!seg->trbs)
		return;

	xdbc_trace("%s\n", str);

	for (i = 0; i < XDBC_TRBS_PER_SEGMENT; i++) {
		trb = &seg->trbs[i];
		xdbc_trace("@%016llx %08x %08x %08x %08x\n", addr,
			le32_to_cpu(trb->field[0]),
			le32_to_cpu(trb->field[1]),
			le32_to_cpu(trb->field[2]),
			le32_to_cpu(trb->field[3]));
		addr += sizeof(*trb);
	}
}

static void xdbc_dbg_dump_string(char *str)
{
	u32 *string = (u32 *)xdbcp->string_base;
	int i, max;

	max = xdbcp->string_size / sizeof(u32);

	xdbc_trace("%s\n", str);

	for (i = 0; i < max; i += 4) {
		xdbc_trace("@%016llx %08x %08x %08x %08x\n",
			xdbcp->string_dma + i * 16,
			le32_to_cpu(string[0]),
			le32_to_cpu(string[1]),
			le32_to_cpu(string[2]),
			le32_to_cpu(string[3]));
		string += 4;
	}
}

static void xdbc_dbg_dump_data(char *str)
{
	xdbc_trace("XDBC data structure: %s\n", str);
	xdbc_dbg_dump_erst("ERST:");
	xdbc_dbg_dump_segment(&xdbcp->evt_seg, "Event Ring Segment:");
	xdbc_dbg_dump_segment(&xdbcp->out_seg, "TXout Ring Segment:");
	xdbc_dbg_dump_segment(&xdbcp->in_seg, "TXin Ring Segment:");
	xdbc_dbg_dump_info_context("DBCC:");
	xdbc_dbg_dump_string("String Descriptor:");
}

static void xdbc_dbg_dump_trb(struct xdbc_trb *trb, char *str)
{
	xdbc_trace("DBC trb: %s\n", str);
	xdbc_trace("@%016llx %08x %08x %08x %08x\n", (u64)__pa(trb),
				le32_to_cpu(trb->field[0]),
				le32_to_cpu(trb->field[1]),
				le32_to_cpu(trb->field[2]),
				le32_to_cpu(trb->field[3]));
}
#else
static inline void xdbc_trace(const char *fmt, ...) { }
static inline void xdbc_dump_debug_buffer(void) { }
static inline void xdbc_dbg_dump_regs(char *str) { }
static inline void xdbc_dbg_dump_data(char *str) { }
static inline void xdbc_dbg_dump_trb(struct xdbc_trb *trb, char *str) { }
#endif	/* DBC_DEBUG */

/*
 * FIXME: kernel provided delay interfaces, like usleep, isn't ready yet
 *        at the time DbC gets initialized. Below implementation is only
 *        for x86 platform. Need to reconsider this when porting it onto
 *        other architectures.
 */
static inline void xdbc_udelay(int us)
{
	while (us-- > 0)
		outb(0x1, 0x80);
}

static void __iomem *xdbc_map_pci_mmio(u32 bus,
		u32 dev, u32 func, u8 bar, size_t *length)
{
	u32 val, sz;
	u64 val64, sz64, mask64;
	u8 byte;
	unsigned long idx, max_idx;
	void __iomem *base;

	val = read_pci_config(bus, dev, func, bar);
	write_pci_config(bus, dev, func, bar, ~0);
	sz = read_pci_config(bus, dev, func, bar);
	write_pci_config(bus, dev, func, bar, val);
	if (val == 0xffffffff || sz == 0xffffffff) {
		xdbc_trace("invalid mmio bar\n");
		return NULL;
	}

	val64 = val & PCI_BASE_ADDRESS_MEM_MASK;
	sz64 = sz & PCI_BASE_ADDRESS_MEM_MASK;
	mask64 = (u32)PCI_BASE_ADDRESS_MEM_MASK;

	if ((val & PCI_BASE_ADDRESS_MEM_TYPE_MASK) ==
			PCI_BASE_ADDRESS_MEM_TYPE_64) {
		val = read_pci_config(bus, dev, func, bar + 4);
		write_pci_config(bus, dev, func, bar + 4, ~0);
		sz = read_pci_config(bus, dev, func, bar + 4);
		write_pci_config(bus, dev, func, bar + 4, val);

		val64 |= ((u64)val << 32);
		sz64 |= ((u64)sz << 32);
		mask64 |= ((u64)~0 << 32);
	}

	sz64 &= mask64;

	if (sizeof(dma_addr_t) < 8 || !sz64) {
		xdbc_trace("can't handle 64bit BAR\n");
		return NULL;
	}

	sz64 = 1ULL << __ffs64(sz64);

	if (sz64 > (FIX_XDBC_END - FIX_XDBC_BASE + 1) * PAGE_SIZE) {
		xdbc_trace("mmio size beyond 64k not supported\n");
		return NULL;
	}

	xdbc_trace("bar: base 0x%llx size 0x%llx offset %03x\n",
			val64, sz64, bar);

	/* check if the mem space is enabled */
	byte = read_pci_config_byte(bus, dev, func, PCI_COMMAND);
	if (!(byte & PCI_COMMAND_MEMORY)) {
		byte  |= PCI_COMMAND_MEMORY;
		write_pci_config_byte(bus, dev, func, PCI_COMMAND, byte);
		xdbc_trace("mmio for xhci enabled\n");
	}

	/* 64k mmio will be fix-mapped */
	max_idx = FIX_XDBC_END - FIX_XDBC_BASE;
	for (idx = 0; idx <= max_idx; idx++)
		set_fixmap_nocache(FIX_XDBC_BASE + idx,
			(val64 & PAGE_MASK) + (max_idx - idx) * PAGE_SIZE);
	base = (void __iomem *)__fix_to_virt(FIX_XDBC_END);
	base += val64 & ~PAGE_MASK;

	/* save in the state block */
	xdbcp->bus = bus;
	xdbcp->dev = dev;
	xdbcp->func = func;
	xdbcp->bar = bar;
	xdbcp->xhci_base = base;
	xdbcp->xhci_length = sz64;
	xdbcp->vendor = read_pci_config_16(bus, dev, func, PCI_VENDOR_ID);
	xdbcp->device = read_pci_config_16(bus, dev, func, PCI_DEVICE_ID);

	if (length)
		*length = sz64;

	return base;
}

/*
 * FIXME: The bootmem allocator isn't ready at the time when DbC gets
 *        initialized. Below implementation reserves DMA memory blocks
 *        in the kernel static data segment.
 */
static void *xdbc_get_page(dma_addr_t *dma_addr,
		enum xdbc_page_type type)
{
	void *virt;
	static char event_page[PAGE_SIZE] __aligned(PAGE_SIZE);
	static char in_ring_page[PAGE_SIZE] __aligned(PAGE_SIZE);
	static char out_ring_page[PAGE_SIZE] __aligned(PAGE_SIZE);
	static char table_page[PAGE_SIZE] __aligned(PAGE_SIZE);
	static char bulk_buf_page[PAGE_SIZE] __aligned(PAGE_SIZE);

	switch (type) {
	case XDBC_PAGE_EVENT:
		virt = (void *)event_page;
		break;
	case XDBC_PAGE_TXIN:
		virt = (void *)in_ring_page;
		break;
	case XDBC_PAGE_TXOUT:
		virt = (void *)out_ring_page;
		break;
	case XDBC_PAGE_TABLE:
		virt = (void *)table_page;
		break;
	case XDBC_PAGE_BUFFER:
		virt = (void *)bulk_buf_page;
		break;
	default:
		return NULL;
	}

	memset(virt, 0, PAGE_SIZE);

	if (dma_addr)
		*dma_addr = (dma_addr_t)__pa(virt);

	return virt;
}

typedef void (*xdbc_walk_excap_cb)(int cap_offset, void *data);

/*
 * xdbc_walk_excap:
 *
 * xHCI extended capability list walker.
 *
 * @bus - xHC PCI bus#
 * @dev - xHC PCI dev#
 * @func - xHC PCI function#
 * @cap - capability ID
 * @oneshot - return immediately once hit match
 * @cb - call back
 * @data - callback private data
 *
 * Return the last cap offset, otherwize 0.
 */
static u32 xdbc_walk_excap(u32 bus, u32 dev, u32 func, int cap,
		bool oneshot, xdbc_walk_excap_cb cb, void *data)
{
	void __iomem *base;
	int offset = 0;
	size_t len = 0;

	if (xdbcp->xhci_base && xdbcp->xhci_length) {
		if (xdbcp->bus != bus ||
				xdbcp->dev != dev ||
				xdbcp->func != func) {
			xdbc_trace("only one DbC can be used\n");
			return 0;
		}

		len = xdbcp->xhci_length;
		base = xdbcp->xhci_base;
	} else {
		base = xdbc_map_pci_mmio(bus, dev, func,
				PCI_BASE_ADDRESS_0, &len);
		if (!base)
			return 0;
	}

	do {
		offset = xhci_find_next_ext_cap(base, offset, cap);
		if (!offset)
			break;

		if (cb)
			cb(offset, data);
		if (oneshot)
			break;
	} while (1);

	return offset;
}

static u32 __init xdbc_find_dbgp(int xdbc_num,
		u32 *rbus, u32 *rdev, u32 *rfunc)
{
	u32 bus, dev, func, class;
	unsigned cap;

	for (bus = 0; bus < XDBC_PCI_MAX_BUSES; bus++) {
		for (dev = 0; dev < XDBC_PCI_MAX_DEVICES; dev++) {
			for (func = 0; func < XDBC_PCI_MAX_FUNCTION; func++) {
				class = read_pci_config(bus, dev, func,
						PCI_CLASS_REVISION);
				if ((class >> 8) != PCI_CLASS_SERIAL_USB_XHCI)
					continue;

				if (xdbc_num-- != 0)
					continue;

				cap = xdbc_walk_excap(bus, dev, func,
						XHCI_EXT_CAPS_DEBUG,
						true, NULL, NULL);
				*rbus = bus;
				*rdev = dev;
				*rfunc = func;
				return cap;
			}
		}
	}

	return 0;
}

static int handshake(void __iomem *ptr, u32 mask, u32 done,
		int wait_usec, int delay_usec)
{
	u32	result;

	do {
		result = readl(ptr);
		result &= mask;
		if (result == done)
			return 0;
		xdbc_udelay(delay_usec);
		wait_usec -= delay_usec;
	} while (wait_usec > 0);

	return -ETIMEDOUT;
}

static void __init xdbc_bios_handoff(void)
{
	int ext_cap_offset;
	int timeout;
	u32 val;

	ext_cap_offset = xdbc_walk_excap(xdbcp->bus,
					xdbcp->dev,
					xdbcp->func,
					XHCI_EXT_CAPS_LEGACY,
					true, NULL, NULL);
	val = readl(xdbcp->xhci_base + ext_cap_offset);

	/* If the BIOS owns the HC, signal that the OS wants it, and wait */
	if (val & XHCI_HC_BIOS_OWNED) {
		writel(val | XHCI_HC_OS_OWNED,
				xdbcp->xhci_base + ext_cap_offset);
		timeout = handshake(xdbcp->xhci_base + ext_cap_offset,
				XHCI_HC_BIOS_OWNED, 0, 5000, 10);

		/* Assume a buggy BIOS and take HC ownership anyway */
		if (timeout) {
			xdbc_trace("xHCI BIOS handoff failed (BIOS bug ?)\n");
			writel(val & ~XHCI_HC_BIOS_OWNED,
					xdbcp->xhci_base + ext_cap_offset);
		}
	}

	/* Disable any BIOS SMIs and clear all SMI events*/
	val = readl(xdbcp->xhci_base + ext_cap_offset +
			XHCI_LEGACY_CONTROL_OFFSET);
	val &= XHCI_LEGACY_DISABLE_SMI;
	val |= XHCI_LEGACY_SMI_EVENTS;
	writel(val, xdbcp->xhci_base + ext_cap_offset +
			XHCI_LEGACY_CONTROL_OFFSET);
}

/*
 * xdbc_alloc_ring: allocate physical memory for a ring
 */
static int xdbc_alloc_ring(struct xdbc_segment *seg,
		struct xdbc_ring *ring,
		enum xdbc_page_type type)
{
	struct xdbc_trb *link_trb;

	seg->trbs = xdbc_get_page(&seg->dma, type);
	if (!seg->trbs)
		return -ENOMEM;

	ring->segment = seg;
	ring->enqueue = seg->trbs;
	ring->dequeue = seg->trbs;
	ring->cycle_state = 1;

	if (type == XDBC_PAGE_TXIN || type == XDBC_PAGE_TXOUT) {
		link_trb = &seg->trbs[XDBC_TRBS_PER_SEGMENT - 1];
		link_trb->field[0] = cpu_to_le32(lower_32_bits(seg->dma));
		link_trb->field[1] = cpu_to_le32(upper_32_bits(seg->dma));
		link_trb->field[3] = cpu_to_le32(TRB_TYPE(TRB_LINK)) |
				cpu_to_le32(LINK_TOGGLE);
	}

	return 0;
}

static inline void xdbc_put_utf16(u16 *s, const char *c, size_t size)
{
	int i;

	for (i = 0; i < size; i++)
		s[i] = cpu_to_le16(c[i]);
}

static int xdbc_mem_init(void)
{
	struct xdbc_erst_entry *entry;
	struct xdbc_strings *strings;
	struct xdbc_context *context;
	struct xdbc_ep_context *ep_in, *ep_out;
	struct usb_string_descriptor *s_desc;
	unsigned int max_burst;
	u32 string_length;
	int ret, index = 0;
	u32 dev_info;

	/* allocate table page */
	xdbcp->table_base = xdbc_get_page(&xdbcp->table_dma,
			XDBC_PAGE_TABLE);
	if (!xdbcp->table_base) {
		xdbc_trace("falied to alloc table page\n");
		return -ENOMEM;
	}

	/* allocate and initialize event ring */
	ret = xdbc_alloc_ring(&xdbcp->evt_seg, &xdbcp->evt_ring,
			XDBC_PAGE_EVENT);
	if (ret < 0) {
		xdbc_trace("failed to alloc event ring\n");
		return ret;
	}

	/* allocate event ring segment table */
	xdbcp->erst_size = 16;
	xdbcp->erst_base = xdbcp->table_base +
			index * XDBC_TABLE_ENTRY_SIZE;
	xdbcp->erst_dma = xdbcp->table_dma +
			index * XDBC_TABLE_ENTRY_SIZE;
	index += XDBC_ERST_ENTRY_NUM;

	/* Initialize Event Ring Segment Table */
	entry = (struct xdbc_erst_entry *)xdbcp->erst_base;
	entry->seg_addr = cpu_to_le64(xdbcp->evt_seg.dma);
	entry->seg_size = cpu_to_le32(XDBC_TRBS_PER_SEGMENT);
	entry->rsvd = 0;

	/* Initialize ERST registers */
	writel(1, &xdbcp->xdbc_reg->ersts);
	xdbc_write64(xdbcp->erst_dma, &xdbcp->xdbc_reg->erstba);
	xdbc_write64(xdbcp->evt_seg.dma, &xdbcp->xdbc_reg->erdp);

	/* debug capability contexts */
	BUILD_BUG_ON(sizeof(struct xdbc_info_context) != 64);
	BUILD_BUG_ON(sizeof(struct xdbc_ep_context) != 64);
	BUILD_BUG_ON(sizeof(struct xdbc_context) != 64 * 3);

	xdbcp->dbcc_size = 64 * 3;
	xdbcp->dbcc_base = xdbcp->table_base +
			index * XDBC_TABLE_ENTRY_SIZE;
	xdbcp->dbcc_dma = xdbcp->table_dma +
			index * XDBC_TABLE_ENTRY_SIZE;
	index += XDBC_DBCC_ENTRY_NUM;

	/* IN/OUT endpoint transfer ring */
	ret = xdbc_alloc_ring(&xdbcp->in_seg, &xdbcp->in_ring,
			XDBC_PAGE_TXIN);
	if (ret < 0) {
		xdbc_trace("failed to alloc IN transfer ring\n");
		return ret;
	}

	ret = xdbc_alloc_ring(&xdbcp->out_seg, &xdbcp->out_ring,
			XDBC_PAGE_TXOUT);
	if (ret < 0) {
		xdbc_trace("failed to alloc OUT transfer ring\n");
		return ret;
	}

	/* strings */
	xdbcp->string_size = sizeof(struct xdbc_strings);
	xdbcp->string_base = xdbcp->table_base +
			index * XDBC_TABLE_ENTRY_SIZE;
	xdbcp->string_dma = xdbcp->table_dma +
			index * XDBC_TABLE_ENTRY_SIZE;
	index += XDBC_STRING_ENTRY_NUM;

	strings = (struct xdbc_strings *)xdbcp->string_base;

	/* serial string */
	s_desc = (struct usb_string_descriptor *)strings->serial;
	s_desc->bLength = (strlen(XDBC_STRING_SERIAL) + 1) * 2;
	s_desc->bDescriptorType = USB_DT_STRING;
	xdbc_put_utf16(s_desc->wData, XDBC_STRING_SERIAL,
			strlen(XDBC_STRING_SERIAL));

	string_length = s_desc->bLength;
	string_length <<= 8;

	/* product string */
	s_desc = (struct usb_string_descriptor *)strings->product;
	s_desc->bLength = (strlen(XDBC_STRING_PRODUCT) + 1) * 2;
	s_desc->bDescriptorType = USB_DT_STRING;
	xdbc_put_utf16(s_desc->wData, XDBC_STRING_PRODUCT,
			strlen(XDBC_STRING_PRODUCT));

	string_length += s_desc->bLength;
	string_length <<= 8;

	/* manufacture string */
	s_desc = (struct usb_string_descriptor *)strings->manufacture;
	s_desc->bLength = (strlen(XDBC_STRING_MANUFACTURE) + 1) * 2;
	s_desc->bDescriptorType = USB_DT_STRING;
	xdbc_put_utf16(s_desc->wData, XDBC_STRING_MANUFACTURE,
			strlen(XDBC_STRING_MANUFACTURE));

	string_length += s_desc->bLength;
	string_length <<= 8;

	/* string 0 */
	strings->string0[0] = 4;
	strings->string0[1] = USB_DT_STRING;
	strings->string0[2] = 0x09;
	strings->string0[3] = 0x04;

	string_length += 4;

	/* populate the contexts */
	context = (struct xdbc_context *)xdbcp->dbcc_base;
	context->info.string0 = cpu_to_le64(xdbcp->string_dma);
	context->info.manufacture = cpu_to_le64(xdbcp->string_dma +
			XDBC_MAX_STRING_LENGTH);
	context->info.product = cpu_to_le64(xdbcp->string_dma +
			XDBC_MAX_STRING_LENGTH * 2);
	context->info.serial = cpu_to_le64(xdbcp->string_dma +
			XDBC_MAX_STRING_LENGTH * 3);
	context->info.length = cpu_to_le32(string_length);

	max_burst = DEBUG_MAX_BURST(readl(&xdbcp->xdbc_reg->control));
	ep_out = (struct xdbc_ep_context *)&context->out;
	ep_out->ep_info1 = 0;
	ep_out->ep_info2 = cpu_to_le32(EP_TYPE(BULK_OUT_EP) |
			MAX_PACKET(1024) | MAX_BURST(max_burst));
	ep_out->deq = cpu_to_le64(xdbcp->out_seg.dma |
			xdbcp->out_ring.cycle_state);

	ep_in = (struct xdbc_ep_context *)&context->in;
	ep_in->ep_info1 = 0;
	ep_in->ep_info2 = cpu_to_le32(EP_TYPE(BULK_OUT_EP) |
			MAX_PACKET(1024) | MAX_BURST(max_burst));
	ep_in->deq = cpu_to_le64(xdbcp->in_seg.dma |
			xdbcp->in_ring.cycle_state);

	/* write DbC context pointer register */
	xdbc_write64(xdbcp->dbcc_dma, &xdbcp->xdbc_reg->dccp);

	/* device descriptor info registers */
	dev_info = cpu_to_le32((XDBC_VENDOR_ID << 16) | XDBC_PROTOCOL);
	writel(dev_info, &xdbcp->xdbc_reg->devinfo1);
	dev_info = cpu_to_le32((XDBC_DEVICE_REV << 16) | XDBC_PRODUCT_ID);
	writel(dev_info, &xdbcp->xdbc_reg->devinfo2);

	/* get and store the transfer buffer */
	xdbcp->out_buf = xdbc_get_page(&xdbcp->out_dma,
			XDBC_PAGE_BUFFER);
	xdbcp->in_buf = xdbcp->out_buf + XDBC_MAX_PACKET;
	xdbcp->in_dma = xdbcp->out_dma + XDBC_MAX_PACKET;

	return 0;
}

static void xdbc_reset_debug_port_callback(int cap_offset, void *data)
{
	u8 major;
	u32 val, port_offset, port_count;
	u32 cap_length;
	void __iomem *ops_reg;
	void __iomem *portsc;
	int i;

	val = readl(xdbcp->xhci_base + cap_offset);
	major = (u8) XHCI_EXT_PORT_MAJOR(val);

	/* only reset super-speed port */
	if (major != 0x3)
		return;

	val = readl(xdbcp->xhci_base + cap_offset + 8);
	port_offset = XHCI_EXT_PORT_OFF(val);
	port_count = XHCI_EXT_PORT_COUNT(val);
	xdbc_trace("Extcap Port offset %d count %d\n",
			port_offset, port_count);

	cap_length = readl(xdbcp->xhci_base) & 0xff;
	ops_reg = xdbcp->xhci_base + cap_length;

	port_offset--;
	for (i = port_offset; i < (port_offset + port_count); i++) {
		portsc = ops_reg + 0x400 + i * 0x10;
		val = readl(portsc);
		/* reset the port if CCS bit is cleared */
		if (!(val & 0x1))
			writel(val | (1 << 4), portsc);
	}
}

static void xdbc_reset_debug_port(void)
{
	xdbc_walk_excap(xdbcp->bus,
			xdbcp->dev,
			xdbcp->func,
			XHCI_EXT_CAPS_PROTOCOL,
			false,
			xdbc_reset_debug_port_callback,
			NULL);
}

/*
 * xdbc_start: start DbC
 *
 * Set DbC enable bit and wait until DbC run bit being set or timed out.
 */
static int xdbc_start(void)
{
	u32 ctrl, status;

	ctrl = readl(&xdbcp->xdbc_reg->control);
	writel(ctrl | CTRL_DCE | CTRL_LSE, &xdbcp->xdbc_reg->control);

	if (handshake(&xdbcp->xdbc_reg->control, CTRL_DCE,
			CTRL_DCE, 100000, 100) < 0) {
		xdbc_trace("falied to initialize hardware\n");
		return -ENODEV;
	}

	/* reset port to avoid bus hang */
	if (xdbcp->vendor == PCI_VENDOR_ID_INTEL)
		xdbc_reset_debug_port();

	/* wait for port connection */
	if (handshake(&xdbcp->xdbc_reg->portsc, PORTSC_CCS,
			PORTSC_CCS, 5000000, 100) < 0) {
		xdbc_trace("waiting for connection timed out\n");
		return -ETIMEDOUT;
	}
	xdbc_trace("port connection detected\n");

	/* wait for debug device to be configured */
	if (handshake(&xdbcp->xdbc_reg->control, CTRL_DCR,
			CTRL_DCR, 5000000, 100) < 0) {
		xdbc_trace("waiting for device configuration timed out\n");
		return -ETIMEDOUT;
	}

	/* port should have a valid port# */
	status = readl(&xdbcp->xdbc_reg->status);
	if (!DCST_DPN(status)) {
		xdbc_trace("invalid root hub port number\n");
		return -ENODEV;
	}

	xdbc_trace("root hub port number %d\n", DCST_DPN(status));

	xdbcp->in_ep_state = EP_RUNNING;
	xdbcp->out_ep_state = EP_RUNNING;

	xdbc_trace("DbC is running now, control 0x%08x\n",
			readl(&xdbcp->xdbc_reg->control));

	return 0;
}

static int xdbc_setup(void)
{
	int ret;

	writel(0, &xdbcp->xdbc_reg->control);
	if (handshake(&xdbcp->xdbc_reg->control, CTRL_DCE,
			0, 100000, 100) < 0) {
		xdbc_trace("falied to initialize hardware\n");
		return -ETIMEDOUT;
	}

	/* allocate and initialize all memory data structures */
	ret = xdbc_mem_init();
	if (ret < 0) {
		xdbc_trace("failed to initialize memory\n");
		return ret;
	}

	/*
	 * Memory barrier to ensure hardware sees the bits
	 * setting above.
	 */
	mmiowb();

	/* dump registers and data structures */
	xdbc_dbg_dump_regs("hardware setup completed");
	xdbc_dbg_dump_data("hardware setup completed");

	ret = xdbc_start();
	if (ret < 0) {
		xdbc_trace("failed to start DbC, cable connected?\n");
		return ret;
	}

	return 0;
}

int __init early_xdbc_init(char *s)
{
	u32 bus = 0, dev = 0, func = 0;
	unsigned long dbgp_num = 0;
	u32 offset;
	int ret;

	if (!early_pci_allowed())
		return -EPERM;

	/* FIXME: early printk "keep" option will be supported later */
	if (strstr(s, "keep"))
		return -EPERM;

	if (xdbcp->xdbc_reg)
		return 0;

	if (*s && kstrtoul(s, 0, &dbgp_num))
		dbgp_num = 0;

	xdbc_trace("dbgp_num: %lu\n", dbgp_num);

	offset = xdbc_find_dbgp(dbgp_num, &bus, &dev, &func);
	if (!offset)
		return -ENODEV;

	xdbc_trace("Found xHCI debug capability on %02x:%02x.%1x\n",
			bus, dev, func);

	if (!xdbcp->xhci_base)
		return -EINVAL;

	xdbcp->xdbc_reg = (struct xdbc_regs __iomem *)
			(xdbcp->xhci_base + offset);
	xdbc_dbg_dump_regs("debug capability located");

	/* hand over the owner of host from BIOS */
	xdbc_bios_handoff();

	ret = xdbc_setup();
	if (ret < 0) {
		pr_notice("failed to setup xHCI DbC connection\n");
		xdbcp->xhci_base = NULL;
		xdbcp->xdbc_reg = NULL;
		xdbc_dump_debug_buffer();
		return ret;
	}

	return 0;
}

static void xdbc_queue_trb(struct xdbc_ring *ring,
		u32 field1, u32 field2, u32 field3, u32 field4)
{
	struct xdbc_trb *trb, *link_trb;

	trb = ring->enqueue;
	trb->field[0] = cpu_to_le32(field1);
	trb->field[1] = cpu_to_le32(field2);
	trb->field[2] = cpu_to_le32(field3);
	trb->field[3] = cpu_to_le32(field4);

	xdbc_dbg_dump_trb(trb, "enqueue trb");

	++(ring->enqueue);
	if (ring->enqueue >= &ring->segment->trbs[TRBS_PER_SEGMENT - 1]) {
		link_trb = ring->enqueue;
		if (ring->cycle_state)
			link_trb->field[3] |= cpu_to_le32(TRB_CYCLE);
		else
			link_trb->field[3] &= cpu_to_le32(~TRB_CYCLE);

		ring->enqueue = ring->segment->trbs;
		ring->cycle_state ^= 1;
	}
}

static void xdbc_ring_doorbell(int target)
{
	writel(DOOR_BELL_TARGET(target), &xdbcp->xdbc_reg->doorbell);
}

static void xdbc_handle_port_status(struct xdbc_trb *evt_trb)
{
	u32 port_reg;

	port_reg = readl(&xdbcp->xdbc_reg->portsc);

	if (port_reg & PORTSC_CSC) {
		xdbc_trace("%s: connect status change event\n", __func__);
		writel(port_reg | PORTSC_CSC, &xdbcp->xdbc_reg->portsc);
		port_reg = readl(&xdbcp->xdbc_reg->portsc);
	}

	if (port_reg & PORTSC_PRC) {
		xdbc_trace("%s: port reset change event\n", __func__);
		writel(port_reg | PORTSC_PRC, &xdbcp->xdbc_reg->portsc);
		port_reg = readl(&xdbcp->xdbc_reg->portsc);
	}

	if (port_reg & PORTSC_PLC) {
		xdbc_trace("%s: port link status change event\n", __func__);
		writel(port_reg | PORTSC_PLC, &xdbcp->xdbc_reg->portsc);
		port_reg = readl(&xdbcp->xdbc_reg->portsc);
	}

	if (port_reg & PORTSC_CEC) {
		xdbc_trace("%s: config error change\n", __func__);
		writel(port_reg | PORTSC_CEC, &xdbcp->xdbc_reg->portsc);
		port_reg = readl(&xdbcp->xdbc_reg->portsc);
	}
}

static void xdbc_handle_tx_event(struct xdbc_trb *evt_trb)
{
	u32 comp_code;
	u32 tx_dma_high, tx_dma_low;
	u64 in_dma, out_dma;
	size_t remain_length;
	int ep_id;

	tx_dma_low = le32_to_cpu(evt_trb->field[0]);
	tx_dma_high = le32_to_cpu(evt_trb->field[1]);
	comp_code = GET_COMP_CODE(le32_to_cpu(evt_trb->field[2]));
	remain_length = EVENT_TRB_LEN(le32_to_cpu(evt_trb->field[2]));
	ep_id = TRB_TO_EP_ID(le32_to_cpu(evt_trb->field[3]));
	in_dma = __pa(xdbcp->in_pending);
	out_dma = __pa(xdbcp->out_pending);

	/*
	 * Possible Completion Codes for DbC Transfer Event are Success,
	 * Stall Error, USB Transaction Error, Babble Detected Error,
	 * TRB Error, Short Packet, Undefined Error, Event Ring Full Error,
	 * and Vendor Defined Error. TRB error, undefined error and vendor
	 * defined error will result in HOT/HIT set and be handled the same
	 * way as Stall error.
	 */
	switch (comp_code) {
	case COMP_SUCCESS:
		remain_length = 0;
	case COMP_SHORT_TX:
		xdbc_trace("%s: endpoint %d remains %d bytes\n", __func__,
			ep_id, remain_length);
		break;
	case COMP_TRB_ERR:
	case COMP_BABBLE:
	case COMP_TX_ERR:
	case COMP_STALL:
	default:
		xdbc_trace("%s: endpoint %d halted\n", __func__, ep_id);
		if (ep_id == XDBC_EPID_OUT)
			xdbcp->out_ep_state = EP_HALTED;
		if (ep_id == XDBC_EPID_IN)
			xdbcp->in_ep_state = EP_HALTED;

		break;
	}

	if (lower_32_bits(in_dma) == tx_dma_low &&
			upper_32_bits(in_dma) == tx_dma_high) {
		xdbcp->in_complete = comp_code;
		xdbcp->in_complete_length =
				(remain_length > xdbcp->in_length) ?
				0 : xdbcp->in_length - remain_length;
	}

	if (lower_32_bits(out_dma) == tx_dma_low &&
			upper_32_bits(out_dma) == tx_dma_high) {
		xdbcp->out_complete = comp_code;
		xdbcp->out_complete_length =
				(remain_length > xdbcp->out_length) ?
				0 : xdbcp->out_length - remain_length;
	}
}

static void xdbc_handle_events(void)
{
	struct xdbc_trb *evt_trb;
	bool update_erdp = false;

	evt_trb = xdbcp->evt_ring.dequeue;
	while ((le32_to_cpu(evt_trb->field[3]) & TRB_CYCLE) ==
			xdbcp->evt_ring.cycle_state) {
		/*
		 * Memory barrier to ensure software sees the trbs
		 * enqueued by hardware.
		 */
		rmb();

		xdbc_dbg_dump_trb(evt_trb, "event trb");

		/* FIXME: Handle more event types. */
		switch ((le32_to_cpu(evt_trb->field[3]) & TRB_TYPE_BITMASK)) {
		case TRB_TYPE(TRB_PORT_STATUS):
			xdbc_handle_port_status(evt_trb);
			break;
		case TRB_TYPE(TRB_TRANSFER):
			xdbc_handle_tx_event(evt_trb);
			break;
		default:
			break;
		}

		/* advance to the next trb */
		++(xdbcp->evt_ring.dequeue);
		if (xdbcp->evt_ring.dequeue ==
				&xdbcp->evt_seg.trbs[TRBS_PER_SEGMENT]) {
			xdbcp->evt_ring.dequeue = xdbcp->evt_seg.trbs;
			xdbcp->evt_ring.cycle_state ^= 1;
		}

		evt_trb = xdbcp->evt_ring.dequeue;
		update_erdp = true;
	}

	/* update event ring dequeue pointer */
	if (update_erdp)
		xdbc_write64(__pa(xdbcp->evt_ring.dequeue),
				&xdbcp->xdbc_reg->erdp);
}

/*
 * Check and dispatch events in event ring. It also checks status
 * of hardware. This function will be called from multiple threads.
 * An atomic lock is applied to protect the access of event ring.
 */
static int xdbc_check_event(void)
{
	/* event ring is under checking by other thread? */
	if (!test_bit(XDBC_ATOMIC_EVENT, &xdbcp->atomic_flags) &&
			!test_and_set_bit(XDBC_ATOMIC_EVENT,
			&xdbcp->atomic_flags))
		return 0;

	xdbc_handle_events();

	test_and_clear_bit(XDBC_ATOMIC_EVENT, &xdbcp->atomic_flags);

	return 0;
}

#define	BULK_IN_COMPLETED(p)	((xdbcp->in_pending == (p)) && \
				 xdbcp->in_complete)
#define	BULK_OUT_COMPLETED(p)	((xdbcp->out_pending == (p)) && \
				 xdbcp->out_complete)

/*
 * Wait for a bulk-in or bulk-out transfer completion or timed out.
 * Return count of the actually transferred bytes or error.
 */
static int xdbc_wait_until_bulk_done(struct xdbc_trb *trb, int loops)
{
	int timeout = 0;
	bool read;

	if (trb != xdbcp->in_pending &&
			trb != xdbcp->out_pending)
		return -EINVAL;

	read = (trb == xdbcp->in_pending);

	do {
		if (xdbc_check_event() < 0)
			break;

		if (read && BULK_IN_COMPLETED(trb)) {
			if (xdbcp->in_ep_state == EP_HALTED)
				return -EAGAIN;
			else
				return xdbcp->in_complete_length;
		}

		if (!read && BULK_OUT_COMPLETED(trb)) {
			if (xdbcp->out_ep_state == EP_HALTED)
				return -EAGAIN;
			else
				return xdbcp->out_complete_length;
		}

		xdbc_udelay(10);
	} while ((timeout++ < loops) || !loops);

	return -EIO;
}

static int xdbc_wait_until_dbc_configured(void)
{
	int timeout = 0;
	u32 reg;

	/* Port exits configured state */
	reg = readl(&xdbcp->xdbc_reg->control);
	if (!(reg & CTRL_DRC))
		return 0;

	/* clear run change bit (RW1C) */
	writel(reg | CTRL_DRC, &xdbcp->xdbc_reg->control);

	do {
		if (readl(&xdbcp->xdbc_reg->control) & CTRL_DCR)
			return 0;

		xdbc_udelay(10);
	} while (timeout++ < XDBC_LOOPS);

	return -ETIMEDOUT;
}

static int xdbc_wait_until_epstall_cleared(bool read)
{
	int timeout = 0;

	if (read) {
		do {
			if (!(readl(&xdbcp->xdbc_reg->control) & CTRL_HIT)) {
				xdbcp->in_ep_state = EP_RUNNING;

				return 0;
			}

			xdbcp->in_ep_state = EP_HALTED;
			xdbc_udelay(10);
		} while (timeout++ < XDBC_LOOPS);
	} else {
		do {
			if (!(readl(&xdbcp->xdbc_reg->control) & CTRL_HOT)) {
				xdbcp->out_ep_state = EP_RUNNING;

				return 0;
			}

			xdbcp->out_ep_state = EP_HALTED;
			xdbc_udelay(10);
		} while (timeout++ < XDBC_LOOPS);
	}

	return -ETIMEDOUT;
}

static int xdbc_bulk_transfer(void *data, int size, int loops, bool read)
{
	u64 addr;
	u32 length, control;
	struct xdbc_trb *trb;
	struct xdbc_ring *ring;
	u32 cycle;
	int ret;

	if (size > XDBC_MAX_PACKET) {
		xdbc_trace("%s: bad parameter, size %d", __func__, size);
		return -EINVAL;
	}

	if (xdbc_wait_until_dbc_configured()) {
		xdbc_trace("%s: hardware not ready\n", __func__);
		return -EPERM;
	}

	if (xdbc_wait_until_epstall_cleared(read)) {
		xdbc_trace("%s: endpoint not ready\n", __func__);
		return -EPERM;
	}

	ring = (read ? &xdbcp->in_ring : &xdbcp->out_ring);
	trb = ring->enqueue;
	cycle = ring->cycle_state;

	length = TRB_LEN(size);
	control = TRB_TYPE(TRB_NORMAL) | TRB_IOC;

	if (cycle)
		control &= cpu_to_le32(~TRB_CYCLE);
	else
		control |= cpu_to_le32(TRB_CYCLE);

	if (read) {
		memset(xdbcp->in_buf, 0, XDBC_MAX_PACKET);
		addr = xdbcp->in_dma;

		xdbcp->in_pending = trb;
		xdbcp->in_length = size;
		xdbcp->in_complete = 0;
		xdbcp->in_complete_length = 0;
	} else {
		memcpy(xdbcp->out_buf, data, size);
		addr = xdbcp->out_dma;

		xdbcp->out_pending = trb;
		xdbcp->out_length = size;
		xdbcp->out_complete = 0;
		xdbcp->out_complete_length = 0;
	}

	xdbc_queue_trb(ring, lower_32_bits(addr),
			upper_32_bits(addr),
			length, control);

	/*
	 * Memory barrier to ensure hardware sees the trbs
	 * enqueued above.
	 */
	wmb();
	if (cycle)
		trb->field[3] |= cpu_to_le32(cycle);
	else
		trb->field[3] &= cpu_to_le32(~TRB_CYCLE);

	xdbc_ring_doorbell(read ? IN_EP_DOORBELL : OUT_EP_DOORBELL);

	ret = xdbc_wait_until_bulk_done(trb, loops);

	if (read)
		xdbcp->in_pending = NULL;
	else
		xdbcp->out_pending = NULL;

	if (ret > 0) {
		if (read)
			memcpy(data, xdbcp->in_buf, size);
		else
			memset(xdbcp->out_buf, 0, XDBC_MAX_PACKET);
	} else {
		xdbc_trace("%s: bulk %s transfer results in error %d\n",
				__func__, read ? "in" : "out", ret);
	}

	return ret;
}

int xdbc_bulk_read(void *data, int size, int loops)
{
	int ret;

	do {
		if (!test_bit(XDBC_ATOMIC_BULKIN, &xdbcp->atomic_flags) &&
				!test_and_set_bit(XDBC_ATOMIC_BULKIN,
				&xdbcp->atomic_flags))
			break;
	} while (1);

	ret = xdbc_bulk_transfer(data, size, loops, true);

	test_and_clear_bit(XDBC_ATOMIC_BULKIN, &xdbcp->atomic_flags);

	return ret;
}

int xdbc_bulk_write(const char *bytes, int size)
{
	int ret;

	do {
		if (!test_bit(XDBC_ATOMIC_BULKOUT, &xdbcp->atomic_flags) &&
				!test_and_set_bit(XDBC_ATOMIC_BULKOUT,
				&xdbcp->atomic_flags))
			break;
	} while (1);

	ret = xdbc_bulk_transfer((void *)bytes, size, XDBC_LOOPS, false);

	test_and_clear_bit(XDBC_ATOMIC_BULKOUT, &xdbcp->atomic_flags);

	return ret;
}

/*
 * Start a bulk-in or bulk-out transfer, wait until transfer completion
 * or error. Return the count of actually transferred bytes or error.
 */
static void early_xdbc_write(struct console *con, const char *str, u32 n)
{
	int chunk, ret;
	static char buf[XDBC_MAX_PACKET];
	int use_cr = 0;

	if (!xdbcp->xdbc_reg)
		return;
	memset(buf, 0, XDBC_MAX_PACKET);
	while (n > 0) {
		for (chunk = 0; chunk < XDBC_MAX_PACKET && n > 0;
		     str++, chunk++, n--) {
			if (!use_cr && *str == '\n') {
				use_cr = 1;
				buf[chunk] = '\r';
				str--;
				n++;
				continue;
			}
			if (use_cr)
				use_cr = 0;
			buf[chunk] = *str;
		}
		if (chunk > 0) {
			ret = xdbc_bulk_write(buf, chunk);
			if (ret < 0)
				break;
		}
	}
}

struct console early_xdbc_console = {
	.name =		"earlyxdbc",
	.write =	early_xdbc_write,
	.flags =	CON_PRINTBUFFER,
	.index =	-1,
};
