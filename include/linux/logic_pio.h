/*
 * Copyright (C) 2017 Hisilicon Limited, All Rights Reserved.
 * Author: Gabriele Paoloni <gabriele.paoloni@huawei.com>
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

#ifndef __LINUX_LOGIC_PIO_H
#define __LINUX_LOGIC_PIO_H

#ifdef __KERNEL__

#include <linux/fwnode.h>

#define PIO_INDIRECT		0x01UL /* indirect IO flag */
#define PIO_CPU_MMIO		0x00UL /* memory mapped io flag */

struct logic_pio_hwaddr {
	struct list_head list;
	struct fwnode_handle *fwnode;
	resource_size_t hw_start;
	resource_size_t io_start;
	resource_size_t size; /* range size populated */
	unsigned long flags;

	void *devpara;	/* private parameter of the host device */
	struct hostio_ops *ops;	/* ops operating on this node */
};

struct hostio_ops {
	u32 (*pfin)(void *devobj, unsigned long ptaddr,	size_t dlen);
	void (*pfout)(void *devobj, unsigned long ptaddr, u32 outval,
			size_t dlen);
	u32 (*pfins)(void *devobj, unsigned long ptaddr, void *inbuf,
			size_t dlen, unsigned int count);
	void (*pfouts)(void *devobj, unsigned long ptaddr,
			const void *outbuf, size_t dlen, unsigned int count);
};

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

#ifdef CONFIG_INDIRECT_PIO
/* Below make 75% of IO Space for MMIO and the rest for Indirect IO */
#define MMIO_UPPER_LIMIT (IO_SPACE_LIMIT - (IO_SPACE_LIMIT >> 2))
#else
#define MMIO_UPPER_LIMIT IO_SPACE_LIMIT
#endif

#ifdef CONFIG_LOGIC_PIO
extern struct logic_pio_hwaddr
*find_io_range_by_fwnode(struct fwnode_handle *fwnode);

extern unsigned long logic_pio_trans_hwaddr(struct fwnode_handle *fwnode,
			resource_size_t hw_addr);

extern int logic_pio_register_range(struct logic_pio_hwaddr *newrange);
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

static inline struct logic_pio_hwaddr
*logic_pio_register_range(struct logic_pio_hwaddr *newrange);
{
	return NULL;
}
#endif

extern resource_size_t logic_pio_to_hwaddr(unsigned long pio);

extern unsigned long logic_pio_trans_cpuaddr(resource_size_t hw_addr);

#endif /* __KERNEL__ */
#endif /* __LINUX_LOGIC_PIO_H */
