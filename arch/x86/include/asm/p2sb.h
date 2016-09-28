/*
 * Primary to Sideband bridge (P2SB) access support
 */

#ifndef P2SB_SYMS_H
#define P2SB_SYMS_H

#include <linux/ioport.h>
#include <linux/pci.h>

#if IS_ENABLED(CONFIG_P2SB)

int p2sb_bar(struct pci_dev *pdev, unsigned int devfn,
	struct resource *res);

#else /* CONFIG_P2SB is not set */

static inline
int p2sb_bar(struct pci_dev *pdev, unsigned int devfn,
	struct resource *res)
{
	return -ENODEV;
}

#endif /* CONFIG_P2SB */

#endif /* P2SB_SYMS_H */
