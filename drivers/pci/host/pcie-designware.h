/*
 * Synopsys Designware PCIe host controller driver
 *
 * Copyright (C) 2013 Samsung Electronics Co., Ltd.
 *		http://www.samsung.com
 *
 * Author: Jingoo Han <jg1.han@samsung.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#ifndef _PCIE_DESIGNWARE_H
#define _PCIE_DESIGNWARE_H

/*
 * Maximum number of MSI IRQs can be 256 per controller. But keep
 * it 32 as of now. Probably we will never need more than 32. If needed,
 * then increment it in multiple of 32.
 */
#define MAX_MSI_IRQS			32
#define MAX_MSI_CTRLS			(MAX_MSI_IRQS / 32)

#define LTSSM_STATE_DETECT_QUIET	0x00
#define LTSSM_STATE_DETECT_ACT		0x01
#define LTSSM_STATE_POLL_ACTIVE		0x02
#define LTSSM_STATE_POLL_COMPLIANCE	0x03
#define LTSSM_STATE_POLL_CONFIG		0x04
#define LTSSM_STATE_PRE_DETECT_QUIET	0x05
#define LTSSM_STATE_DETECT_WAIT		0x06
#define LTSSM_STATE_CFG_LINKWD_START	0x07
#define LTSSM_STATE_CFG_LINKWD_ACEPT	0x08
#define LTSSM_STATE_CFG_LANENUM_WAIT	0x09
#define LTSSM_STATE_CFG_LANENUM_ACEPT	0x0a
#define LTSSM_STATE_CFG_COMPLETE	0x0b
#define LTSSM_STATE_CFG_IDLE		0x0c
#define LTSSM_STATE_RCVRY_LOCK		0x0d
#define LTSSM_STATE_RCVRY_SPEED		0x0e
#define LTSSM_STATE_RCVRY_RCVRCFG	0x0f
#define LTSSM_STATE_RCVRY_IDLE		0x10
#define LTSSM_STATE_L0			0x11
#define LTSSM_STATE_L0S			0x12
#define LTSSM_STATE_L123_SEND_EIDLE	0x13
#define LTSSM_STATE_L1_IDLE		0x14
#define LTSSM_STATE_L2_IDLE		0x15
#define LTSSM_STATE_L2_WAKE		0x16
#define LTSSM_STATE_DISABLED_ENTRY	0x17
#define LTSSM_STATE_DISABLED_IDLE	0x18
#define LTSSM_STATE_DISABLED		0x19
#define LTSSM_STATE_LPBK_ENTRY		0x1a
#define LTSSM_STATE_LPBK_ACTIVE		0x1b
#define LTSSM_STATE_LPBK_EXIT		0x1c
#define LTSSM_STATE_LPBK_EXIT_TIMEOUT	0x1d
#define LTSSM_STATE_HOT_RESET_ENTRY	0x1e
#define LTSSM_STATE_HOT_RESET		0x1f
#define LTSSM_STATE_MASK		0x1f

struct pcie_port {
	struct device		*dev;
	u8			root_bus_nr;
	void __iomem		*dbi_base;
	u64			cfg0_base;
	u64			cfg0_mod_base;
	void __iomem		*va_cfg0_base;
	u32			cfg0_size;
	u64			cfg1_base;
	u64			cfg1_mod_base;
	void __iomem		*va_cfg1_base;
	u32			cfg1_size;
	u64			io_base;
	u64			io_mod_base;
	phys_addr_t		io_bus_addr;
	u32			io_size;
	u64			mem_base;
	u64			mem_mod_base;
	phys_addr_t		mem_bus_addr;
	u32			mem_size;
	struct resource		cfg;
	struct resource		io;
	struct resource		mem;
	struct resource		busn;
	int			irq;
	u32			lanes;
	struct pcie_host_ops	*ops;
	int			msi_irq;
	struct irq_domain	*irq_domain;
	unsigned long		msi_data;
	DECLARE_BITMAP(msi_irq_in_use, MAX_MSI_IRQS);
};

struct pcie_host_ops {
	void (*readl_rc)(struct pcie_port *pp,
			void __iomem *dbi_base, u32 *val);
	void (*writel_rc)(struct pcie_port *pp,
			u32 val, void __iomem *dbi_base);
	int (*rd_own_conf)(struct pcie_port *pp, int where, int size, u32 *val);
	int (*wr_own_conf)(struct pcie_port *pp, int where, int size, u32 val);
	int (*rd_other_conf)(struct pcie_port *pp, struct pci_bus *bus,
			unsigned int devfn, int where, int size, u32 *val);
	int (*wr_other_conf)(struct pcie_port *pp, struct pci_bus *bus,
			unsigned int devfn, int where, int size, u32 val);
	int (*link_up)(struct pcie_port *pp);
	void (*host_init)(struct pcie_port *pp);
	void (*msi_set_irq)(struct pcie_port *pp, int irq);
	void (*msi_clear_irq)(struct pcie_port *pp, int irq);
	phys_addr_t (*get_msi_addr)(struct pcie_port *pp);
	u32 (*get_msi_data)(struct pcie_port *pp, int pos);
	void (*scan_bus)(struct pcie_port *pp);
	int (*msi_host_init)(struct pcie_port *pp, struct msi_controller *chip);
};

int dw_pcie_cfg_read(void __iomem *addr, int size, u32 *val);
int dw_pcie_cfg_write(void __iomem *addr, int size, u32 val);
irqreturn_t dw_handle_msi_irq(struct pcie_port *pp);
void dw_pcie_msi_init(struct pcie_port *pp);
int dw_pcie_link_up(struct pcie_port *pp);
void dw_pcie_setup_rc(struct pcie_port *pp);
int dw_pcie_host_init(struct pcie_port *pp);

#endif /* _PCIE_DESIGNWARE_H */
