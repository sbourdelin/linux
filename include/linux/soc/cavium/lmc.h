#ifndef _LMC_H
#define _LMC_H

#include <linux/pci.h>

int thunderx_edac_lmc_probe(struct pci_dev *pdev, const struct pci_device_id *ent);
void thunderx_edac_lmc_remove(struct pci_dev *pdev);

#endif
