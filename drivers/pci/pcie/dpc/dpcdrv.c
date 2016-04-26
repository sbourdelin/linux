/*
 * PCI-Express Downstream Port Containment services driver
 * Copyright (C) 2016 Intel Corp.
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 */

#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/pcieport_if.h>

struct event_info {
	struct pcie_device *dev;
	struct work_struct work;
};

static void dpc_wait_link_inactive(struct pci_dev *pdev)
{
	int timeout = jiffies + HZ;
	u16 lnk_status;

	pcie_capability_read_word(pdev, PCI_EXP_LNKSTA, &lnk_status);
	while (lnk_status & PCI_EXP_LNKSTA_DLLLA &&
					!time_after(jiffies, timeout)) {
		msleep(10);
		pcie_capability_read_word(pdev, PCI_EXP_LNKSTA, &lnk_status);
	} 
}

static void interrupt_event_handler(struct work_struct *work)
{
	int pos;

	struct event_info *info = container_of(work, struct event_info, work);
	struct pci_dev *dev, *temp, *pdev = info->dev->port;
	struct pci_bus *parent = pdev->subordinate;

	pci_lock_rescan_remove();
	list_for_each_entry_safe_reverse(dev, temp, &parent->devices,
					 bus_list) {
		pci_dev_get(dev);
		pci_stop_and_remove_bus_device(dev);
		pci_dev_put(dev);
	}
	pci_unlock_rescan_remove();

	dpc_wait_link_inactive(pdev);

	pos = pci_find_ext_capability(pdev, PCI_EXT_CAP_ID_DPC);
	pci_write_config_word(info->dev->port, pos + PCI_EXP_DPC_STATUS,
		PCI_EXP_DPC_STATUS_TRIGGER | PCI_EXP_DPC_STATUS_INTERRUPT);

	kfree(info);
}

static void dpc_queue_event(struct pcie_device *dev)
{
	struct event_info *info;

	info = kmalloc(sizeof(*info), GFP_ATOMIC);
	if (!info) {
		dev_warn(&dev->device, "dropped containment event\n");
		return;
	}

	INIT_WORK(&info->work, interrupt_event_handler);
	info->dev = dev;

	schedule_work(&info->work);
}

static irqreturn_t dpc_irq(int irq, void *context)
{
	int pos;
	u16 status, source;

	struct pcie_device *dev = (struct pcie_device *)context;
	struct pci_dev *pdev = dev->port;

	pos = pci_find_ext_capability(pdev, PCI_EXT_CAP_ID_DPC);
	pci_read_config_word(pdev, pos + PCI_EXP_DPC_STATUS, &status);
	pci_read_config_word(pdev, pos + PCI_EXP_DPC_SOURCE_ID, &source);

	if (!status)
		return IRQ_NONE;

	dev_warn(&dev->device, "dpc status:%04x source:%04x\n", status, source);

	if (status & PCI_EXP_DPC_STATUS_TRIGGER)
		dpc_queue_event(dev);

	return IRQ_HANDLED;
}

#define FLAG(x, y)	(((x) & (y)) ? '+' : '-')

static void dpc_enable_port(struct pcie_device *dev)
{
	struct pci_dev *pdev = dev->port;
	int pos;
	u16 ctl, cap;

	pos = pci_find_ext_capability(pdev, PCI_EXT_CAP_ID_DPC);
	pci_read_config_word(pdev, pos + PCI_EXP_DPC_CAP, &cap);
	pci_read_config_word(pdev, pos + PCI_EXP_DPC_CTL, &ctl);

	ctl |= PCI_EXP_DPC_CTL_EN_NONFATAL | PCI_EXP_DPC_CTL_INT_EN;
	pci_write_config_word(pdev, pos + PCI_EXP_DPC_CTL, ctl);

	dev_info(&dev->device,
		"DPC Int Msg #%d, RPExt%c PoisonedTLP%c SwTrigger%c RP PIO Log %d, DL_ActiveErr%c\n",
		cap & 0xf, FLAG(cap, PCI_EXP_DPC_CAP_RP_EXT),
		FLAG(cap, PCI_EXP_DPC_CAP_POISONED_TLP),
		FLAG(cap, PCI_EXP_DPC_CAP_SW_TRIGGER), (cap >> 8) & 0xf,
		FLAG(cap, PCI_EXP_DPC_CAP_DL_ACTIVE));
}

static void dpc_disable_port(struct pcie_device *dev)
{
	struct pci_dev *pdev = dev->port;
	int pos;
	u16 ctl;

	pos = pci_find_ext_capability(pdev, PCI_EXT_CAP_ID_DPC);
	pci_read_config_word(pdev, pos + PCI_EXP_DPC_CTL, &ctl);

	ctl |= ~(PCI_EXP_DPC_CTL_EN_NONFATAL | PCI_EXP_DPC_CTL_INT_EN);
	pci_write_config_word(pdev, pos + PCI_EXP_DPC_CTL, ctl);
}

static int dpc_probe(struct pcie_device *dev)
{
	int status;

	status = request_irq(dev->irq, dpc_irq, IRQF_SHARED, "dpcdrv", dev);
	if (status) {
		dev_warn(&dev->device, "request IRQ failed\n");
		return status;
	}
	dpc_enable_port(dev);

	return status;
}

static void dpc_remove(struct pcie_device *dev)
{
	dpc_disable_port(dev);
	free_irq(dev->irq, dev);
}

static struct pcie_port_service_driver dpcdriver = {
	.name		= "pciedpc",
	.port_type	= PCI_EXP_TYPE_ROOT_PORT | PCI_EXP_TYPE_DOWNSTREAM,
	.service	= PCIE_PORT_SERVICE_DPC,
	.probe		= dpc_probe,
	.remove		= dpc_remove,
};

static int __init dpc_service_init(void)
{
	/* XXX: Add kernel parameters to control PCIe DPC module */
	return pcie_port_service_register(&dpcdriver);
}

static void __exit dpc_service_exit(void)
{
	pcie_port_service_unregister(&dpcdriver);
}

MODULE_AUTHOR("Keith Busch <keith.busch@intel.com>");
MODULE_DESCRIPTION("PCI Express Downstream Port Containment driver");
MODULE_LICENSE("GPL");
MODULE_VERSION("0.1");

module_init(dpc_service_init);
module_exit(dpc_service_exit);
