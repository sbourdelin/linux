/* SPDX-License-Identifier: GPL-2.0 */
/*
 *  Copyright (C) 2014 Lei Chuanhua <Chuanhua.lei@lantiq.com>
 *  Copyright (C) 2018 Intel Corporation.
 */
#ifndef __ASM_MACH_INTEL_MIPS_IOREMAP_H
#define __ASM_MACH_INTEL_MIPS_IOREMAP_H

#include <linux/types.h>

static inline phys_addr_t fixup_bigphys_addr(phys_addr_t phys_addr,
					     phys_addr_t size)
{
	return phys_addr;
}

/*
 * TOP IO Space definition for SSX7 components /PCIe/ToE/Memcpy
 * physical 0xa0000000 --> virtual 0xe0000000
 */
#define GRX500_TOP_IOREMAP_BASE			0xA0000000
#define GRX500_TOP_IOREMAP_SIZE			0x20000000
#define GRX500_TOP_IOREMAP_PHYS_VIRT_OFFSET	0x40000000

static inline void __iomem *plat_ioremap(phys_addr_t offset, unsigned long size,
					 unsigned long flags)
{
	if (offset >= GRX500_TOP_IOREMAP_BASE &&
	    offset < (GRX500_TOP_IOREMAP_BASE + GRX500_TOP_IOREMAP_SIZE))
		return (void __iomem *)(unsigned long)
			(offset + GRX500_TOP_IOREMAP_PHYS_VIRT_OFFSET);
	return NULL;
}

static inline int plat_iounmap(const volatile void __iomem *addr)
{
	return (unsigned long)addr >= (unsigned long)GRX500_TOP_IOREMAP_BASE;
}
#endif /* __ASM_MACH_INTEL_MIPS_IOREMAP_H */
