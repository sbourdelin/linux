/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LINUX_GOLDFISH_H
#define __LINUX_GOLDFISH_H

/* Helpers for Goldfish virtual platform */

static inline void gf_write_addr(unsigned long addr, void __iomem *portl,
				 void __iomem *porth)
{
	writel((u32)addr, portl);
#ifdef CONFIG_64BIT
	writel((u32)(addr >> 32), porth);
#endif
}

static inline void gf_write_ptr(const void *ptr, void __iomem *portl,
				void __iomem *porth)
{
	gf_write_addr((unsigned long)ptr, portl, porth);
}

static inline void gf_write_u64(u64 value, void __iomem *portl,
				void __iomem *porth)
{
	writel((u32)value, portl);
	writel((u32)(value >> 32), porth);
}

static inline u64 gf_read_u64(void __iomem *portl, void __iomem *porth)
{
	u64 lo = readl(portl);
	u64 hi = readl(porth);

	return lo | (hi << 32);
}

static inline void gf_write_dma_addr(const dma_addr_t addr, void __iomem *portl,
				     void __iomem *porth)
{
	writel((u32)addr, portl);
#ifdef CONFIG_ARCH_DMA_ADDR_T_64BIT
	writel((u32)(addr >> 32), porth);
#endif
}

#endif /* __LINUX_GOLDFISH_H */
