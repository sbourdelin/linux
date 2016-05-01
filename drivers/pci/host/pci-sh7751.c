/*
 * SH7751 PCI driver
 * Copyright (C) 2016 Yoshinori Sato
 *
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/of_pci.h>
#include <linux/of_platform.h>
#include <linux/pci.h>
#include <linux/platform_device.h>
#include <linux/io.h>
#include "pci-host-common.h"
#include "pci-sh7751.h"

#define pcic_writel(val, reg) __raw_writel(val, pci_reg_base + (reg))
#define pcic_readl(reg) __raw_readl(pci_reg_base + (reg))

unsigned long PCIBIOS_MIN_IO;
unsigned long PCIBIOS_MIN_MEM;
DEFINE_RAW_SPINLOCK(pci_config_lock);

/*
 * PCIC fixups
 */

static __initconst const struct fixups {
	char *compatible;
	void (*fixup)(void __iomem *, void __iomem *);
} fixup_list[] = {
};

static __init void pcic_fixups(struct device_node *np,
		       void __iomem *pcic, void __iomem *bcr)
{
	int i;
	const struct fixups *f = fixup_list;

	for (i = 0; i < ARRAY_SIZE(fixup_list); i++) {
		if (of_device_is_compatible(np, f->compatible)) {
			f->fixup(pcic, bcr);
			break;
		}
	}
}

/*
 * Direct access to PCI hardware...
 */
#define CONFIG_CMD(bus, devfn, where) \
	(0x80000000 | (bus->number << 16) | (devfn << 8) | (where & ~3))

/*
 * Functions for accessing PCI configuration space with type 1 accesses
 */
static int sh4_pci_read(struct pci_bus *bus, unsigned int devfn,
			   int where, int size, u32 *val)
{
	struct gen_pci *pci = bus->sysdata;
	void __iomem *pci_reg_base = (void __iomem *)pci->cfg.res.start;
	unsigned long flags;
	u32 data;

	/*
	 * PCIPDR may only be accessed as 32 bit words,
	 * so we must do byte alignment by hand
	 */
	raw_spin_lock_irqsave(&pci_config_lock, flags);
	pcic_writel(CONFIG_CMD(bus, devfn, where), SH4_PCIPAR);
	data = pcic_readl(SH4_PCIPDR);
	raw_spin_unlock_irqrestore(&pci_config_lock, flags);

	switch (size) {
	case 1:
		*val = (data >> ((where & 3) << 3)) & 0xff;
		break;
	case 2:
		*val = (data >> ((where & 2) << 3)) & 0xffff;
		break;
	case 4:
		*val = data;
		break;
	default:
		return PCIBIOS_FUNC_NOT_SUPPORTED;
	}

	return PCIBIOS_SUCCESSFUL;
}

/*
 * Since SH4 only does 32bit access we'll have to do a read,
 * mask,write operation.
 * We'll allow an odd byte offset, though it should be illegal.
 */
static int sh4_pci_write(struct pci_bus *bus, unsigned int devfn,
			 int where, int size, u32 val)
{
	struct gen_pci *pci = bus->sysdata;
	void __iomem *pci_reg_base = (void __iomem *)pci->cfg.res.start;
	unsigned long flags;
	int shift;
	u32 data;

	raw_spin_lock_irqsave(&pci_config_lock, flags);
	pcic_writel(CONFIG_CMD(bus, devfn, where), SH4_PCIPAR);
	data = pcic_readl(SH4_PCIPDR);
	raw_spin_unlock_irqrestore(&pci_config_lock, flags);

	switch (size) {
	case 1:
		shift = (where & 3) << 3;
		data &= ~(0xff << shift);
		data |= ((val & 0xff) << shift);
		break;
	case 2:
		shift = (where & 2) << 3;
		data &= ~(0xffff << shift);
		data |= ((val & 0xffff) << shift);
		break;
	case 4:
		data = val;
		break;
	default:
		return PCIBIOS_FUNC_NOT_SUPPORTED;
	}

	pcic_writel(data, SH4_PCIPDR);

	return PCIBIOS_SUCCESSFUL;
}

static struct gen_pci_cfg_bus_ops pci_sh7751_ops = {
	.ops = {
		.read	= sh4_pci_read,
		.write	= sh4_pci_write,
	},
};

/*
 *  Called after each bus is probed, but before its children
 *  are examined.
 */
void pcibios_fixup_bus(struct pci_bus *bus)
{
}

/*
 * We need to avoid collisions with `mirrored' VGA ports
 * and other strange ISA hardware, so we always want the
 * addresses to be allocated in the 0x000-0x0ff region
 * modulo 0x400.
 */
resource_size_t pcibios_align_resource(void *data, const struct resource *res,
					      resource_size_t size, resource_size_t align)
{
	resource_size_t start = res->start;

	return start;
}

int pci_mmap_page_range(struct pci_dev *dev, struct vm_area_struct *vma,
			enum pci_mmap_state mmap_state, int write_combine)
{
	/*
	 * I/O space can be accessed via normal processor loads and stores on
	 * this platform but for now we elect not to do this and portable
	 * drivers should not do this anyway.
	 */
	if (mmap_state == pci_mmap_io)
		return -EINVAL;

	/*
	 * Ignore write-combine; for now only return uncached mappings.
	 */
	vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);

	return remap_pfn_range(vma, vma->vm_start, vma->vm_pgoff,
			       vma->vm_end - vma->vm_start,
			       vma->vm_page_prot);
}

static const struct of_device_id sh7751_pci_of_match[] = {
	{ .compatible = "renesas,sh7751-pci", },
	{ },
};
MODULE_DEVICE_TABLE(of, sh7751_pci_of_match);

static int __init area_sdram_check(void __iomem *pci_reg_base,
				   void __iomem *bcr,
				   unsigned int area)
{
	unsigned long word;

	word = __raw_readl(bcr + SH7751_BCR1);
	/* check BCR for SDRAM in area */
	if (((word >> area) & 1) == 0) {
		printk("PCI: Area %d is not configured for SDRAM. BCR1=0x%lx\n",
		       area, word);
		return 0;
	}
	pcic_writel(word, SH4_PCIBCR1);

	word = __raw_readw(bcr + SH7751_BCR2);
	/* check BCR2 for 32bit SDRAM interface*/
	if (((word >> (area << 1)) & 0x3) != 0x3) {
		printk("PCI: Area %d is not 32 bit SDRAM. BCR2=0x%lx\n",
		       area, word);
		return 0;
	}
	pcic_writel(word, SH4_PCIBCR2);

	return 1;
}

static __init int sh7751_pci_probe(struct platform_device *pdev)
{
	struct resource *res, *wres;
	u32 id;
	u32 reg, word;
	void __iomem *pci_reg_base;
	void __iomem *bcr;
	struct gen_pci *pci;

	pci = devm_kzalloc(&pdev->dev, sizeof(*pci), GFP_KERNEL);
	if (!pci)
		return -ENOMEM;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	pci_reg_base = (void __iomem *)res->start;
	if (IS_ERR(pci_reg_base))
		return PTR_ERR(pci_reg_base);

	wres = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	if (IS_ERR(wres))
		return PTR_ERR(wres);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 2);
	bcr = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(pci_reg_base))
		return PTR_ERR(bcr);

	/* check for SH7751/SH7751R hardware */
	id = pcic_readl(SH7751_PCICONF0);
	if (id != ((SH7751_DEVICE_ID << 16) | SH7751_VENDOR_ID) &&
	    id != ((SH7751R_DEVICE_ID << 16) | SH7751_VENDOR_ID)) {
		pr_warn("PCI: This is not an SH7751(R)\n");
		return -ENODEV;
	}
	dev_info(&pdev->dev, "PCI core found at %p\n",
		pci_reg_base);

	/* Set the BCR's to enable PCI access */
	reg = __raw_readl(bcr);
	reg |= 0x80000;
	__raw_writel(reg, bcr);

	/* Turn the clocks back on (not done in reset)*/
	pcic_writel(0, SH4_PCICLKR);
	/* Clear Powerdown IRQ's (not done in reset) */
	word = SH4_PCIPINT_D3 | SH4_PCIPINT_D0;
	pcic_writel(word, SH4_PCIPINT);

	/* set the command/status bits to:
	 * Wait Cycle Control + Parity Enable + Bus Master +
	 * Mem space enable
	 */
	word = SH7751_PCICONF1_WCC | SH7751_PCICONF1_PER |
	       SH7751_PCICONF1_BUM | SH7751_PCICONF1_MES;
	pcic_writel(word, SH7751_PCICONF1);

	/* define this host as the host bridge */
	word = PCI_BASE_CLASS_BRIDGE << 24;
	pcic_writel(word, SH7751_PCICONF2);

	/* Set IO and Mem windows to local address
	 * Make PCI and local address the same for easy 1 to 1 mapping
	 */
	word = wres->end - wres->start - 1;
	pcic_writel(word, SH4_PCILSR0);
	/* Set the values on window 0 PCI config registers */
	word = P2SEGADDR(wres->start);
	pcic_writel(word, SH4_PCILAR0);
	pcic_writel(word, SH7751_PCICONF5);

	/* check BCR for SDRAM in specified area */
	area_sdram_check(pci_reg_base, bcr, (wres->start >> 27) & 0x07);

	/* configure the wait control registers */
	word = __raw_readl(bcr + SH7751_WCR1);
	pcic_writel(word, SH4_PCIWCR1);
	word = __raw_readl(bcr + SH7751_WCR2);
	pcic_writel(word, SH4_PCIWCR2);
	word = __raw_readl(bcr + SH7751_WCR3);
	pcic_writel(word, SH4_PCIWCR3);
	word = __raw_readl(bcr + SH7751_MCR);
	pcic_writel(word, SH4_PCIMCR);

	pcic_fixups(pdev->dev.of_node, pci_reg_base, bcr);

	/* SH7751 init done, set central function init complete */
	/* use round robin mode to stop a device starving/overruning */
	word = SH4_PCICR_PREFIX | SH4_PCICR_CFIN | SH4_PCICR_ARBM;
	pcic_writel(word, SH4_PCICR);

	pci->cfg.ops = &pci_sh7751_ops;
	return pci_host_common_probe(pdev, pci);
}

static __refdata struct platform_driver sh7751_pci_driver = {
	.driver = {
		.name = "sh7751-pci",
		.of_match_table = sh7751_pci_of_match,
	},
	.probe = sh7751_pci_probe,
};
module_platform_driver(sh7751_pci_driver);

MODULE_DESCRIPTION("SH7751 PCI driver");
MODULE_LICENSE("GPL v2");
