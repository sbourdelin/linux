#include <linux/kernel.h>
#include <linux/of_pci.h>
#include <linux/of_irq.h>
#include <linux/pm_wakeirq.h>
#include <linux/export.h>

int of_pci_setup_wake_irq(struct pci_dev *pdev)
{
	struct pci_dev *ppdev;
	struct device_node *dn;
	int ret, irq;

	/* Get the pci_dev of our parent. Hopefully it's a port. */
	ppdev = pdev->bus->self;
	/* Nope, it's a host bridge. */
	if (!ppdev)
		return 0;

	dn = pci_device_to_OF_node(ppdev);
	if (!dn)
		return 0;

	irq = of_irq_get_byname(dn, "wakeup");
	if (irq == -EPROBE_DEFER) {
		return irq;
	} else if (irq < 0) {
		/* Ignore other errors, since a missing wakeup is non-fatal. */
		dev_info(&pdev->dev, "cannot get wakeup interrupt: %d\n", irq);
		return 0;
	}

	device_init_wakeup(&pdev->dev, true);

	ret = dev_pm_set_dedicated_wake_irq(&pdev->dev, irq);
	if (ret < 0) {
		dev_err(&pdev->dev, "failed to set wake IRQ: %d\n", ret);
		device_init_wakeup(&pdev->dev, false);
		return ret;
	}

	/* Start out disabled to avoid irq storm */
	dev_pm_disable_wake_irq(&pdev->dev);

	return 0;
}
EXPORT_SYMBOL_GPL(of_pci_setup_wake_irq);

void of_pci_teardown_wake_irq(struct pci_dev *pdev)
{
	dev_pm_clear_wake_irq(&pdev->dev);
	device_init_wakeup(&pdev->dev, false);
}
EXPORT_SYMBOL_GPL(of_pci_teardown_wake_irq);

/**
 * of_irq_parse_pci - Resolve the interrupt for a PCI device
 * @pdev:       the device whose interrupt is to be resolved
 * @out_irq:    structure of_irq filled by this function
 *
 * This function resolves the PCI interrupt for a given PCI device. If a
 * device-node exists for a given pci_dev, it will use normal OF tree
 * walking. If not, it will implement standard swizzling and walk up the
 * PCI tree until an device-node is found, at which point it will finish
 * resolving using the OF tree walking.
 */
int of_irq_parse_pci(const struct pci_dev *pdev, struct of_phandle_args *out_irq)
{
	struct device_node *dn, *ppnode;
	struct pci_dev *ppdev;
	__be32 laddr[3];
	u8 pin;
	int rc;

	/* Check if we have a device node, if yes, fallback to standard
	 * device tree parsing
	 */
	dn = pci_device_to_OF_node(pdev);
	if (dn) {
		struct property *prop;
		const char *name;
		int index = 0;

		of_property_for_each_string(dn, "interrupt-names", prop, name) {
			if (!strcmp(name, "pci"))
				break;
			index++;
		}

		/*
		 * Only parse from DT if we have no "interrupt-names",
		 * or if we found an interrupt named "pci".
		 */
		if (index == 0 || name) {
			rc = of_irq_parse_one(dn, index, out_irq);
			if (!rc)
				return rc;
		}
	}

	/* Ok, we don't, time to have fun. Let's start by building up an
	 * interrupt spec.  we assume #interrupt-cells is 1, which is standard
	 * for PCI. If you do different, then don't use that routine.
	 */
	rc = pci_read_config_byte(pdev, PCI_INTERRUPT_PIN, &pin);
	if (rc != 0)
		goto err;
	/* No pin, exit with no error message. */
	if (pin == 0)
		return -ENODEV;

	/* Now we walk up the PCI tree */
	for (;;) {
		/* Get the pci_dev of our parent */
		ppdev = pdev->bus->self;

		/* Ouch, it's a host bridge... */
		if (ppdev == NULL) {
			ppnode = pci_bus_to_OF_node(pdev->bus);

			/* No node for host bridge ? give up */
			if (ppnode == NULL) {
				rc = -EINVAL;
				goto err;
			}
		} else {
			/* We found a P2P bridge, check if it has a node */
			ppnode = pci_device_to_OF_node(ppdev);
		}

		/* Ok, we have found a parent with a device-node, hand over to
		 * the OF parsing code.
		 * We build a unit address from the linux device to be used for
		 * resolution. Note that we use the linux bus number which may
		 * not match your firmware bus numbering.
		 * Fortunately, in most cases, interrupt-map-mask doesn't
		 * include the bus number as part of the matching.
		 * You should still be careful about that though if you intend
		 * to rely on this function (you ship  a firmware that doesn't
		 * create device nodes for all PCI devices).
		 */
		if (ppnode)
			break;

		/* We can only get here if we hit a P2P bridge with no node,
		 * let's do standard swizzling and try again
		 */
		pin = pci_swizzle_interrupt_pin(pdev, pin);
		pdev = ppdev;
	}

	out_irq->np = ppnode;
	out_irq->args_count = 1;
	out_irq->args[0] = pin;
	laddr[0] = cpu_to_be32((pdev->bus->number << 16) | (pdev->devfn << 8));
	laddr[1] = laddr[2] = cpu_to_be32(0);
	rc = of_irq_parse_raw(laddr, out_irq);
	if (rc)
		goto err;
	return 0;
err:
	if (rc == -ENOENT) {
		dev_warn(&pdev->dev,
			"%s: no interrupt-map found, INTx interrupts not available\n",
			__func__);
		pr_warn_once("%s: possibly some PCI slots don't have level triggered interrupts capability\n",
			__func__);
	} else {
		dev_err(&pdev->dev, "%s: failed with rc=%d\n", __func__, rc);
	}
	return rc;
}
EXPORT_SYMBOL_GPL(of_irq_parse_pci);

/**
 * of_irq_parse_and_map_pci() - Decode a PCI irq from the device tree and map to a virq
 * @dev: The pci device needing an irq
 * @slot: PCI slot number; passed when used as map_irq callback. Unused
 * @pin: PCI irq pin number; passed when used as map_irq callback. Unused
 *
 * @slot and @pin are unused, but included in the function so that this
 * function can be used directly as the map_irq callback to
 * pci_assign_irq() and struct pci_host_bridge.map_irq pointer
 */
int of_irq_parse_and_map_pci(const struct pci_dev *dev, u8 slot, u8 pin)
{
	struct of_phandle_args oirq;
	int ret;

	ret = of_irq_parse_pci(dev, &oirq);
	if (ret)
		return 0; /* Proper return code 0 == NO_IRQ */

	return irq_create_of_mapping(&oirq);
}
EXPORT_SYMBOL_GPL(of_irq_parse_and_map_pci);

