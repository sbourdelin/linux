#include <linux/module.h>
#include <linux/irqchip/chained_irq.h>
#include <linux/irqdomain.h>
#include <linux/pci-ecam.h>
#include <linux/msi.h>

#define MSI_MAX 256

struct tango_pcie {
	DECLARE_BITMAP(bitmap, MSI_MAX);
	spinlock_t lock;
	void __iomem *mux;
	void __iomem *msi_status;
	void __iomem *msi_mask;
	phys_addr_t msi_doorbell;
	struct irq_domain *irq_domain;
	struct irq_domain *msi_domain;
	int irq;
};

/*** MSI CONTROLLER SUPPORT ***/

static void dispatch(struct tango_pcie *pcie, unsigned long status, int base)
{
	unsigned int pos, virq;

	for_each_set_bit(pos, &status, 32) {
		virq = irq_find_mapping(pcie->irq_domain, base + pos);
		generic_handle_irq(virq);
	}
}

static void tango_msi_isr(struct irq_desc *desc)
{
	u32 status;
	struct irq_chip *chip = irq_desc_get_chip(desc);
	struct tango_pcie *pcie = irq_desc_get_handler_data(desc);
	unsigned int base, offset, pos = 0;

	chained_irq_enter(chip, desc);

	while ((pos = find_next_bit(pcie->bitmap, MSI_MAX, pos)) < MSI_MAX) {
		base = round_down(pos, 32);
		offset = (pos / 32) * 4;
		status = readl_relaxed(pcie->msi_status + offset);
		writel_relaxed(status, pcie->msi_status + offset);
		dispatch(pcie, status, base);
		pos = base + 32;
	}

	chained_irq_exit(chip, desc);
}

static struct irq_chip tango_msi_irq_chip = {
	.name = "MSI",
	.irq_mask = pci_msi_mask_irq,
	.irq_unmask = pci_msi_unmask_irq,
};

#define USE_DEF_OPS (MSI_FLAG_USE_DEF_DOM_OPS | MSI_FLAG_USE_DEF_CHIP_OPS)

static struct msi_domain_info msi_domain_info = {
	.flags	= USE_DEF_OPS | MSI_FLAG_PCI_MSIX,
	.chip	= &tango_msi_irq_chip,
};

static void tango_compose_msi_msg(struct irq_data *data, struct msi_msg *msg)
{
	struct tango_pcie *pcie = irq_data_get_irq_chip_data(data);

	msg->address_lo = lower_32_bits(pcie->msi_doorbell);
	msg->address_hi = upper_32_bits(pcie->msi_doorbell);
	msg->data = data->hwirq;
}

static int tango_set_affinity(struct irq_data *irq_data,
		const struct cpumask *mask, bool force)
{
	 return -EINVAL;
}

static struct irq_chip tango_msi_chip = {
	.name			= "MSI",
	.irq_compose_msi_msg	= tango_compose_msi_msg,
	.irq_set_affinity	= tango_set_affinity,
};

static int find_free_msi(struct irq_domain *dom, unsigned int virq)
{
	u32 val;
	struct tango_pcie *pcie = dom->host_data;
	unsigned int offset, pos;

	pos = find_first_zero_bit(pcie->bitmap, MSI_MAX);
	if (pos >= MSI_MAX)
		return -ENOSPC;

	offset = (pos / 32) * 4;
	val = readl_relaxed(pcie->msi_mask + offset);
	writel_relaxed(val | BIT(pos % 32), pcie->msi_mask + offset);
	__set_bit(pos, pcie->bitmap);

	irq_domain_set_info(dom, virq, pos, &tango_msi_chip,
			dom->host_data, handle_simple_irq, NULL, NULL);

	return 0;
}

static int tango_irq_domain_alloc(struct irq_domain *dom,
		unsigned int virq, unsigned int nr_irqs, void *args)
{
	int err;
	struct tango_pcie *pcie = dom->host_data;

	spin_lock(&pcie->lock);
	err = find_free_msi(dom, virq);
	spin_unlock(&pcie->lock);

	return err;
}

static void tango_irq_domain_free(struct irq_domain *dom,
		unsigned int virq, unsigned int nr_irqs)
{
	u32 val;
	struct irq_data *d = irq_domain_get_irq_data(dom, virq);
	struct tango_pcie *pcie = irq_data_get_irq_chip_data(d);
	unsigned int offset, pos = d->hwirq;

	spin_lock(&pcie->lock);

	offset = (pos / 32) * 4;
	val = readl_relaxed(pcie->msi_mask + offset);
	writel_relaxed(val & ~BIT(pos % 32), pcie->msi_mask + offset);
	__clear_bit(pos, pcie->bitmap);

	spin_unlock(&pcie->lock);
}

static const struct irq_domain_ops msi_dom_ops = {
	.alloc	= tango_irq_domain_alloc,
	.free	= tango_irq_domain_free,
};

static int tango_msi_remove(struct platform_device *pdev)
{
	struct tango_pcie *msi = platform_get_drvdata(pdev);

	irq_set_chained_handler_and_data(msi->irq, NULL, NULL);
	irq_domain_remove(msi->msi_domain);
	irq_domain_remove(msi->irq_domain);

	return 0;
}

static int tango_msi_probe(struct platform_device *pdev, struct tango_pcie *pcie)
{
	int i, virq;
	struct fwnode_handle *fwnode = of_node_to_fwnode(pdev->dev.of_node);
	struct irq_domain *msi_dom, *irq_dom;

	spin_lock_init(&pcie->lock);

	for (i = 0; i < MSI_MAX / 32; ++i)
		writel_relaxed(0, pcie->msi_mask + i * 4);

	irq_dom = irq_domain_create_linear(fwnode, MSI_MAX, &msi_dom_ops, pcie);
	if (!irq_dom) {
		pr_err("Failed to create IRQ domain\n");
		return -ENOMEM;
	}

	msi_dom = pci_msi_create_irq_domain(fwnode, &msi_domain_info, irq_dom);
	if (!msi_dom) {
		pr_err("Failed to create MSI domain\n");
		irq_domain_remove(irq_dom);
		return -ENOMEM;
	}

	virq = platform_get_irq(pdev, 1);
	if (virq <= 0) {
		pr_err("Failed to map IRQ\n");
		irq_domain_remove(msi_dom);
		irq_domain_remove(irq_dom);
		return -ENXIO;
	}

	pcie->irq_domain = irq_dom;
	pcie->msi_domain = msi_dom;
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
	if (devfn != 0) {
		*val = ~0; /* Is this required even if we return an error? */
		return PCIBIOS_FUNC_NOT_SUPPORTED; /* Error seems appropriate */
	}

	/*
	 * QUIRK #2
	 * The root complex advertizes a fake BAR, which is used to filter
	 * bus-to-system requests. Hide it from Linux.
	 */
	if (where == PCI_BASE_ADDRESS_0 && bus->number == 0) {
		*val = 0; /* 0 or ~0 to hide the BAR from Linux? */
		return PCIBIOS_SUCCESSFUL; /* Should we return error or success? */
	}

	/*
	 * QUIRK #3
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

static void smp8759_init(struct tango_pcie *pcie, void __iomem *base)
{
	pcie->mux		= base + 0x48;
	pcie->msi_status	= base + 0x80;
	pcie->msi_mask		= base + 0xa0;
	pcie->msi_doorbell	= 0xa0000000 + 0x2e07c;
}

static int tango_pcie_probe(struct platform_device *pdev)
{
	int ret;
	void __iomem *base;
	struct resource *res;
	struct tango_pcie *pcie;
	struct device *dev = &pdev->dev;

	pcie = devm_kzalloc(dev, sizeof(*pcie), GFP_KERNEL);
	if (!pcie)
		return -ENOMEM;

	platform_set_drvdata(pdev, pcie);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(base))
		return PTR_ERR(base);

	if (of_device_is_compatible(dev->of_node, "sigma,smp8759-pcie"))
		smp8759_init(pcie, base);

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

module_platform_driver(tango_pcie_driver);

#define VENDOR_SIGMA	0x1105

/*
 * QUIRK #4
 * The root complex advertizes the wrong device class.
 * Header Type 1 is for PCI-to-PCI bridges.
 */
static void tango_fixup_class(struct pci_dev *dev)
{
	dev->class = PCI_CLASS_BRIDGE_PCI << 8;
}
DECLARE_PCI_FIXUP_EARLY(VENDOR_SIGMA, PCI_ANY_ID, tango_fixup_class);

/*
 * QUIRK #5
 * Only transfers within the root complex BAR are forwarded to the host.
 * By default, the DMA framework expects that
 * PCI address 0x8000_0000 maps to system address 0x8000_0000
 * which is where DRAM0 is mapped.
 */
static void tango_fixup_bar(struct pci_dev *dev)
{
        pci_write_config_dword(dev, PCI_BASE_ADDRESS_0, 0x80000000);
}
DECLARE_PCI_FIXUP_FINAL(VENDOR_SIGMA, PCI_ANY_ID, tango_fixup_bar);
