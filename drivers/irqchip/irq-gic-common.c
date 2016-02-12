/*
 * Copyright (C) 2002 ARM Limited, All Rights Reserved.
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

#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/irqchip/arm-gic.h>
#include <linux/iommu.h>
#include <linux/msi.h>

#include "irq-gic-common.h"

void gic_enable_quirks(u32 iidr, const struct gic_quirk *quirks,
		void *data)
{
	for (; quirks->desc; quirks++) {
		if (quirks->iidr != (quirks->mask & iidr))
			continue;
		quirks->init(data);
		pr_info("GIC: enabling workaround for %s\n", quirks->desc);
	}
}

int gic_configure_irq(unsigned int irq, unsigned int type,
		       void __iomem *base, void (*sync_access)(void))
{
	u32 confmask = 0x2 << ((irq % 16) * 2);
	u32 confoff = (irq / 16) * 4;
	u32 val, oldval;
	int ret = 0;

	/*
	 * Read current configuration register, and insert the config
	 * for "irq", depending on "type".
	 */
	val = oldval = readl_relaxed(base + GIC_DIST_CONFIG + confoff);
	if (type & IRQ_TYPE_LEVEL_MASK)
		val &= ~confmask;
	else if (type & IRQ_TYPE_EDGE_BOTH)
		val |= confmask;

	/*
	 * Write back the new configuration, and possibly re-enable
	 * the interrupt. If we tried to write a new configuration and failed,
	 * return an error.
	 */
	writel_relaxed(val, base + GIC_DIST_CONFIG + confoff);
	if (readl_relaxed(base + GIC_DIST_CONFIG + confoff) != val && val != oldval)
		ret = -EINVAL;

	if (sync_access)
		sync_access();

	return ret;
}

void __init gic_dist_config(void __iomem *base, int gic_irqs,
			    void (*sync_access)(void))
{
	unsigned int i;

	/*
	 * Set all global interrupts to be level triggered, active low.
	 */
	for (i = 32; i < gic_irqs; i += 16)
		writel_relaxed(GICD_INT_ACTLOW_LVLTRIG,
					base + GIC_DIST_CONFIG + i / 4);

	/*
	 * Set priority on all global interrupts.
	 */
	for (i = 32; i < gic_irqs; i += 4)
		writel_relaxed(GICD_INT_DEF_PRI_X4, base + GIC_DIST_PRI + i);

	/*
	 * Deactivate and disable all SPIs. Leave the PPI and SGIs
	 * alone as they are in the redistributor registers on GICv3.
	 */
	for (i = 32; i < gic_irqs; i += 32) {
		writel_relaxed(GICD_INT_EN_CLR_X32,
			       base + GIC_DIST_ACTIVE_CLEAR + i / 8);
		writel_relaxed(GICD_INT_EN_CLR_X32,
			       base + GIC_DIST_ENABLE_CLEAR + i / 8);
	}

	if (sync_access)
		sync_access();
}

void gic_cpu_config(void __iomem *base, void (*sync_access)(void))
{
	int i;

	/*
	 * Deal with the banked PPI and SGI interrupts - disable all
	 * PPI interrupts, ensure all SGI interrupts are enabled.
	 * Make sure everything is deactivated.
	 */
	writel_relaxed(GICD_INT_EN_CLR_X32, base + GIC_DIST_ACTIVE_CLEAR);
	writel_relaxed(GICD_INT_EN_CLR_PPI, base + GIC_DIST_ENABLE_CLEAR);
	writel_relaxed(GICD_INT_EN_SET_SGI, base + GIC_DIST_ENABLE_SET);

	/*
	 * Set priority on PPI and SGI interrupts
	 */
	for (i = 0; i < 32; i += 4)
		writel_relaxed(GICD_INT_DEF_PRI_X4,
					base + GIC_DIST_PRI + i * 4 / 4);

	if (sync_access)
		sync_access();
}

#if defined(CONFIG_IOMMU_API) && defined(CONFIG_PCI_MSI_IRQ_DOMAIN)
static int gic_set_msi_addr(struct irq_data *data, struct msi_msg *msg)
{
	struct msi_desc *desc = irq_data_get_msi_desc(data);
	struct device *dev = msi_desc_to_dev(desc);
	struct iommu_domain *d;
	phys_addr_t addr;
	dma_addr_t iova;
	int ret;

	d = iommu_get_domain_for_dev(dev);
	if (!d)
		return 0;
#ifdef CONFIG_PHYS_ADDR_T_64BIT
	addr = ((phys_addr_t)(msg->address_hi) << 32) | msg->address_lo;
#else
	addr = msg->address_lo;
#endif

	ret = iommu_get_single_reserved(d, addr, IOMMU_WRITE, &iova);

	if (!ret) {
		msg->address_lo = lower_32_bits(iova);
		msg->address_hi = upper_32_bits(iova);
	}
	return ret;
}


static void gic_unset_msi_addr(struct irq_data *data)
{
	struct msi_desc *desc = irq_data_get_msi_desc(data);
	struct device *dev;
	struct iommu_domain *d;
	dma_addr_t iova;

#ifdef CONFIG_ARCH_DMA_ADDR_T_64BIT
	iova = ((dma_addr_t)(desc->msg.address_hi) << 32) |
		desc->msg.address_lo;
#else
	iova = desc->msg.address_lo;
#endif

	dev = msi_desc_to_dev(desc);
	if (!dev)
		return;

	d = iommu_get_domain_for_dev(dev);
	if (!d)
		return;

	iommu_put_single_reserved(d, iova);
}

void gic_pci_msi_domain_write_msg(struct irq_data *irq_data,
				  struct msi_msg *msg)
{
	if (!msg->address_hi && !msg->address_lo && !msg->data)
		gic_unset_msi_addr(irq_data); /* deactivate */
	else
		gic_set_msi_addr(irq_data, msg); /* activate, set_affinity */

	pci_msi_domain_write_msg(irq_data, msg);
}
#endif

