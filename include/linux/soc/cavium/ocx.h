#ifndef _OCX_H
#define _OCX_H

#include <linux/pci.h>

int thunderx_edac_ocx_probe(struct pci_dev *pdev, const struct pci_device_id *ent);
void thunderx_edac_ocx_remove(struct pci_dev *pdev);

#endif
