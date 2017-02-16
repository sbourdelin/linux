/*
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 *
 * Copyright (C) 2016, 2017 Cavium Inc.
 */

#include <linux/gpio/driver.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/spinlock.h>


#define GPIO_RX_DAT	0x0
#define GPIO_TX_SET	0x8
#define GPIO_TX_CLR	0x10
#define GPIO_CONST	0x90
#define  GPIO_CONST_GPIOS_MASK 0xff
#define GPIO_BIT_CFG	0x400
#define  GPIO_BIT_CFG_TX_OE		BIT(0)
#define  GPIO_BIT_CFG_PIN_XOR		BIT(1)
#define  GPIO_BIT_CFG_INT_EN		BIT(2)
#define  GPIO_BIT_CFG_INT_TYPE		BIT(3)
#define  GPIO_BIT_CFG_FIL_CNT_SHIFT	4
#define  GPIO_BIT_CFG_FIL_SEL_SHIFT	8
#define  GPIO_BIT_CFG_TX_OD		BIT(12)
#define  GPIO_BIT_CFG_PIN_SEL_MASK	GENMASK(25, 16)
#define GPIO_INTR	0x800
#define  GPIO_INTR_INTR			BIT(0)
#define  GPIO_INTR_INTR_W1S		BIT(1)
#define  GPIO_INTR_ENA_W1C		BIT(2)
#define  GPIO_INTR_ENA_W1S		BIT(3)
#define GPIO_2ND_BANK	0x1400

#define GLITCH_FILTER_400NS ((4ull << GPIO_BIT_CFG_FIL_SEL_SHIFT) | \
			     (9ull << GPIO_BIT_CFG_FIL_CNT_SHIFT))

struct thunderx_gpio;

struct thunderx_line {
	struct thunderx_gpio	*txgpio;
	unsigned int		line;
};

struct thunderx_gpio {
	struct gpio_chip	chip;
	u8 __iomem		*register_base;
	struct irq_domain	*irqd;
	struct msix_entry	*msix_entries;	/* per line MSI-X */
	struct thunderx_line	*line_entries;	/* per line irq info */
	raw_spinlock_t		lock;
	unsigned long		invert_mask[2];
	unsigned long		od_mask[2];
	int			base_msi;
};

static unsigned int bit_cfg_reg(unsigned int line)
{
	return 8 * line + GPIO_BIT_CFG;
}

static unsigned int intr_reg(unsigned int line)
{
	return 8 * line + GPIO_INTR;
}

/*
 * Check (and WARN) that the pin is available for GPIO.  We will not
 * allow modification of the state of non-GPIO pins from this driver.
 */
static bool thunderx_gpio_is_gpio(struct thunderx_gpio *txgpio,
				  unsigned int line)
{
	u64 bit_cfg = readq(txgpio->register_base + bit_cfg_reg(line));
	bool rv = (bit_cfg & GPIO_BIT_CFG_PIN_SEL_MASK) == 0;

	WARN_RATELIMIT(!rv, "Pin %d not available for GPIO\n", line);

	return rv;
}

static int thunderx_gpio_dir_in(struct gpio_chip *chip, unsigned int line)
{
	struct thunderx_gpio *txgpio = gpiochip_get_data(chip);

	if (!thunderx_gpio_is_gpio(txgpio, line))
		return -EIO;

	raw_spin_lock(&txgpio->lock);
	clear_bit(line, txgpio->invert_mask);
	clear_bit(line, txgpio->od_mask);
	writeq(GLITCH_FILTER_400NS, txgpio->register_base + bit_cfg_reg(line));
	raw_spin_unlock(&txgpio->lock);
	return 0;
}

static void thunderx_gpio_set(struct gpio_chip *chip, unsigned int line,
			      int value)
{
	struct thunderx_gpio *txgpio = gpiochip_get_data(chip);
	int bank = line / 64;
	int bank_bit = line % 64;

	void __iomem *reg = txgpio->register_base +
		(bank * GPIO_2ND_BANK) + (value ? GPIO_TX_SET : GPIO_TX_CLR);

	writeq(BIT_ULL(bank_bit), reg);
}

static int thunderx_gpio_dir_out(struct gpio_chip *chip, unsigned int line,
				 int value)
{
	struct thunderx_gpio *txgpio = gpiochip_get_data(chip);
	u64 bit_cfg = GPIO_BIT_CFG_TX_OE;

	if (!thunderx_gpio_is_gpio(txgpio, line))
		return -EIO;

	raw_spin_lock(&txgpio->lock);

	thunderx_gpio_set(chip, line, value);

	if (test_bit(line, txgpio->invert_mask))
		bit_cfg |= GPIO_BIT_CFG_PIN_XOR;

	if (test_bit(line, txgpio->od_mask))
		bit_cfg |= GPIO_BIT_CFG_TX_OD;

	writeq(bit_cfg, txgpio->register_base + bit_cfg_reg(line));

	raw_spin_unlock(&txgpio->lock);
	return 0;
}

/*
 * Weird, setting open-drain mode causes signal inversion.  Note this
 * so we can compensate in the dir_out function.
 */
static int thunderx_gpio_set_single_ended(struct gpio_chip *chip,
					  unsigned int line,
					  enum single_ended_mode mode)
{
	struct thunderx_gpio *txgpio = gpiochip_get_data(chip);

	if (mode == LINE_MODE_OPEN_SOURCE)
		return -ENOTSUPP;

	if (!thunderx_gpio_is_gpio(txgpio, line))
		return -EIO;

	raw_spin_lock(&txgpio->lock);
	if (mode == LINE_MODE_OPEN_DRAIN) {
		set_bit(line, txgpio->invert_mask);
		set_bit(line, txgpio->od_mask);
	} else {
		clear_bit(line, txgpio->invert_mask);
		clear_bit(line, txgpio->od_mask);
	}
	raw_spin_unlock(&txgpio->lock);

	return 0;
}

static int thunderx_gpio_get(struct gpio_chip *chip, unsigned int line)
{
	struct thunderx_gpio *txgpio = gpiochip_get_data(chip);
	int bank = line / 64;
	int bank_bit = line % 64;
	u64 read_bits = readq(txgpio->register_base + (bank * GPIO_2ND_BANK) + GPIO_RX_DAT);
	u64 masked_bits = read_bits & BIT_ULL(bank_bit);

	if (test_bit(line, txgpio->invert_mask))
		return masked_bits == 0;
	else
		return masked_bits != 0;
}

static void thunderx_gpio_set_multiple(struct gpio_chip *chip,
				       unsigned long *mask,
				       unsigned long *bits)
{
	int bank;
	u64 set_bits, clear_bits;
	struct thunderx_gpio *txgpio = gpiochip_get_data(chip);

	for (bank = 0; bank <= chip->ngpio / 64; bank++) {
		set_bits = bits[bank] & mask[bank];
		clear_bits = ~bits[bank] & mask[bank];
		writeq(set_bits, txgpio->register_base + (bank * GPIO_2ND_BANK) + GPIO_TX_SET);
		writeq(clear_bits, txgpio->register_base + (bank * GPIO_2ND_BANK) + GPIO_TX_CLR);
	}
}

static void thunderx_gpio_irq_ack(struct irq_data *data)
{
	struct thunderx_line *txline = irq_data_get_irq_chip_data(data);

	writeq(GPIO_INTR_INTR,
	       txline->txgpio->register_base + intr_reg(txline->line));
}

static void thunderx_gpio_irq_mask(struct irq_data *data)
{
	struct thunderx_line *txline = irq_data_get_irq_chip_data(data);

	writeq(GPIO_INTR_ENA_W1C,
	       txline->txgpio->register_base + intr_reg(txline->line));
}

static void thunderx_gpio_irq_mask_ack(struct irq_data *data)
{
	struct thunderx_line *txline = irq_data_get_irq_chip_data(data);

	writeq(GPIO_INTR_ENA_W1C | GPIO_INTR_INTR,
	       txline->txgpio->register_base + intr_reg(txline->line));
}

static void thunderx_gpio_irq_unmask(struct irq_data *data)
{
	struct thunderx_line *txline = irq_data_get_irq_chip_data(data);

	writeq(GPIO_INTR_ENA_W1S,
	       txline->txgpio->register_base + intr_reg(txline->line));
}

static int thunderx_gpio_irq_set_type(struct irq_data *data,
				      unsigned int flow_type)
{
	struct thunderx_line *txline = irq_data_get_irq_chip_data(data);
	struct thunderx_gpio *txgpio = txline->txgpio;
	u64 bit_cfg;

	irqd_set_trigger_type(data, flow_type);

	bit_cfg = GLITCH_FILTER_400NS | GPIO_BIT_CFG_INT_EN;

	if (flow_type & IRQ_TYPE_EDGE_BOTH) {
		irq_set_handler_locked(data, handle_fasteoi_edge_irq);
		bit_cfg |= GPIO_BIT_CFG_INT_TYPE;
	} else {
		irq_set_handler_locked(data, handle_fasteoi_level_irq);
	}

	raw_spin_lock(&txgpio->lock);
	if (flow_type & (IRQ_TYPE_EDGE_FALLING | IRQ_TYPE_LEVEL_LOW)) {
		bit_cfg |= GPIO_BIT_CFG_PIN_XOR;
		set_bit(txline->line, txgpio->invert_mask);
	} else {
		clear_bit(txline->line, txgpio->invert_mask);
	}
	clear_bit(txline->line, txgpio->od_mask);
	writeq(bit_cfg, txgpio->register_base + bit_cfg_reg(txline->line));
	raw_spin_unlock(&txgpio->lock);

	return IRQ_SET_MASK_OK;
}

static void thunderx_gpio_irq_enable(struct irq_data *data)
{
	irq_chip_enable_parent(data);
	thunderx_gpio_irq_unmask(data);
}

static void thunderx_gpio_irq_disable(struct irq_data *data)
{
	thunderx_gpio_irq_mask(data);
	irq_chip_disable_parent(data);
}

/*
 * Interrupts are chained from underlying MSI-X vectors.  We have
 * these irq_chip functions to be able to handle level triggering
 * semantics and other acknowledgment tasks associated with the GPIO
 * mechanism.
 */
static struct irq_chip thunderx_gpio_irq_chip = {
	.name			= "GPIO",
	.irq_enable		= thunderx_gpio_irq_enable,
	.irq_disable		= thunderx_gpio_irq_disable,
	.irq_ack		= thunderx_gpio_irq_ack,
	.irq_mask		= thunderx_gpio_irq_mask,
	.irq_mask_ack		= thunderx_gpio_irq_mask_ack,
	.irq_unmask		= thunderx_gpio_irq_unmask,
	.irq_eoi		= irq_chip_eoi_parent,
	.irq_set_affinity	= irq_chip_set_affinity_parent,
	.irq_set_type		= thunderx_gpio_irq_set_type,

	.flags			= IRQCHIP_SET_TYPE_MASKED
};

static int thunderx_gpio_irq_map(struct irq_domain *d, unsigned int irq,
				 irq_hw_number_t hwirq)
{
	irq_set_irq_type(irq, IRQ_TYPE_LEVEL_LOW);
	return 0;
}

static int thunderx_gpio_irq_translate(struct irq_domain *d,
				       struct irq_fwspec *fwspec,
				       irq_hw_number_t *hwirq,
				       unsigned int *type)
{
	if (WARN_ON(fwspec->param_count < 2))
		return -EINVAL;
	*hwirq = fwspec->param[0];
	*type = fwspec->param[1] & IRQ_TYPE_SENSE_MASK;
	return 0;
}

static int thunderx_gpio_irq_alloc(struct irq_domain *d, unsigned int virq,
				   unsigned int nr_irqs, void *arg)
{
	struct thunderx_line *txline = arg;

	return irq_domain_set_hwirq_and_chip(d, virq, txline->line,
					     &thunderx_gpio_irq_chip, txline);
}

static const struct irq_domain_ops thunderx_gpio_irqd_ops = {
	.map		= thunderx_gpio_irq_map,
	.alloc		= thunderx_gpio_irq_alloc,
	.translate	= thunderx_gpio_irq_translate
};

static int thunderx_gpio_to_irq(struct gpio_chip *chip, unsigned int offset)
{
	struct thunderx_gpio *txgpio = gpiochip_get_data(chip);

	return irq_find_mapping(txgpio->irqd, offset);
}

static int thunderx_gpio_probe(struct pci_dev *pdev,
			       const struct pci_device_id *id)
{
	void __iomem * const *tbl;
	struct device *dev = &pdev->dev;
	struct thunderx_gpio *txgpio;
	struct gpio_chip *chip;
	int ngpio, i;
	int err = 0;

	txgpio = devm_kzalloc(dev, sizeof(*txgpio), GFP_KERNEL);
	if (!txgpio)
		return -ENOMEM;

	raw_spin_lock_init(&txgpio->lock);
	chip = &txgpio->chip;

	pci_set_drvdata(pdev, txgpio);

	err = pcim_enable_device(pdev);
	if (err) {
		dev_err(dev, "Failed to enable PCI device: err %d\n", err);
		goto out;
	}

	err = pcim_iomap_regions(pdev, 1 << 0, KBUILD_MODNAME);
	if (err) {
		dev_err(dev, "Failed to iomap PCI device: err %d\n", err);
		goto out;
	}

	tbl = pcim_iomap_table(pdev);
	txgpio->register_base = tbl[0];
	if (!txgpio->register_base) {
		dev_err(dev, "Cannot map PCI resource\n");
		err = -ENOMEM;
		goto out;
	}

	if (pdev->subsystem_device == 0xa10a) {
		/* CN88XX has no GPIO_CONST register*/
		ngpio = 50;
		txgpio->base_msi = 48;
	} else {
		u64 c = readq(txgpio->register_base + GPIO_CONST);

		ngpio = c & GPIO_CONST_GPIOS_MASK;
		txgpio->base_msi = (c >> 8) & 0xff;
	}

	txgpio->msix_entries = devm_kzalloc(dev,
					  sizeof(struct msix_entry) * ngpio,
					  GFP_KERNEL);
	if (!txgpio->msix_entries) {
		err = -ENOMEM;
		goto out;
	}

	txgpio->line_entries = devm_kzalloc(dev,
					    sizeof(struct thunderx_line) * ngpio,
					    GFP_KERNEL);
	if (!txgpio->line_entries) {
		err = -ENOMEM;
		goto out;
	}

	for (i = 0; i < ngpio; i++) {
		txgpio->msix_entries[i].entry = txgpio->base_msi + (2 * i);
		txgpio->line_entries[i].line = i;
		txgpio->line_entries[i].txgpio = txgpio;
	}


	/* Enable all MSI-X for interrupts on all possible lines. */
	err = pci_enable_msix(pdev, txgpio->msix_entries, ngpio);
	if (err < 0)
		goto out;

	/*
	 * Push GPIO specific irqdomain on hierarchy created as a side
	 * effect of the pci_enable_msix()
	 */
	txgpio->irqd = irq_domain_create_hierarchy(irq_get_irq_data(txgpio->msix_entries[0].vector)->domain,
						   0, 0, of_node_to_fwnode(dev->of_node),
						   &thunderx_gpio_irqd_ops, txgpio);
	if (!txgpio->irqd)
		goto out;

	/* Push on irq_data and the domain for each line. */
	for (i = 0; i < ngpio; i++) {
		err = irq_domain_push_irq(txgpio->irqd,
					  txgpio->msix_entries[i].vector,
					  &txgpio->line_entries[i]);
		if (err < 0)
			dev_err(dev, "irq_domain_push_irq: %d\n", err);
	}

	chip->label = KBUILD_MODNAME;
	chip->parent = dev;
	chip->owner = THIS_MODULE;
	chip->base = -1; /* System allocated */
	chip->can_sleep = false;
	chip->ngpio = ngpio;
	chip->direction_input = thunderx_gpio_dir_in;
	chip->get = thunderx_gpio_get;
	chip->direction_output = thunderx_gpio_dir_out;
	chip->set = thunderx_gpio_set;
	chip->set_multiple = thunderx_gpio_set_multiple;
	chip->set_single_ended = thunderx_gpio_set_single_ended;
	chip->to_irq = thunderx_gpio_to_irq;
	err = devm_gpiochip_add_data(dev, chip, txgpio);
	if (err)
		goto out;

	dev_info(dev, "ThunderX GPIO: %d lines with base %d.\n",
		 ngpio, chip->base);
	return 0;
out:
	pci_set_drvdata(pdev, NULL);
	return err;
}

static void thunderx_gpio_remove(struct pci_dev *pdev)
{
	int i;
	struct thunderx_gpio *txgpio = pci_get_drvdata(pdev);

	for (i = 0; i < txgpio->chip.ngpio; i++)
		irq_domain_pop_irq(txgpio->irqd,
				   txgpio->msix_entries[i].vector);

	irq_domain_remove(txgpio->irqd);

	pci_set_drvdata(pdev, NULL);
}

static const struct pci_device_id thunderx_gpio_id_table[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_CAVIUM, 0xA00A) },
	{ 0, }	/* end of table */
};

MODULE_DEVICE_TABLE(pci, thunderx_gpio_id_table);

static struct pci_driver thunderx_gpio_driver = {
	.name = KBUILD_MODNAME,
	.id_table = thunderx_gpio_id_table,
	.probe = thunderx_gpio_probe,
	.remove = thunderx_gpio_remove,
};

module_pci_driver(thunderx_gpio_driver);

MODULE_DESCRIPTION("Cavium Inc. ThunderX/OCTEON-TX GPIO Driver");
MODULE_LICENSE("GPL");
