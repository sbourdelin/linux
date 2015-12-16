/*
 * Copyright (C) 2014-2015 Synopsys, Inc. (www.synopsys.com)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/delay.h>
#include <linux/string.h>
#include <linux/init.h>
#include <linux/sched.h>
#include <linux/errno.h>
#include <linux/bootmem.h>
#include <linux/sizes.h>
#include <linux/slab.h>

#include <asm/pci.h>
#include <asm/mach/pci.h>

unsigned long pcibios_min_io = 0x100;
EXPORT_SYMBOL(pcibios_min_io);

unsigned long pcibios_min_mem = 0x100000;
EXPORT_SYMBOL(pcibios_min_mem);

static int pcibios_init_resources(int busnr, struct pci_sys_data *sys)
{
	int ret;
	struct resource_entry *window;

	if (list_empty(&sys->resources)) {
		pci_add_resource_offset(&sys->resources,
			 &iomem_resource, sys->mem_offset);
	}

	resource_list_for_each_entry(window, &sys->resources) {
		if (resource_type(window->res) == IORESOURCE_IO)
			return 0;
	}

	sys->io_res.start = (busnr * SZ_64K) ?  : pcibios_min_io;
	sys->io_res.end = (busnr + 1) * SZ_64K - 1;
	sys->io_res.flags = IORESOURCE_IO;
	sys->io_res.name = sys->io_res_name;
	sprintf(sys->io_res_name, "PCI%d I/O", busnr);

	ret = request_resource(&ioport_resource, &sys->io_res);
	if (ret) {
		pr_err("PCI: unable to allocate I/O port region (%d)\n", ret);
		return ret;
	}
	pci_add_resource_offset(&sys->resources, &sys->io_res,
				sys->io_offset);

	return 0;
}


static void pcibios_init_hw(struct device *parent, struct hw_pci *hw,
			    struct list_head *head)
{
	struct pci_sys_data *sys = NULL;
	int ret;
	int nr, busnr;

	for (nr = busnr = 0; nr < hw->nr_controllers; nr++) {
		sys = kzalloc(sizeof(struct pci_sys_data), GFP_KERNEL);
		if (!sys)
			panic("PCI: unable to allocate sys data!");

#ifdef CONFIG_PCI_DOMAINS
		sys->domain  = hw->domain;
#endif
		sys->busnr   = busnr;
		sys->swizzle = hw->swizzle;
		sys->map_irq = hw->map_irq;
		sys->align_resource = hw->align_resource;
		sys->add_bus = hw->add_bus;
		sys->remove_bus = hw->remove_bus;
		INIT_LIST_HEAD(&sys->resources);


		if (hw->private_data)
			sys->private_data = hw->private_data[nr];

		ret = hw->setup(nr, sys);

		if (ret > 0) {
			ret = pcibios_init_resources(nr, sys);
			if (ret)  {
				kfree(sys);
				break;
			}

			if (hw->scan)
				sys->bus = hw->scan(nr, sys);
			else
				sys->bus = pci_scan_root_bus(parent, sys->busnr,
						hw->ops, sys, &sys->resources);

			if (!sys->bus)
				panic("PCI: unable to scan bus!");

			busnr = sys->bus->busn_res.end + 1;

			list_add(&sys->node, head);
		} else {
			kfree(sys);
			if (ret < 0)
				break;
		}
	}
}

int pcibios_root_bridge_prepare(struct pci_host_bridge *bridge)
{
	return 0;
}

void pcibios_add_bus(struct pci_bus *bus)
{
}

void pcibios_remove_bus(struct pci_bus *bus)
{
}

/*
 * Swizzle the device pin each time we cross a bridge.  If a platform does
 * not provide a swizzle function, we perform the standard PCI swizzling.
 *
 * The default swizzling walks up the bus tree one level at a time, applying
 * the standard swizzle function at each step, stopping when it finds the PCI
 * root bus.  This will return the slot number of the bridge device on the
 * root bus and the interrupt pin on that device which should correspond
 * with the downstream device interrupt.
 *
 * Platforms may override this, in which case the slot and pin returned
 * depend entirely on the platform code.  However, please note that the
 * PCI standard swizzle is implemented on plug-in cards and Cardbus based
 * PCI extenders, so it can not be ignored.
 */
static u8 pcibios_swizzle(struct pci_dev *dev, u8 *pin)
{
	int slot;
	struct pci_sys_data *sys = dev->sysdata;

	if (sys->swizzle)
		slot = sys->swizzle(dev, pin);
	else
		slot = pci_common_swizzle(dev, pin);

	return slot;
}

/*
 * Map a slot/pin to an IRQ.
 */
static int pcibios_map_irq(const struct pci_dev *dev, u8 slot, u8 pin)
{
	struct pci_sys_data *sys = dev->sysdata;
	int irq = -1;

	if (sys->map_irq)
		irq = sys->map_irq(dev, slot, pin);

	return irq;
}

void pci_common_init_dev(struct device *parent, struct hw_pci *hw)
{
	struct pci_sys_data *sys;
	LIST_HEAD(head);

	pci_add_flags(PCI_REASSIGN_ALL_RSRC);
	if (hw->preinit)
		hw->preinit();
	pcibios_init_hw(parent, hw, &head);
	if (hw->postinit)
		hw->postinit();

	pci_fixup_irqs(pcibios_swizzle, pcibios_map_irq);

	list_for_each_entry(sys, &head, node) {
		struct pci_bus *bus = sys->bus;

		if (!pci_has_flag(PCI_PROBE_ONLY)) {
			/*
			 * Size the bridge windows.
			 */
			pci_bus_size_bridges(bus);

			/*
			 * Assign resources.
			 */
			pci_bus_assign_resources(bus);
		}

		/*
		 * Tell drivers about devices found.
		 */
		pci_bus_add_devices(bus);
	}
}

/*
 * We don't have to worry about legacy ISA devices, so nothing to do here
 */
resource_size_t pcibios_align_resource(void *data, const struct resource *res,
				resource_size_t size, resource_size_t align)
{
	return res->start;
}

/**
 * pcibios_enable_device - Enable I/O and memory.
 * @dev: PCI device to be enabled
 */
int pcibios_enable_device(struct pci_dev *pci_dev, int mask)
{
	u16 cmd, old_cmd;
	int idx;
	struct resource *r;

	pci_read_config_word(pci_dev, PCI_COMMAND, &cmd);
	old_cmd = cmd;
	for (idx = 0; idx < 6; idx++) {
		/* Only set up the requested stuff */
		if (!(mask & (1 << idx)))
			continue;

		r = pci_dev->resource + idx;
		if (!r->start && r->end) {
			dev_err(&pci_dev->dev, "PCI: Device %s not available\n",
							pci_name(pci_dev));
			return -EINVAL;
		}
		if (r->flags & IORESOURCE_IO)
			cmd |= PCI_COMMAND_IO;
		if (r->flags & IORESOURCE_MEM)
			cmd |= PCI_COMMAND_MEMORY;
	}

	/*
	 * Bridges (eg, cardbus bridges) need to be fully enabled
	 */
	if ((pci_dev->class >> 16) == PCI_BASE_CLASS_BRIDGE)
		cmd |= PCI_COMMAND_IO | PCI_COMMAND_MEMORY;

	if (cmd != old_cmd) {
		pr_info("PCI: enabling device %s (%04x -> %04x)\n",
		       pci_name(pci_dev), old_cmd, cmd);
		pci_write_config_word(pci_dev, PCI_COMMAND, cmd);
	}
	return 0;
}

/*
 * If the bus contains any of these devices, then we must not turn on
 * parity checking of any kind.  Currently this is CyberPro 20x0 only.
 */
static inline int pdev_bad_for_parity(struct pci_dev *dev)
{
	return ((dev->vendor == PCI_VENDOR_ID_INTERG &&
		 (dev->device == PCI_DEVICE_ID_INTERG_2000 ||
		  dev->device == PCI_DEVICE_ID_INTERG_2010)) ||
		(dev->vendor == PCI_VENDOR_ID_ITE &&
		 dev->device == PCI_DEVICE_ID_ITE_8152));

}

/*
 * pcibios_fixup_bus - Called after each bus is probed,
 * but before its children are examined.
 */
void pcibios_fixup_bus(struct pci_bus *bus)
{
	struct pci_dev *dev;
	u16 features = PCI_COMMAND_SERR | PCI_COMMAND_PARITY
			| PCI_COMMAND_FAST_BACK;

	/*
	 * Walk the devices on this bus, working out what we can
	 * and can't support.
	 */
	list_for_each_entry(dev, &bus->devices, bus_list) {
		u16 status;

		pci_read_config_word(dev, PCI_STATUS, &status);

		/*
		 * If any device on this bus does not support fast back
		 * to back transfers, then the bus as a whole is not able
		 * to support them.  Having fast back to back transfers
		 * on saves us one PCI cycle per transaction.
		 */
		if (!(status & PCI_STATUS_FAST_BACK))
			features &= ~PCI_COMMAND_FAST_BACK;

		if (pdev_bad_for_parity(dev))
			features &= ~(PCI_COMMAND_SERR | PCI_COMMAND_PARITY);

		switch (dev->class >> 8) {
		case PCI_CLASS_BRIDGE_PCI:
			pci_read_config_word(dev, PCI_BRIDGE_CONTROL, &status);
			status |= PCI_BRIDGE_CTL_PARITY
					|PCI_BRIDGE_CTL_MASTER_ABORT;
			status &= ~(PCI_BRIDGE_CTL_BUS_RESET
					|PCI_BRIDGE_CTL_FAST_BACK);
			pci_write_config_word(dev, PCI_BRIDGE_CONTROL, status);
			break;

		case PCI_CLASS_BRIDGE_CARDBUS:
			pci_read_config_word(dev, PCI_CB_BRIDGE_CONTROL,
						&status);
			status |= PCI_CB_BRIDGE_CTL_PARITY
					|PCI_CB_BRIDGE_CTL_MASTER_ABORT;
			pci_write_config_word(dev, PCI_CB_BRIDGE_CONTROL,
						status);
			break;
		}
	}

	/*
	 * Now walk the devices again, this time setting them up.
	 */
	list_for_each_entry(dev, &bus->devices, bus_list) {
		u16 cmd;

		pci_read_config_word(dev, PCI_COMMAND, &cmd);
		cmd |= features;
		pci_write_config_word(dev, PCI_COMMAND, cmd);

		pci_write_config_byte(dev, PCI_CACHE_LINE_SIZE,
				      L1_CACHE_BYTES >> 2);
	}

	/*
	 * Propagate the flags to the PCI bridge.
	 */
	if (bus->self && bus->self->hdr_type == PCI_HEADER_TYPE_BRIDGE) {
		if (features & PCI_COMMAND_FAST_BACK)
			bus->bridge_ctl |= PCI_BRIDGE_CTL_FAST_BACK;
		if (features & PCI_COMMAND_PARITY)
			bus->bridge_ctl |= PCI_BRIDGE_CTL_PARITY;
	}

	/*
	 * Report what we did for this bus
	 */
	pr_info("PCI: bus%d: Fast back to back transfers %sabled\n",
		bus->number, (features & PCI_COMMAND_FAST_BACK) ? "en" : "dis");
}
EXPORT_SYMBOL(pcibios_fixup_bus);
