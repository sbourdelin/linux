/*
 * linux/include/asm-generic/pci.h
 *
 *  Copyright (C) 2003 Russell King
 */
#ifndef _ASM_GENERIC_PCI_H
#define _ASM_GENERIC_PCI_H

#ifndef HAVE_ARCH_PCI_GET_LEGACY_IDE_IRQ
static inline int pci_get_legacy_ide_irq(struct pci_dev *dev, int channel)
{
	return channel ? 15 : 14;
}
#endif /* HAVE_ARCH_PCI_GET_LEGACY_IDE_IRQ */

/*
 * By default, assume that no iommu is in use and that the PCI
 * space is mapped to address physical 0.
 */
#ifndef PCI_DMA_BUS_IS_PHYS
#define PCI_DMA_BUS_IS_PHYS	(1)
#endif

#ifdef CONFIG_PCI

#ifndef PCIBIOS_MIN_IO
#define PCIBIOS_MIN_IO	(0UL)
#endif

#ifndef PCIBIOS_MIN_MEM
#define PCIBIOS_MIN_MEM	(0UL)
#endif

#ifndef pcibios_assign_all_busses
#define pcibios_assign_all_busses()	(pci_has_flag(PCI_REASSIGN_ALL_BUS))
#endif

#ifndef pci_proc_domain
#define pci_proc_domain pci_proc_domain
static inline int pci_proc_domain(struct pci_bus *bus)
{
#ifdef CONFIG_PCI_DOMAINS_GENERIC
	return pci_domain_nr(bus);
#else
	return 1;
#endif
}
#endif

extern int isa_dma_bridge_buggy;

#else

#define isa_dma_bridge_buggy	(0)

#endif /* CONFIG_PCI */

#endif /* _ASM_GENERIC_PCI_H */
