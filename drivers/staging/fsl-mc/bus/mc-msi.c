/*
 * Freescale Management Complex (MC) bus driver MSI support
 *
 * Copyright (C) 2015 Freescale Semiconductor, Inc.
 * Author: German Rivera <German.Rivera@freescale.com>
 *
 * This file is licensed under the terms of the GNU General Public
 * License version 2. This program is licensed "as is" without any
 * warranty of any kind, whether express or implied.
 */

#include "../include/mc-private.h"
#include <linux/of_device.h>
#include <linux/of_address.h>
#include <linux/irqchip/arm-gic-v3.h>
#include <linux/of_irq.h>
#include <linux/irq.h>
#include <linux/irqdomain.h>
#include "../include/mc-sys.h"
#include "dprc-cmd.h"

static void __fsl_mc_msi_write_msg(struct fsl_mc_device *mc_bus_dev,
				   struct fsl_mc_device_irq *mc_dev_irq)
{
	int error;
	struct fsl_mc_device *owner_mc_dev = mc_dev_irq->mc_dev;
	struct msi_desc *msi_desc = mc_dev_irq->msi_desc;
	struct dprc_irq_cfg irq_cfg;

	/*
	 * msi_desc->msg.address is 0x0 when this function is invoked in
	 * the free_irq() code path. In this case, for the MC, we don't
	 * really need to "unprogram" the MSI, so we just return.
	 */
	if (msi_desc->msg.address_lo == 0x0 && msi_desc->msg.address_hi == 0x0)
		return;

	if (WARN_ON(!owner_mc_dev))
		return;

	irq_cfg.paddr = ((u64)msi_desc->msg.address_hi << 32) |
			msi_desc->msg.address_lo;
	irq_cfg.val = msi_desc->msg.data;
	irq_cfg.user_irq_id = msi_desc->irq;

	if (owner_mc_dev == mc_bus_dev) {
		/*
		 * IRQ is for the mc_bus_dev's DPRC itself
		 */
		error = dprc_set_irq(mc_bus_dev->mc_io,
				     MC_CMD_FLAG_INTR_DIS | MC_CMD_FLAG_PRI,
				     mc_bus_dev->mc_handle,
				     mc_dev_irq->dev_irq_index,
				     &irq_cfg);
		if (error < 0) {
			dev_err(&owner_mc_dev->dev,
				"dprc_set_irq() failed: %d\n", error);
		}
	} else {
		/*
		 * IRQ is for for a child device of mc_bus_dev
		 */
		error = dprc_set_obj_irq(mc_bus_dev->mc_io,
					 MC_CMD_FLAG_INTR_DIS | MC_CMD_FLAG_PRI,
					 mc_bus_dev->mc_handle,
					 owner_mc_dev->obj_desc.type,
					 owner_mc_dev->obj_desc.id,
					 mc_dev_irq->dev_irq_index,
					 &irq_cfg);
		if (error < 0) {
			dev_err(&owner_mc_dev->dev,
				"dprc_obj_set_irq() failed: %d\n", error);
		}
	}
}

/*
 * NOTE: This function is invoked with interrupts disabled
 */
void fsl_mc_msi_write_msg(struct msi_desc *msi_desc,
			  struct msi_msg *msg)
{
	struct fsl_mc_device *mc_bus_dev = to_fsl_mc_device(msi_desc->dev);
	struct fsl_mc_bus *mc_bus = to_fsl_mc_bus(mc_bus_dev);
	struct fsl_mc_device_irq *mc_dev_irq =
		&mc_bus->irq_resources[msi_desc->platform.msi_index];

	WARN_ON(mc_dev_irq->msi_desc != msi_desc);
	msi_desc->msg = *msg;

	/*
	 * Program the MSI (paddr, value) pair in the device:
	 */
	__fsl_mc_msi_write_msg(mc_bus_dev, mc_dev_irq);
}

int fsl_mc_find_msi_domain(struct device *mc_platform_dev,
			   struct irq_domain **mc_msi_domain)
{
	struct irq_domain *msi_domain;
	struct device_node *mc_of_node = mc_platform_dev->of_node;

	msi_domain = of_msi_get_domain(mc_platform_dev, mc_of_node,
				       DOMAIN_BUS_PLATFORM_MSI);
	if (!msi_domain) {
		pr_err("Unable to find fsl-mc MSI domain for %s\n",
		       mc_of_node->full_name);

		return -ENOENT;
	}

	*mc_msi_domain = msi_domain;
	return 0;
}

