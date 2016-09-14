/**
 * pci-epc.h - PCI Endpoint *Controller* (EPC) header file
 *
 * Copyright (C) 2016 Texas Instruments
 * Author: Kishon Vijay Abraham I <kishon@ti.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 of
 * the License as published by the Free Software Foundation.
 */

#ifndef __DRIVERS_PCI_EPC_H
#define __DRIVERS_PCI_EPC_H

#include <linux/pci-epf.h>

struct pci_epc;

enum pci_epc_irq_type {
	PCI_EPC_IRQ_UNKNOWN,
	PCI_EPC_IRQ_LEGACY,
	PCI_EPC_IRQ_MSI,
};

/**
 * struct pci_epc_ops - set of function pointers for performing EPC operations
 * @write_header: ops to populate configuration space header
 * @set_bar: ops to configure the BAR
 * @clear_bar: ops to reset the BAR
 * @alloc_addr_space: ops to allocate *in* PCI controller address space
 * @free_addr_space: ops to free the allocated address space
 * @raise_irq: ops to raise a legacy or MSI interrupt
 * @start: ops to start the PCI link
 * @stop: ops to stop the PCI link
 * @owner: the module owner containing the ops
 */
struct pci_epc_ops {
	int	(*write_header)(struct pci_epc *pci_epc,
				struct pci_epf_header *hdr);
	int	(*set_bar)(struct pci_epc *epc, enum pci_barno bar,
			   dma_addr_t bar_phys, size_t size, int flags);
	void	(*clear_bar)(struct pci_epc *epc, enum pci_barno bar);
	void	*(*alloc_addr_space)(struct pci_epc *pci_epc, size_t size);
	void	(*free_addr_space)(struct pci_epc *pci_epc);
	int	(*raise_irq)(struct pci_epc *pci_epc,
			     enum pci_epc_irq_type type);
	int	(*start)(struct pci_epc *epc);
	void	(*stop)(struct pci_epc *epc);
	struct module *owner;
};

/**
 * struct pci_epc - represents the PCI EPC device
 * @dev: PCI EPC device
 * @ops: function pointers for performing endpoint operations
 * @mutex: mutex to protect pci_epc ops
 */
struct pci_epc {
	struct device			dev;
	/* support only single function PCI device for now */
	struct pci_epf			*epf;
	const struct pci_epc_ops	*ops;
	spinlock_t			irq_lock;
};

#define to_pci_epc(device) container_of((device), struct pci_epc, dev)

#define pci_epc_create(dev, ops)    \
		__pci_epc_create((dev), (ops), THIS_MODULE)
#define devm_pci_epc_create(dev, ops)    \
		__devm_pci_epc_create((dev), (ops), THIS_MODULE)

static inline void epc_set_drvdata(struct pci_epc *epc, void *data)
{
	dev_set_drvdata(&epc->dev, data);
}

static inline void *epc_get_drvdata(struct pci_epc *epc)
{
	return dev_get_drvdata(&epc->dev);
}

struct pci_epc *
__devm_pci_epc_create(struct device *dev, const struct pci_epc_ops *ops,
		      struct module *owner);
struct pci_epc *
__pci_epc_create(struct device *dev, const struct pci_epc_ops *ops,
		 struct module *owner);
void devm_pci_epc_destroy(struct device *dev, struct pci_epc *epc);
void pci_epc_destroy(struct pci_epc *epc);
int pci_epc_write_header(struct pci_epc *epc, struct pci_epf_header *hdr);
int pci_epc_set_bar(struct pci_epc *epc, enum pci_barno bar,
		    dma_addr_t bar_phys, size_t size, int flags);
void pci_epc_clear_bar(struct pci_epc *epc, int bar);
int pci_epc_raise_irq(struct pci_epc *epc, enum pci_epc_irq_type type);
int pci_epc_start(struct pci_epc *epc);
void pci_epc_stop(struct pci_epc *epc);
int pci_epc_bind_epf(struct pci_epf *epf);
void pci_epc_unbind_epf(struct pci_epf *epf);
#endif /* __DRIVERS_PCI_EPC_H */
