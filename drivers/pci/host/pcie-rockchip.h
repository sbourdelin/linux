/*
 * Rockchip AXI PCIe host controller driver
 *
 * Copyright (c) 2016 Rockchip, Inc.
 *
 * Author: Shawn Lin <shawn.lin@rock-chips.com>
 *            Wenrui Li <wenrui.li@rock-chips.com>
 *
 * Bits taken from Synopsys Designware Host controller driver and
 * ARM PCI Host generic driver.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef _PCIE_ROCKCHIP_H
#define _PCIE_ROCKCHIP_H

#define PCIE_CLIENT_BASE		0x0
#define PCIE_RC_CONFIG_NORMAL_BASE      0x800000
#define PCIE_RC_CONFIG_BASE		0xa00000
#define PCIE_CORE_LINK_CTRL_STATUS      0x8000d0
#define PCIE_CORE_CTRL_MGMT_BASE	0x900000
#define PCIE_CORE_AXI_CONF_BASE		0xc00000
#define PCIE_CORE_AXI_INBOUND_BASE	0xc00800

#define PCIE_CLIENT_BASIC_STATUS0	0x44
#define PCIE_CLIENT_BASIC_STATUS1	0x48
#define PCIE_CLIENT_INT_MASK		0x4c
#define PCIE_CLIENT_INT_STATUS		0x50
#define PCIE_RC_CONFIG_RID_CCR          0x8
#define PCIR_RC_CONFIG_LCS              0xd0
#define PCIE_RC_BAR_CONF                0x300
#define PCIE_CORE_OB_REGION_ADDR1       0x4
#define PCIE_CORE_OB_REGION_DESC0       0x8
#define PCIE_CORE_OB_REGION_DESC1       0xc
#define PCIE_RP_IB_ADDR_TRANS           0x4
#define PCIE_CORE_INT_MASK		0x900210
#define PCIE_CORE_INT_STATUS		0x90020c

/* Size of one AXI Region (not Region 0) */
#define	AXI_REGION_SIZE			BIT(20)
/* Size of Region 0, equal to sum of sizes of other regions */
#define	AXI_REGION_0_SIZE              (32 * (0x1 << 20))
#define OB_REG_SIZE_SHIFT               5
#define IB_ROOT_PORT_REG_SIZE_SHIFT     3

#define AXI_WRAPPER_IO_WRITE            0x6
#define AXI_WRAPPER_MEM_WRITE           0x2
#define MAX_AXI_IB_ROOTPORT_REGION_NUM  3
#define	MIN_AXI_ADDR_BITS_PASSED        8
#define ROCKCHIP_VENDOR_ID              0x1d87

#define PCIE_ECAM_BUS(x)		(((x) & 0xFF) << 20)
#define PCIE_ECAM_DEV(x)		(((x) & 0x1F) << 15)
#define PCIE_ECAM_FUNC(x)		(((x) & 0x7) << 12)
#define PCIE_ECAM_REG(x)		(((x) & 0xFFF) << 0)
#define PCIE_ECAM_ADDR(bus, dev, func, reg) \
	  (PCIE_ECAM_BUS(bus) | PCIE_ECAM_DEV(dev) | \
	   PCIE_ECAM_FUNC(func) | PCIE_ECAM_REG(reg))

/*
  * The higher 16-bit of this register is used for write protection
  * only if BIT(x + 16) set to 1 the BIT(x) can be written.
  */
#define HIWORD_UPDATE(val, mask, shift) \
	((val) << (shift) | (mask) << ((shift) + 16))

#define RC_REGION_0_ADDR_TRANS_H	0x00000000
#define RC_REGION_0_ADDR_TRANS_L	0x00000000
#define RC_REGION_0_PASS_BITS		(25 - 1)
#define RC_REGION_1_ADDR_TRANS_H	0x00000000
#define RC_REGION_1_ADDR_TRANS_L	0x00400000
#define RC_REGION_1_PASS_BITS		(20 - 1)
#define MAX_AXI_WRAPPER_REGION_NUM	33
#define PCIE_CORE_LCSR_RETAIN_LINK      BIT(5)
#define PCIE_CLIENT_CONF_ENABLE         1
#define PCIE_CLIENT_LINK_TRAIN_ENABLE   1
#define PCIE_CLIENT_ARI_ENABLE          1
#define PCIE_CLIENT_CONF_LANE_NUM(x)    (x / 2)
#define PCIE_CLIENT_MODE_RC             1
#define PCIE_CLIENT_GEN_SEL_2           1
#define PCIE_CLIENT_GEN_SEL_1           0
#define PCIE_CLIENT_CONF_ENABLE_SHIFT   0
#define PCIE_CLIENT_CONF_ENABLE_MASK    0x1
#define PCIE_CLIENT_LINK_TRAIN_SHIFT    1
#define PCIE_CLIENT_LINK_TRAIN_MASK     0x1
#define PCIE_CLIENT_ARI_ENABLE_SHIFT    3
#define PCIE_CLIENT_ARI_ENABLE_MASK     0x1
#define PCIE_CLIENT_CONF_LANE_NUM_SHIFT 4
#define PCIE_CLIENT_CONF_LANE_NUM_MASK  0x3
#define PCIE_CLIENT_MODE_SHIFT          6
#define PCIE_CLIENT_MODE_MASK           0x1
#define PCIE_CLIENT_GEN_SEL_SHIFT       7
#define PCIE_CLIENT_GEN_SEL_MASK        0x1
#define PCIE_CLIENT_LINK_STATUS_UP      0x3
#define PCIE_CLIENT_LINK_STATUS_SHIFT   20
#define PCIE_CLIENT_LINK_STATUS_MASK    0x3
#define PCIE_CORE_PL_CONF_SPEED_25G     0x0
#define PCIE_CORE_PL_CONF_SPEED_50G     0x1
#define PCIE_CORE_PL_CONF_SPEED_80G     0x2
#define PCIE_CORE_PL_CONF_SPEED_SHIFT   3
#define PCIE_CORE_PL_CONF_SPEED_MASK    0x3
#define PCIE_CORE_PL_CONF_LANE_SHIFT    1
#define PCIE_CORE_PL_CONF_LANE_MASK     0x3
#define PCIE_CORE_RC_CONF_SCC_SHIFT     16

/* PCIE_CLIENT_INT_STATUS */
#define PCIE_CLIENT_INT_LEGACY_DONE     BIT(15)
#define PCIE_CLIENT_INT_MSG             BIT(14)
#define PCIE_CLIENT_INT_HOT_RST         BIT(13)
#define PCIE_CLIENT_INT_DPA             BIT(12)
#define PCIE_CLIENT_INT_FATAL_ERR       BIT(11)
#define PCIE_CLIENT_INT_NFATAL_ERR      BIT(10)
#define PCIE_CLIENT_INT_CORR_ERR        BIT(9)
#define PCIE_CLIENT_INT_INTD            BIT(8)
#define PCIE_CLIENT_INT_INTC            BIT(7)
#define PCIE_CLIENT_INT_INTB            BIT(6)
#define PCIE_CLIENT_INT_INTA            BIT(5)
#define PCIE_CLIENT_INT_LOCAL           BIT(4)
#define PCIE_CLIENT_INT_UDMA            BIT(3)
#define PCIE_CLIENT_INT_PHY             BIT(2)
#define PCIE_CLIENT_INT_HOT_PLUG        BIT(1)
#define PCIE_CLIENT_INT_PWR_STCG        BIT(0)
#define PCIE_CORE_INT_PRFPE             BIT(0)
#define PCIE_CORE_INT_CRFPE             BIT(1)
#define PCIE_CORE_INT_RRPE              BIT(2)
#define PCIE_CORE_INT_PRFO              BIT(3)
#define PCIE_CORE_INT_CRFO              BIT(4)
#define PCIE_CORE_INT_RT                BIT(5)
#define PCIE_CORE_INT_RTR               BIT(6)
#define PCIE_CORE_INT_PE                BIT(7)
#define PCIE_CORE_INT_MTR               BIT(8)
#define PCIE_CORE_INT_UCR               BIT(9)
#define PCIE_CORE_INT_FCE               BIT(10)
#define PCIE_CORE_INT_CT                BIT(11)
#define PCIE_CORE_INT_UTC               BIT(18)
#define PCIE_CORE_INT_MMVC              BIT(19)

#define ROCKCHIP_PCIE_RPIFR1_INTR_MASK	GENMASK(8, 5)
#define ROCKCHIP_PCIE_RPIFR1_INTR_SHIFT	5

#define PCIE_CORE_INT \
		(PCIE_CORE_INT_PRFPE | PCIE_CORE_INT_CRFPE | \
		 PCIE_CORE_INT_RRPE | PCIE_CORE_INT_CRFO | \
		 PCIE_CORE_INT_RT | PCIE_CORE_INT_RTR | \
		 PCIE_CORE_INT_PE | PCIE_CORE_INT_MTR | \
		 PCIE_CORE_INT_UCR | PCIE_CORE_INT_FCE | \
		 PCIE_CORE_INT_CT | PCIE_CORE_INT_UTC | \
		 PCIE_CORE_INT_MMVC)

#define PCIE_CLIENT_INT_SUBSYSTEM \
	(PCIE_CLIENT_INT_PWR_STCG | PCIE_CLIENT_INT_HOT_PLUG | \
	PCIE_CLIENT_INT_PHY | PCIE_CLIENT_INT_UDMA | \
	PCIE_CLIENT_INT_LOCAL)

#define PCIE_CLIENT_INT_LEGACY \
	(PCIE_CLIENT_INT_INTA | PCIE_CLIENT_INT_INTB | \
	PCIE_CLIENT_INT_INTC | PCIE_CLIENT_INT_INTD)

#define PCIE_CLIENT_INT_CLI \
	(PCIE_CLIENT_INT_CORR_ERR | PCIE_CLIENT_INT_NFATAL_ERR | \
	PCIE_CLIENT_INT_FATAL_ERR | PCIE_CLIENT_INT_DPA | \
	PCIE_CLIENT_INT_HOT_RST | PCIE_CLIENT_INT_MSG | \
	PCIE_CLIENT_INT_LEGACY_DONE | PCIE_CLIENT_INT_LEGACY)

struct rockchip_pcie_port {
	void __iomem *reg_base;
	void __iomem *apb_base;
	struct phy           *phy;
	struct reset_control *core_rst;
	struct reset_control *mgmt_rst;
	struct reset_control *mgmt_sticky_rst;
	struct reset_control *pipe_rst;
	struct clk *aclk_pcie;
	struct clk *aclk_perf_pcie;
	struct clk *hclk_pcie;
	struct clk *clk_pcie_pm;
	struct regulator *vpcie3v3; /* 3.3V power supply */
	struct regulator *vpcie1v8; /* 1.8V power supply */
	struct regulator *vpcie0v9; /* 0.9V power supply */
#define VPCIE_3V3    3300000
#define VPCIE_1V8    1800000
#define VPCIE_0V9    900000
	struct gpio_desc *ep_gpio;
	u32 lanes;
	resource_size_t		io_base;
	struct resource		*cfg;
	struct resource		*io;
	struct resource		*mem;
	struct resource		*busn;
	phys_addr_t		io_bus_addr;
	u32			io_size;
	phys_addr_t		mem_bus_addr;
	u32			mem_size;
	u8	root_bus_nr;
	int irq;
	struct msi_controller *msi;

	struct device *dev;
	struct irq_domain *irq_domain;
};

static irqreturn_t rockchip_pcie_subsys_irq_handler(int irq, void *arg);
static irqreturn_t rockchip_pcie_client_irq_handler(int irq, void *arg);
static void rockchip_pcie_legacy_int_handler(struct irq_desc *desc);

#endif
