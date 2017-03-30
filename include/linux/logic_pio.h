/*
 * Copyright (C) 2017 Hisilicon Limited, All Rights Reserved.
 * Author: Zhichang Yuan <yuanzhichang@hisilicon.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef __LINUX_LIBIO_H
#define __LINUX_LIBIO_H

#ifdef __KERNEL__

#include <linux/fwnode.h>

/*
 *		Total IO space is 0 to IO_SPACE_LIMIT
 *
 *    section			pio
 * |________|________________________________________|
 *
 * In this division, the benefits are:
 * 1) The MMIO PIO space is consecutive, then ioport_map() still works well
 * for MMIO;
 * 2) The search happened in inX/outX with input PIO will have better
 * performance for indirect_IO. For MMIO, the performance is nearly same
 * even when CONFIG_INDIRECT_PIO is enabled;
 *
 * Some notes:
 * 1) Don't increase the IO_SPACE_LIMIT to avoid modification on so many
 * architectural files;
 * 2) To reduce the impact on the original I/O space to a minimum, we only
 * apply this IO space division when CONFIG_INDIRECT_PIO is enabled; And
 * only allocate the last section to INDIRECT_PIO, all the other PIO space are
 * for MMIO;
 * 3) For better efficiency, one more I/O segment can be separated from 'pio'
 * bit section. But it will make the IO space size decreased. Won't apply at
 * this moment;
 */
#ifdef CONFIG_INDIRECT_PIO
#define PIO_SECT_BITS		2
#else
#define PIO_SECT_BITS		0
#endif
#define PIO_MAX_SECT		(0x01UL << PIO_SECT_BITS)
#define PIO_SECT_MASK		(PIO_MAX_SECT - 1)

/* The last section. */
#define PIO_INDIRECT		(PIO_MAX_SECT - 1)
/* This one is for MMIO(PCI) to keep compatibility */
#define PIO_CPU_MMIO		0x00UL

struct logic_pio_root {
	struct list_head sec_head;
	resource_size_t sec_min;
	resource_size_t sec_max;
};

#if ((IO_SPACE_LIMIT + 1) & IO_SPACE_LIMIT)
#error "(IO_SPACE_LIMIT + 1) must be power of 2!"
#endif

#define PIO_VAL_MASK		(IO_SPACE_LIMIT >> PIO_SECT_BITS)
#define PIO_VAL_BIT_LEN		(ilog2(PIO_VAL_MASK) + 1)

#define PIO_SECT_MIN(sec_id)	((sec_id) << PIO_VAL_BIT_LEN)
#define PIO_SECT_MAX(sec_id)	(PIO_SECT_MIN(sec_id) | PIO_VAL_MASK)

#define PIO_SECT_ID(pio)	((pio >> PIO_VAL_BIT_LEN) & PIO_SECT_MASK)

struct logic_pio_sect {
	struct list_head list;
	resource_size_t io_start;

	struct logic_pio_hwaddr *hwpeer;
};
#define to_pio_sect(node) container_of(node, struct logic_pio_sect, list)

struct logic_pio_hwaddr {
	struct list_head list;
	struct fwnode_handle *fwnode;
	resource_size_t hw_start;
	resource_size_t size; /* range size populated */
	unsigned long flags;

	struct logic_pio_sect *pio_peer;

	void *devpara;	/* private parameter of the host device */
	struct hostio_ops *ops;	/* ops operating on this node */
};
#define to_pio_hwaddr(node) container_of(node, struct logic_pio_hwaddr, list)

struct hostio_ops {
	u32 (*pfin)(void *devobj, unsigned long ptaddr,	size_t dlen);
	void (*pfout)(void *devobj, unsigned long ptaddr, u32 outval,
			size_t dlen);
	u32 (*pfins)(void *devobj, unsigned long ptaddr, void *inbuf,
			size_t dlen, unsigned int count);
	void (*pfouts)(void *devobj, unsigned long ptaddr,
			const void *outbuf, size_t dlen, unsigned int count);
};

#ifdef CONFIG_INDIRECT_PIO
#define LPC_MIN_BUS_RANGE	0x0

/*
 * The default maximal IO size for Hip06/Hip07 LPC bus.
 * Defining the I/O range size as 0x400 here should be sufficient for
 * all peripherals under the bus.
 */
#define LPC_BUS_IO_SIZE		0x400
#endif

extern u8 logic_inb(unsigned long addr);
extern void logic_outb(u8 value, unsigned long addr);
extern void logic_outw(u16 value, unsigned long addr);
extern void logic_outl(u32 value, unsigned long addr);
extern u16 logic_inw(unsigned long addr);
extern u32 logic_inl(unsigned long addr);
extern void logic_outb(u8 value, unsigned long addr);
extern void logic_outw(u16 value, unsigned long addr);
extern void logic_outl(u32 value, unsigned long addr);
extern void logic_insb(unsigned long addr, void *buffer, unsigned int count);
extern void logic_insl(unsigned long addr, void *buffer, unsigned int count);
extern void logic_insw(unsigned long addr, void *buffer, unsigned int count);
extern void logic_outsb(unsigned long addr, const void *buffer,
			unsigned int count);
extern void logic_outsw(unsigned long addr, const void *buffer,
			unsigned int count);
extern void logic_outsl(unsigned long addr, const void *buffer,
			unsigned int count);
#ifdef CONFIG_LOGIC_PIO
extern struct logic_pio_hwaddr
*find_io_range_by_fwnode(struct fwnode_handle *fwnode);

extern unsigned long logic_pio_trans_hwaddr(struct fwnode_handle *fwnode,
			resource_size_t hw_addr);
#else
static inline struct logic_pio_hwaddr
*find_io_range_by_fwnode(struct fwnode_handle *fwnode)
{
	return NULL;
}

static inline unsigned long
logic_pio_trans_hwaddr(struct fwnode_handle *fwnode, resource_size_t hw_addr)
{
	return -1;
}
#endif

/*
 * These are used by pci. As LOGIC_PIO is bound with PCI, no need to add dummy
 * functions for them.
 */
extern struct logic_pio_hwaddr
*logic_pio_register_range(struct logic_pio_hwaddr *newrange,
	unsigned long align);

extern resource_size_t logic_pio_to_hwaddr(unsigned long pio);

extern unsigned long logic_pio_trans_cpuaddr(resource_size_t hw_addr);

#endif /* __KERNEL__ */
#endif /* __LINUX_LIBIO_H */
