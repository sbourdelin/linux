#include <linux/irqchip/chained_irq.h>
#include <linux/irqdomain.h>
#include <linux/pci-ecam.h>
#include <linux/delay.h>
#include <linux/msi.h>
#include <linux/of.h>

#define MSI_MAX 256

#define SMP8759_MUX		0x48
#define SMP8759_TEST_OUT	0x74
#define SMP8759_STATUS		0x80
#define SMP8759_ENABLE		0xa0
#define SMP8759_DOORBELL	0xa002e07c

struct tango_pcie {
	DECLARE_BITMAP(used, MSI_MAX);
	spinlock_t lock;
	void __iomem *mux;
	void __iomem *msi_status;
	void __iomem *msi_enable;
	phys_addr_t msi_doorbell;
	struct irq_domain *irq_dom;
	struct irq_domain *msi_dom;
	int irq;
};

/*** MSI CONTROLLER SUPPORT ***/

static void tango_msi_isr(struct irq_desc *desc)
{
	struct irq_chip *chip = irq_desc_get_chip(desc);
	struct tango_pcie *pcie = irq_desc_get_handler_data(desc);
	unsigned long status, base, virq, idx, pos = 0;

	chained_irq_enter(chip, desc);

	while ((pos = find_next_bit(pcie->used, MSI_MAX, pos)) < MSI_MAX) {
		base = round_down(pos, 32);
		status = readl_relaxed(pcie->msi_status + base / 8);
		for_each_set_bit(idx, &status, 32) {
			virq = irq_find_mapping(pcie->irq_dom, base + idx);
			generic_handle_irq(virq);
		}
		pos = base + 32;
	}

	chained_irq_exit(chip, desc);
}

static void tango_ack(struct irq_data *d)
{
	struct tango_pcie *pcie = d->chip_data;
	u32 offset = (d->hwirq / 32) * 4;
	u32 bit = BIT(d->hwirq % 32);

	writel_relaxed(bit, pcie->msi_status + offset);
}

static void update_msi_enable(struct irq_data *d, bool unmask)
{
	unsigned long flags;
	struct tango_pcie *pcie = d->chip_data;
	u32 offset = (d->hwirq / 32) * 4;
	u32 bit = BIT(d->hwirq % 32);
	u32 val;

	spin_lock_irqsave(&pcie->lock, flags);
	val = readl_relaxed(pcie->msi_enable + offset);
	val = unmask ? val | bit : val & ~bit;
	writel_relaxed(val, pcie->msi_enable + offset);
	spin_unlock_irqrestore(&pcie->lock, flags);
}

static void tango_mask(struct irq_data *d)
{
	update_msi_enable(d, false);
}

static void tango_unmask(struct irq_data *d)
{
	update_msi_enable(d, true);
}

static int tango_set_affinity(struct irq_data *d,
		const struct cpumask *mask, bool force)
{
	return -EINVAL;
}

static void tango_compose_msi_msg(struct irq_data *d, struct msi_msg *msg)
{
	struct tango_pcie *pcie = d->chip_data;
	msg->address_lo = lower_32_bits(pcie->msi_doorbell);
	msg->address_hi = upper_32_bits(pcie->msi_doorbell);
	msg->data = d->hwirq;
}

static struct irq_chip tango_chip = {
	.irq_ack		= tango_ack,
	.irq_mask		= tango_mask,
	.irq_unmask		= tango_unmask,
	.irq_set_affinity	= tango_set_affinity,
	.irq_compose_msi_msg	= tango_compose_msi_msg,
};

static void msi_ack(struct irq_data *d)
{
	irq_chip_ack_parent(d);
}

static void msi_mask(struct irq_data *d)
{
	pci_msi_mask_irq(d);
	irq_chip_mask_parent(d);
}

static void msi_unmask(struct irq_data *d)
{
	pci_msi_unmask_irq(d);
	irq_chip_unmask_parent(d);
}

static struct irq_chip msi_chip = {
	.name = "MSI",
	.irq_ack = msi_ack,
	.irq_mask = msi_mask,
	.irq_unmask = msi_unmask,
};

static struct msi_domain_info msi_dom_info = {
	.flags	= MSI_FLAG_PCI_MSIX
		| MSI_FLAG_USE_DEF_DOM_OPS
		| MSI_FLAG_USE_DEF_CHIP_OPS,
	.chip	= &msi_chip,
};

static int tango_irq_domain_alloc(struct irq_domain *dom,
		unsigned int virq, unsigned int nr_irqs, void *args)
{
	unsigned long flags;
	int pos, err = -ENOSPC;
	struct tango_pcie *pcie = dom->host_data;

	spin_lock_irqsave(&pcie->lock, flags);
	pos = find_first_zero_bit(pcie->used, MSI_MAX);
	if (pos < MSI_MAX) {
		err = 0;
		__set_bit(pos, pcie->used);
		irq_domain_set_info(dom, virq, pos,
				&tango_chip, pcie, handle_edge_irq, NULL, NULL);
	}
	spin_unlock_irqrestore(&pcie->lock, flags);

	return err;
}

static void tango_irq_domain_free(struct irq_domain *dom,
		unsigned int virq, unsigned int nr_irqs)
{
	unsigned long flags;
	struct irq_data *d = irq_domain_get_irq_data(dom, virq);
	struct tango_pcie *pcie = d->chip_data;

	spin_lock_irqsave(&pcie->lock, flags);
	__clear_bit(d->hwirq, pcie->used);
	spin_unlock_irqrestore(&pcie->lock, flags);
}

static const struct irq_domain_ops irq_dom_ops = {
	.alloc	= tango_irq_domain_alloc,
	.free	= tango_irq_domain_free,
};

static int tango_msi_remove(struct platform_device *pdev)
{
	struct tango_pcie *pcie = platform_get_drvdata(pdev);

	irq_set_chained_handler_and_data(pcie->irq, NULL, NULL);
	irq_domain_remove(pcie->msi_dom);
	irq_domain_remove(pcie->irq_dom);

	return 0;
}

static int tango_msi_probe(struct platform_device *pdev, struct tango_pcie *pcie)
{
	int i, virq;
	struct irq_domain *msi_dom, *irq_dom;
	struct fwnode_handle *fwnode = of_node_to_fwnode(pdev->dev.of_node);

	spin_lock_init(&pcie->lock);
	for (i = 0; i < MSI_MAX / 32; ++i)
		writel_relaxed(0, pcie->msi_enable + i * 4);

	virq = platform_get_irq(pdev, 1);
	if (virq <= 0) {
		pr_err("Failed to map IRQ\n");
		return -ENXIO;
	}

	irq_dom = irq_domain_create_linear(fwnode, MSI_MAX, &irq_dom_ops, pcie);
	if (!irq_dom) {
		pr_err("Failed to create IRQ domain\n");
		return -ENOMEM;
	}

	msi_dom = pci_msi_create_irq_domain(fwnode, &msi_dom_info, irq_dom);
	if (!msi_dom) {
		pr_err("Failed to create MSI domain\n");
		return -ENOMEM;
	}

	pcie->irq_dom = irq_dom;
	pcie->msi_dom = msi_dom;
	pcie->irq = virq;

	irq_set_chained_handler_and_data(virq, tango_msi_isr, pcie);

	return 0;
}

/*** HOST BRIDGE SUPPORT ***/

static int smp8759_config_read(struct pci_bus *bus,
		unsigned int devfn, int where, int size, u32 *val)
{
	int ret;
	struct pci_config_window *cfg = bus->sysdata;
	struct tango_pcie *pcie = dev_get_drvdata(cfg->parent);

	/*
	 * QUIRK #1
	 * Reads in configuration space outside devfn 0 return garbage.
	 */
	if (devfn != 0)
		return PCIBIOS_FUNC_NOT_SUPPORTED;

	/*
	 * QUIRK #2
	 * Unfortunately, config and mem spaces are muxed.
	 * Linux does not support such a setting, since drivers are free
	 * to access mem space directly, at any time.
	 * Therefore, we can only PRAY that config and mem space accesses
	 * NEVER occur concurrently.
	 */
	writel_relaxed(1, pcie->mux);
	ret = pci_generic_config_read(bus, devfn, where, size, val);
	writel_relaxed(0, pcie->mux);

	return ret;
}

static int smp8759_config_write(struct pci_bus *bus,
		unsigned int devfn, int where, int size, u32 val)
{
	int ret;
	struct pci_config_window *cfg = bus->sysdata;
	struct tango_pcie *pcie = dev_get_drvdata(cfg->parent);

	writel_relaxed(1, pcie->mux);
	ret = pci_generic_config_write(bus, devfn, where, size, val);
	writel_relaxed(0, pcie->mux);

	return ret;
}

static struct pci_ecam_ops smp8759_ecam_ops = {
	.bus_shift	= 20,
	.pci_ops	= {
		.map_bus	= pci_ecam_map_bus,
		.read		= smp8759_config_read,
		.write		= smp8759_config_write,
	}
};

static const struct of_device_id tango_pcie_ids[] = {
	{ .compatible = "sigma,smp8759-pcie" },
	{ /* sentinel */ },
};

static int tango_check_pcie_link(void __iomem *test_out)
{
	int i;

	writel_relaxed(16, test_out);
	for (i = 0; i < 10; ++i) {
		u32 ltssm_state = readl_relaxed(test_out) >> 8;
		if ((ltssm_state & 0x1f) == 0xf) /* L0 */
			return 0;
		usleep_range(3000, 4000);
	}

	return -ENODEV;
}

static int smp8759_init(struct tango_pcie *pcie, void __iomem *base)
{
	pcie->mux		= base + SMP8759_MUX;
	pcie->msi_status	= base + SMP8759_STATUS;
	pcie->msi_enable	= base + SMP8759_ENABLE;
	pcie->msi_doorbell	= SMP8759_DOORBELL;

	return tango_check_pcie_link(base + SMP8759_TEST_OUT);
}

static int tango_pcie_probe(struct platform_device *pdev)
{
	int ret = -EINVAL;
	void __iomem *base;
	struct resource *res;
	struct tango_pcie *pcie;
	struct device *dev = &pdev->dev;

	pr_err("MAJOR ISSUE: PCIe config and mem spaces are muxed\n");
	pr_err("Tainting kernel... Use driver at your own risk\n");
	add_taint(TAINT_FIRMWARE_WORKAROUND, LOCKDEP_STILL_OK);

	pcie = devm_kzalloc(dev, sizeof(*pcie), GFP_KERNEL);
	if (!pcie)
		return -ENOMEM;

	platform_set_drvdata(pdev, pcie);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(base))
		return PTR_ERR(base);

	if (of_device_is_compatible(dev->of_node, "sigma,smp8759-pcie"))
		ret = smp8759_init(pcie, base);

	if (ret)
		return ret;

	ret = tango_msi_probe(pdev, pcie);
	if (ret)
		return ret;

	return pci_host_common_probe(pdev, &smp8759_ecam_ops);
}

static int tango_pcie_remove(struct platform_device *pdev)
{
	return tango_msi_remove(pdev);
}

static struct platform_driver tango_pcie_driver = {
	.probe	= tango_pcie_probe,
	.remove	= tango_pcie_remove,
	.driver	= {
		.name = KBUILD_MODNAME,
		.of_match_table = tango_pcie_ids,
	},
};

builtin_platform_driver(tango_pcie_driver);

/*
 * QUIRK #3
 * The root complex advertizes the wrong device class.
 * Header Type 1 is for PCI-to-PCI bridges.
 */
static void tango_fixup_class(struct pci_dev *dev)
{
	dev->class = PCI_CLASS_BRIDGE_PCI << 8;
}
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_SIGMA, 0x24, tango_fixup_class);
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_SIGMA, 0x28, tango_fixup_class);

/*
 * QUIRK #4
 * The root complex exposes a "fake" BAR, which is used to filter
 * bus-to-system accesses. Only accesses within the range defined
 * by this BAR are forwarded to the host, others are ignored.
 *
 * By default, the DMA framework expects an identity mapping,
 * and DRAM0 is mapped at 0x80000000.
 */
static void tango_fixup_bar(struct pci_dev *dev)
{
	dev->non_compliant_bars = true;
	pci_write_config_dword(dev, PCI_BASE_ADDRESS_0, 0x80000000);
}
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_SIGMA, 0x24, tango_fixup_bar);
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_SIGMA, 0x28, tango_fixup_bar);
