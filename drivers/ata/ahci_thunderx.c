/*
 * SATA glue for Cavium Thunder SOCs.
 *
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 *
 * Copyright (C) 2010-2016 Cavium Networks
 *
 */

#include <linux/module.h>
#include "ahci.h"
#include "libata.h"

static irqreturn_t ahci_thunderx_irq_intr(int irq, void *dev_instance)
{
	struct ata_host *host = dev_instance;
	struct ahci_host_priv *hpriv;
	unsigned int rc = 0;
	void __iomem *mmio;
	u32 irq_stat, irq_masked;
	unsigned int handled = 1;

	VPRINTK("ENTER\n");

	hpriv = host->private_data;
	mmio = hpriv->mmio;

	/* sigh.  0xffffffff is a valid return from h/w */
	irq_stat = readl(mmio + HOST_IRQ_STAT);
	if (!irq_stat)
		return IRQ_NONE;
redo:

	irq_masked = irq_stat & hpriv->port_map;

	spin_lock(&host->lock);

	rc = ahci_handle_port_intr(host, irq_masked);

	if (!rc)
		handled = 0;

	writel(irq_stat, mmio + HOST_IRQ_STAT);

	/* Due to ERRATA#22536, ThunderX need to handle
	 * HOST_IRQ_STAT differently.
	 * Work around is to make sure all pending IRQs
	 * are served before leaving handler
	 */
	irq_stat = readl(mmio + HOST_IRQ_STAT);

	spin_unlock(&host->lock);

	if (irq_stat)
		goto redo;

	VPRINTK("EXIT\n");

	return IRQ_RETVAL(handled);
}

void ahci_thunderx_init(struct device *dev, struct ahci_host_priv *hpriv)
{
	hpriv->irq_handler = ahci_thunderx_irq_intr;
}
EXPORT_SYMBOL_GPL(ahci_thunderx_init);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Cavium, Inc. <support@cavium.com>");
MODULE_DESCRIPTION("Cavium Inc. ThunderX sata config.");
