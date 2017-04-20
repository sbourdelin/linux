#include <linux/irqchip/chained_irq.h>
#include <linux/irqdomain.h>
#include <linux/pci-ecam.h>
#include <linux/delay.h>
#include <linux/msi.h>

#define MSI_MAX 256

struct tango_pcie {
	DECLARE_BITMAP(bitmap, MSI_MAX);
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

static void dispatch(struct tango_pcie *pcie, unsigned long status, int base)
{
	unsigned int pos, virq;

	for_each_set_bit(pos, &status, 32) {
		virq = irq_find_mapping(pcie->irq_dom, base + pos);
		generic_handle_irq(virq);
	}
}

static void tango_msi_isr(struct irq_desc *desc)
{
	struct irq_chip *chip = irq_desc_get_chip(desc);
	struct tango_pcie *pcie = irq_desc_get_handler_data(desc);
	unsigned int status, base, pos = 0;

	chained_irq_enter(chip, desc);

	while ((pos = find_next_bit(pcie->bitmap, MSI_MAX, pos)) < MSI_MAX) {
		base = round_down(pos, 32);
		status = readl_relaxed(pcie->msi_status + base / 8);
		dispatch(pcie, status, base);
		pos = base + 32;
	}

	chained_irq_exit(chip, desc);
}

static void tango_ack(struct irq_data *data)
{
	u32 bit = BIT(data->hwirq);
	struct tango_pcie *pcie = irq_data_get_irq_chip_data(data);

	writel_relaxed(bit, pcie->msi_status);
}

static void update_msi_enable(struct irq_data *data, bool unmask)
{
	unsigned long flags;
	u32 val, bit = BIT(data->hwirq % 32);
	int byte_offset = (data->hwirq / 32) * 4;
	struct tango_pcie *pcie = data->chip_data;

	spin_lock_irqsave(&pcie->lock, flags);
	val = readl_relaxed(pcie->msi_enable + byte_offset);
	val = unmask ? val | bit : val & ~bit;
	writel_relaxed(val, pcie->msi_enable + byte_offset);
	spin_unlock_irqrestore(&pcie->lock, flags);
}

static void tango_mask(struct irq_data *data)
{
	update_msi_enable(data, false);
}

static void tango_unmask(struct irq_data *data)
{
	update_msi_enable(data, true);
}

static int tango_set_affinity(struct irq_data *data,
		const struct cpumask *mask, bool force)
{
	return -EINVAL;
}

static void tango_compose_msi_msg(struct irq_data *data, struct msi_msg *msg)
{
	struct tango_pcie *pcie = irq_data_get_irq_chip_data(data);

	msg->address_lo = lower_32_bits(pcie->msi_doorbell);
	msg->address_hi = upper_32_bits(pcie->msi_doorbell);
	msg->data = data->hwirq;
}

static struct irq_chip tango_chip = {
	.irq_ack		= tango_ack,
	.irq_mask		= tango_mask,
	.irq_unmask		= tango_unmask,
	.irq_set_affinity	= tango_set_affinity,
	.irq_compose_msi_msg	= tango_compose_msi_msg,
};

static void msi_ack(struct irq_data *data)
{
	irq_chip_ack_parent(data);
}

static void msi_mask(struct irq_data *data)
{
	pci_msi_mask_irq(data);
	irq_chip_mask_parent(data);
}

static void msi_unmask(struct irq_data *data)
{
	pci_msi_unmask_irq(data);
	irq_chip_unmask_parent(data);
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

static int find_free_msi(struct irq_domain *dom, unsigned int virq)
{
	unsigned int pos;
	struct tango_pcie *pcie = dom->host_data;

	pos = find_first_zero_bit(pcie->bitmap, MSI_MAX);
	if (pos >= MSI_MAX)
		return -ENOSPC;
	__set_bit(pos, pcie->bitmap);

	irq_domain_set_info(dom, virq, pos, &tango_chip, pcie,
			handle_edge_irq, NULL, NULL);

	return 0;
}

static int tango_irq_domain_alloc(struct irq_domain *dom,
		unsigned int virq, unsigned int nr_irqs, void *args)
{
	int err;
	unsigned long flags;
	struct tango_pcie *pcie = dom->host_data;

	spin_lock_irqsave(&pcie->lock, flags);
	err = find_free_msi(dom, virq);
	spin_unlock_irqrestore(&pcie->lock, flags);

	return err;
}

static void tango_irq_domain_free(struct irq_domain *dom,
		unsigned int virq, unsigned int nr_irqs)
{
	unsigned long flags;
	struct irq_data *data = irq_domain_get_irq_data(dom, virq);
	struct tango_pcie *pcie = irq_data_get_irq_chip_data(data);

	spin_lock_irqsave(&pcie->lock, flags);
	__clear_bit(data->hwirq, pcie->bitmap);
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

	irq_dom = irq_domain_create_linear(fwnode, MSI_MAX, &irq_dom_ops, pcie);
	if (!irq_dom) {
		pr_err("Failed to create IRQ domain\n");
		return -ENOMEM;
	}

	msi_dom = pci_msi_create_irq_domain(fwnode, &msi_dom_info, irq_dom);
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

	pcie->irq_dom = irq_dom;
	pcie->msi_dom = msi_dom;
	pcie->irq = virq;
	irq_set_chained_handler_and_data(virq, tango_msi_isr, pcie);

	return 0;
}
