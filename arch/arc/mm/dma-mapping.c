// SPDX-License-Identifier: GPL-2.0
// (C) 2018 Synopsys, Inc. (www.synopsys.com)

#include <linux/dma-mapping.h>

void arch_setup_dma_ops(struct device *dev, u64 dma_base, u64 size,
			const struct iommu_ops *iommu, bool coherent)
{
	const struct dma_map_ops *dma_ops = &dma_noncoherent_ops;

	/*
	 * IOC hardware snoops all DMA traffic keeping the caches consistent
	 * with memory - eliding need for any explicit cache maintenance of
	 * DMA buffers - so we can use dma_direct cache ops.
	 */
	if (is_isa_arcv2() && ioc_enable && coherent)
		dma_ops = &dma_direct_ops;

	set_dma_ops(dev, dma_ops);
}
