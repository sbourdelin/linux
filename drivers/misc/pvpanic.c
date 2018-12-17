// SPDX-License-Identifier: GPL-2.0+
/*
 *  Pvpanic Device Support
 *
 *  Copyright (C) 2013 Fujitsu.
 *  Copyright (C) 2018 ZTE.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/acpi.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/pci.h>
#include <linux/platform_device.h>
#include <linux/types.h>

static void __iomem *base;

#define PCI_VENDOR_ID_REDHAT             0x1b36
#define PCI_DEVICE_ID_REDHAT_PVPANIC     0x0101
#define PVPANIC_PANICKED        (1 << 0)

MODULE_AUTHOR("Hu Tao <hutao@cn.fujitsu.com>");
MODULE_DESCRIPTION("pvpanic device driver");
MODULE_LICENSE("GPL");

static void
pvpanic_send_event(unsigned int event)
{
	iowrite8(event, base);
}

static int
pvpanic_panic_notify(struct notifier_block *nb, unsigned long code,
		     void *unused)
{
	pvpanic_send_event(PVPANIC_PANICKED);
	return NOTIFY_DONE;
}

static struct notifier_block pvpanic_panic_nb = {
	.notifier_call = pvpanic_panic_notify,
	.priority = 1, /* let this called before broken drm_fb_helper */
};

#ifdef CONFIG_ACPI
static int pvpanic_add(struct acpi_device *device);
static int pvpanic_remove(struct acpi_device *device);

static const struct acpi_device_id pvpanic_device_ids[] = {
	{ "QEMU0001", 0 },
	{ "", 0 }
};
MODULE_DEVICE_TABLE(acpi, pvpanic_device_ids);

static struct acpi_driver pvpanic_driver = {
	.name =		"pvpanic",
	.class =	"QEMU",
	.ids =		pvpanic_device_ids,
	.ops =		{
				.add =		pvpanic_add,
				.remove =	pvpanic_remove,
			},
	.owner =	THIS_MODULE,
};

static acpi_status
pvpanic_walk_resources(struct acpi_resource *res, void *context)
{
	struct resource r;

	if (acpi_dev_resource_io(res, &r)) {
		base = ioport_map(r.start, resource_size(&r));
		return AE_OK;
	} else if (acpi_dev_resource_memory(res, &r)) {
		base = ioremap(r.start, resource_size(&r));
		return AE_OK;
	}

	return AE_ERROR;
}

static int pvpanic_add(struct acpi_device *device)
{
	int ret;

	ret = acpi_bus_get_status(device);
	if (ret < 0)
		return ret;

	if (!device->status.enabled || !device->status.functional)
		return -ENODEV;

	acpi_walk_resources(device->handle, METHOD_NAME__CRS,
			    pvpanic_walk_resources, NULL);

	if (!base)
		return -ENODEV;

	atomic_notifier_chain_register(&panic_notifier_list,
				       &pvpanic_panic_nb);

	return 0;
}

static int pvpanic_remove(struct acpi_device *device)
{

	atomic_notifier_chain_unregister(&panic_notifier_list,
					 &pvpanic_panic_nb);
	iounmap(base);

	return 0;
}

static int pvpanic_register_acpi_driver(void)
{
	return acpi_bus_register_driver(&pvpanic_driver);
}

static void pvpanic_unregister_acpi_driver(void)
{
	acpi_bus_unregister_driver(&pvpanic_driver);
}
#else
static int pvpanic_register_acpi_driver(void)
{
	return 0;
}

static void pvpanic_unregister_acpi_driver(void) {}
#endif

static int pvpanic_mmio_probe(struct platform_device *pdev)
{
	struct resource *mem;

	mem = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!mem)
		return -EINVAL;

	base = devm_ioremap_resource(&pdev->dev, mem);
	if (IS_ERR(base))
		return PTR_ERR(base);

	atomic_notifier_chain_register(&panic_notifier_list,
				       &pvpanic_panic_nb);

	return 0;
}

static int pvpanic_mmio_remove(struct platform_device *pdev)
{

	atomic_notifier_chain_unregister(&panic_notifier_list,
					 &pvpanic_panic_nb);

	return 0;
}

static const struct of_device_id pvpanic_mmio_match[] = {
	{ .compatible = "qemu,pvpanic-mmio", },
	{}
};

static struct platform_driver pvpanic_mmio_driver = {
	.driver = {
		.name = "pvpanic-mmio",
		.of_match_table = pvpanic_mmio_match,
	},
	.probe = pvpanic_mmio_probe,
	.remove = pvpanic_mmio_remove,
};

#ifdef CONFIG_PCI
static const struct pci_device_id pvpanic_pci_id_tbl[]  = {
	{ PCI_DEVICE(PCI_VENDOR_ID_REDHAT, PCI_DEVICE_ID_REDHAT_PVPANIC),},
	{}
};

static int pvpanic_pci_probe(struct pci_dev *pdev,
			      const struct pci_device_id *ent)
{
	int err;

	err = pci_enable_device(pdev);
	if (err)
		return err;
	if (pci_request_region(pdev, 0, "pvpanic-pci"))
		goto disable;

	base = pci_ioremap_bar(pdev, 0);
	if (!base)
		goto release;

	atomic_notifier_chain_register(&panic_notifier_list,
				       &pvpanic_panic_nb);
	return 0;

release:
	pci_release_region(pdev, 0);
disable:
	pci_disable_device(pdev);

	return -ENODEV;
}

static void pvpanic_pci_remove(struct pci_dev *pdev)
{
	atomic_notifier_chain_unregister(&panic_notifier_list,
					 &pvpanic_panic_nb);
	iounmap(base);
	pci_release_region(pdev, 0);
	pci_disable_device(pdev);
}

static struct pci_driver pvpanic_pci_driver = {
	.name =         "pvpanic-pci",
	.id_table =     pvpanic_pci_id_tbl,
	.probe =        pvpanic_pci_probe,
	.remove =       pvpanic_pci_remove,
};

static int pvpanic_register_pci_drvier(void)
{
	return pci_register_driver(&pvpanic_pci_driver);
}

static void pvpanic_unregister_pci_drvier(void)
{
	pci_unregister_driver(&pvpanic_pci_driver);
}
#else /* CONFIG_PCI */
static int pvpanic_register_pci_drvier(void)
{
	return 0;
}

static void pvpanic_unregister_pci_drvier(void) {}
#endif

static int __init pvpanic_mmio_init(void)
{
	int r1, r2;

	if (acpi_disabled)
		r1 = platform_driver_register(&pvpanic_mmio_driver);
	else
		r1 = pvpanic_register_acpi_driver();
	r2 = pvpanic_register_pci_drvier();
	if (r1 && r2) /* all drivers register failed */
		return 1;
	else
		return 0;
}

static void __exit pvpanic_mmio_exit(void)
{
	if (acpi_disabled)
		platform_driver_unregister(&pvpanic_mmio_driver);
	else
		pvpanic_unregister_acpi_driver();
	pvpanic_unregister_pci_drvier();
}

module_init(pvpanic_mmio_init);
module_exit(pvpanic_mmio_exit);
