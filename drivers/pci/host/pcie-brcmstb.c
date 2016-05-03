/*
 * Broadcom STB PCIe root complex driver
 *
 * Copyright (C) 2009 - 2016 Broadcom
 *
 * This file is licensed under the terms of the GNU General Public
 * License version 2.  This program is licensed "as is" without any
 * warranty of any kind, whether express or implied.
 */
#include <linux/init.h>
#include <linux/types.h>
#include <linux/slab.h>
#include <linux/pci.h>
#include <linux/kernel.h>
#include <linux/ioport.h>
#include <linux/compiler.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/clk.h>
#include <linux/interrupt.h>
#include <linux/list.h>
#include <linux/msi.h>
#include <linux/printk.h>
#include <linux/of_irq.h>
#include <linux/of_pci.h>
#include <linux/of_platform.h>
#include <linux/of_address.h>
#include <linux/of_pci.h>
#include <linux/module.h>
#include <linux/irqdomain.h>
#include <linux/regulator/consumer.h>
#include <linux/string.h>
#include <linux/sizes.h>

/* Broadcom PCIE Offsets */
#define PCIE_RC_CFG_PCIE_LINK_CAPABILITY		0x00b8
#define PCIE_RC_CFG_PCIE_LINK_STATUS_CONTROL		0x00bc
#define PCIE_RC_CFG_PCIE_ROOT_CAP_CONTROL		0x00c8
#define PCIE_RC_CFG_PCIE_LINK_STATUS_CONTROL_2		0x00dc
#define PCIE_RC_CFG_VENDOR_VENDOR_SPECIFIC_REG1		0x0188
#define PCIE_RC_CFG_PRIV1_ID_VAL3			0x043c
#define PCIE_RC_DL_MDIO_ADDR				0x1100
#define PCIE_RC_DL_MDIO_WR_DATA				0x1104
#define PCIE_RC_DL_MDIO_RD_DATA				0x1108
#define PCIE_MISC_MISC_CTRL				0x4008
#define PCIE_MISC_CPU_2_PCIE_MEM_WIN0_LO		0x400c
#define PCIE_MISC_CPU_2_PCIE_MEM_WIN0_HI		0x4010
#define PCIE_MISC_RC_BAR1_CONFIG_LO			0x402c
#define PCIE_MISC_RC_BAR1_CONFIG_HI			0x4030
#define PCIE_MISC_RC_BAR2_CONFIG_LO			0x4034
#define PCIE_MISC_RC_BAR2_CONFIG_HI			0x4038
#define PCIE_MISC_RC_BAR3_CONFIG_LO			0x403c
#define PCIE_MISC_RC_BAR3_CONFIG_HI			0x4040
#define PCIE_MISC_MSI_BAR_CONFIG_LO			0x4044
#define PCIE_MISC_MSI_BAR_CONFIG_HI			0x4048
#define PCIE_MISC_MSI_DATA_CONFIG			0x404c
#define PCIE_MISC_PCIE_CTRL				0x4064
#define PCIE_MISC_PCIE_STATUS				0x4068
#define PCIE_MISC_REVISION				0x406c
#define PCIE_MISC_CPU_2_PCIE_MEM_WIN0_BASE_LIMIT	0x4070
#define PCIE_MISC_HARD_PCIE_HARD_DEBUG			0x4204
#define PCIE_INTR2_CPU_BASE				0x4300
#define PCIE_MSI_INTR2_BASE				0x4500
#define BRCM_NUM_PCI_OUT_WINS		0x4
#define BRCM_MAX_SCB			0x4
#define BRCM_INT_PCI_MSI_NR		32
#define BRCM_PCIE_HW_REV_33		0x0303
#define BRCM_MSI_TARGET_ADDR_LO		0x0
#define BRCM_MSI_TARGET_ADDR_HI		0xffffffff

/* Offsets from PCIE_INTR2_CPU_BASE and PCIE_MSI_INTR2_BASE */
#define STATUS				0x0
#define SET				0x4
#define CLR				0x8
#define MASK_STATUS			0xc
#define MASK_SET			0x10
#define MASK_CLR			0x14

#define PCI_BUSNUM_SHIFT		20
#define PCI_SLOT_SHIFT			15
#define PCI_FUNC_SHIFT			12

/* Helper macros for reading registers varying from chip-to-chip */
#define IDX_ADDR(pcie)			((pcie->base) + \
					 pcie->reg_offsets[EXT_CFG_INDEX])
#define DATA_ADDR(pcie)			((pcie->base) + \
					 pcie->reg_offsets[EXT_CFG_DATA])
#define PCIE_RGR1_SW_INIT_1(pcie)	((pcie->base) + \
					 pcie->reg_offsets[RGR1_SW_INIT_1])

enum {
	RGR1_SW_INIT_1,
	EXT_CFG_INDEX,
	EXT_CFG_DATA,
};

enum pcie_type {
	BCM7425,
	BCM7435,
	GENERIC,
};

struct pcie_cfg_data {
	const int *offsets;
	const enum pcie_type type;
};

static const int pcie_offset_bcm7425[] = {
	[RGR1_SW_INIT_1] = 0x8010,
	[EXT_CFG_INDEX]  = 0x8300,
	[EXT_CFG_DATA]   = 0x8304,
};

static const struct pcie_cfg_data bcm7425_cfg = {
	.offsets	= pcie_offset_bcm7425,
	.type		= BCM7425,
};

static const int pcie_offsets[] = {
	[RGR1_SW_INIT_1] = 0x9210,
	[EXT_CFG_INDEX]  = 0x9000,
	[EXT_CFG_DATA]   = 0x9004,
};

static const struct pcie_cfg_data bcm7435_cfg = {
	.offsets	= pcie_offsets,
	.type		= BCM7435,
};

static const struct pcie_cfg_data generic_cfg = {
	.offsets	= pcie_offsets,
	.type		= GENERIC,
};

struct brcm_msi {
	struct irq_domain *domain;
	struct irq_chip irq_chip;
	struct msi_controller chip;
	struct mutex lock;
	int irq;
	/* intr_base is the base pointer for interrupt status/set/clr regs */
	void __iomem *intr_base;
	/* intr_legacy_mask indicates how many bits are MSI interrupts */
	u32 intr_legacy_mask;
	/* intr_legacy_offset indicates bit position of MSI_01 */
	u32 intr_legacy_offset;
	/* used indicates which MSI interrupts have been alloc'd */
	unsigned long used;
	/* working indicates that on boot we have brought up MSI */
	bool working;
};

struct brcm_window {
	u64 size;
	u64 cpu_addr;
	struct resource pcie_iomem_res;
};

struct brcm_dev_pwr_supply {
	struct list_head node;
	char name[32];
	struct regulator *regulator;
};

/* Internal Bus Controller Information.*/
struct brcm_pcie {
	struct list_head	list;
	void __iomem		*base;
	char			name[8];
	bool			suspended;
	struct clk		*clk;
	struct device_node	*dn;
	int			pcie_irq[4];
	int			irq;
	int			num_out_wins;
	bool			ssc;
	int			gen;
	int			scb_size_vals[BRCM_MAX_SCB];
	struct brcm_window	out_wins[BRCM_NUM_PCI_OUT_WINS];
	struct pci_bus		*bus;
	struct device		*dev;
	struct list_head	resource;
	struct list_head	pwr_supplies;
	struct brcm_msi		msi;
	unsigned int		rev;
	unsigned int		num;
	bool			bridge_setup_done;
	const int		*reg_offsets;
	enum pcie_type		type;
};

static int brcm_num_pci_controllers;
static int num_memc;

#if defined(__BIG_ENDIAN)
#define	DATA_ENDIAN		2	/* PCI->DDR inbound accesses */
#define MMIO_ENDIAN		2	/* CPU->PCI outbound accesses */
#else
#define	DATA_ENDIAN		0
#define MMIO_ENDIAN		0
#endif

/*
 * MIPS endianness is configured by boot strap, which also reverses all
 * bus endianness (i.e., big-endian CPU + big endian bus ==> native
 * endian I/O).
 *
 * Other architectures (e.g., ARM) either do not support big endian, or
 * else leave I/O in little endian mode.
 */
static inline u32 bpcie_readl(void __iomem *base)
{
	if (IS_ENABLED(CONFIG_MIPS) && IS_ENABLED(CONFIG_CPU_BIG_ENDIAN))
		return __raw_readl(base);
	else
		return readl_relaxed(base);
}

static inline void bpcie_writel(u32 val, void __iomem *base)
{
	if (IS_ENABLED(CONFIG_MIPS) && IS_ENABLED(CONFIG_CPU_BIG_ENDIAN))
		__raw_writel(val, base);
	else
		writel_relaxed(val, base);
}

/* negative return value indicates error */
static int mdio_read(void __iomem *base, u8 phyad, u8 regad)
{
	u32 data = ((phyad & 0xf) << 16)
		| (regad & 0x1f)
		| 0x100000;

	bpcie_writel(data, base + PCIE_RC_DL_MDIO_ADDR);
	bpcie_readl(base + PCIE_RC_DL_MDIO_ADDR);

	data = bpcie_readl(base + PCIE_RC_DL_MDIO_RD_DATA);
	if (!(data & 0x80000000)) {
		mdelay(1);
		data = bpcie_readl(base + PCIE_RC_DL_MDIO_RD_DATA);
	}
	return (data & 0x80000000) ? (data & 0xffff) : -EIO;
}

/* negative return value indicates error */
static int mdio_write(void __iomem *base, u8 phyad, u8 regad, u16 wrdata)
{
	u32 data = ((phyad & 0xf) << 16) | (regad & 0x1f);

	bpcie_writel(data, base + PCIE_RC_DL_MDIO_ADDR);
	bpcie_readl(base + PCIE_RC_DL_MDIO_ADDR);

	bpcie_writel(0x80000000 | wrdata, base + PCIE_RC_DL_MDIO_WR_DATA);
	data = bpcie_readl(base + PCIE_RC_DL_MDIO_WR_DATA);
	if (!(data & 0x80000000)) {
		mdelay(1);
		data = bpcie_readl(base + PCIE_RC_DL_MDIO_WR_DATA);
	}
	return (data & 0x80000000) ? 0 : -EIO;
}


static void wr_fld(void __iomem *p, u32 mask, int shift, u32 val)
{
	u32 reg;

	reg = bpcie_readl(p);
	reg = (reg & ~mask) | (val << shift);
	bpcie_writel(reg, p);
}

static void wr_fld_rb(void __iomem *p, u32 mask, int shift, u32 val)
{
	wr_fld(p, mask, shift, val);
	(void) bpcie_readl(p);
}

/* configures device for ssc mode; negative return value indicates error */
static int set_ssc(void __iomem *base)
{
	int tmp;
	u16 wrdata;

	tmp = mdio_write(base, 0, 0x1f, 0x1100);
	if (tmp < 0)
		return tmp;

	tmp = mdio_read(base, 0, 2);
	if (tmp < 0)
		return tmp;

	wrdata = ((u16)tmp & 0x3fff) | 0xc000;
	tmp = mdio_write(base, 0, 2, wrdata);
	if (tmp < 0)
		return tmp;

	mdelay(1);
	tmp = mdio_read(base, 0, 1);
	if (tmp < 0)
		return tmp;

	return 0;
}


/* returns 0 if in ssc mode, 1 if not, <0 on error */
static int is_ssc(void __iomem *base)
{
	int tmp = mdio_write(base, 0, 0x1f, 0x1100);

	if (tmp < 0)
		return tmp;
	tmp = mdio_read(base, 0, 1);
	if (tmp < 0)
		return tmp;
	return (tmp & 0xc00) == 0xc00 ? 0 : 1;
}


/* limits operation to a specific generation (1, 2, or 3) */
static void set_gen(void __iomem *base, int gen)
{
	wr_fld(base + PCIE_RC_CFG_PCIE_LINK_CAPABILITY, 0xf, 0, gen);
	wr_fld(base + PCIE_RC_CFG_PCIE_LINK_STATUS_CONTROL_2, 0xf, 0, gen);
}


static void set_pcie_outbound_win(void __iomem *base, unsigned int win,
				  u64 start, u64 len)
{
	u32 tmp;

	bpcie_writel((u32)(start) + MMIO_ENDIAN,
		     base + PCIE_MISC_CPU_2_PCIE_MEM_WIN0_LO+(win*8));
	bpcie_writel((u32)(start >> 32),
		     base + PCIE_MISC_CPU_2_PCIE_MEM_WIN0_HI+(win*8));
	tmp = ((((u32)start) >> 20) << 4)
		| (((((u32)start) + ((u32)len) - 1) >> 20) << 20);
	bpcie_writel(tmp,
		     base + PCIE_MISC_CPU_2_PCIE_MEM_WIN0_BASE_LIMIT+(win*4));
}


static int is_pcie_link_up(struct brcm_pcie *pcie)
{
	void __iomem *base = pcie->base;
	u32 val = bpcie_readl(base + PCIE_MISC_PCIE_STATUS);

	return  ((val & 0x30) == 0x30) ? 1 : 0;
}


static inline struct brcm_msi *to_brcm_msi(struct msi_controller *chip)
{
	return container_of(chip, struct brcm_msi, chip);
}


static int brcm_msi_alloc(struct brcm_msi *chip)
{
	int msi;

	mutex_lock(&chip->lock);
	msi = ~chip->used ? ffz(chip->used) : -1;

	if (msi >= 0 && msi < BRCM_INT_PCI_MSI_NR)
		chip->used |= (1 << msi);
	else
		msi = -ENOSPC;

	mutex_unlock(&chip->lock);
	return msi;
}


static void brcm_msi_free(struct brcm_msi *chip, unsigned long irq)
{
	mutex_lock(&chip->lock);
	chip->used &= ~(1 << irq);
	mutex_unlock(&chip->lock);
}


static irqreturn_t brcm_pcie_msi_irq(int irq, void *data)
{
	struct brcm_pcie *pcie = data;
	struct brcm_msi *msi = &pcie->msi;
	unsigned long status;

	status = bpcie_readl(msi->intr_base + STATUS) & msi->intr_legacy_mask;

	if (!status)
		return IRQ_NONE;

	while (status) {
		unsigned int index = ffs(status) - 1;
		unsigned int irq;

		/* clear the interrupt */
		bpcie_writel(1 << index, msi->intr_base + CLR);
		status &= ~(1 << index);

		/* Account for legacy interrupt offset */
		index -= msi->intr_legacy_offset;

		irq = irq_find_mapping(msi->domain, index);
		if (irq) {
			if (msi->used & (1 << index))
				generic_handle_irq(irq);
			else
				dev_info(pcie->dev, "unhandled MSI %d\n",
					 index);
		} else {
			/* Unknown MSI, just clear it */
			dev_dbg(pcie->dev, "unexpected MSI\n");
		}
	}
	return IRQ_HANDLED;
}


static int brcm_msi_setup_irq(struct msi_controller *chip, struct pci_dev *pdev,
			      struct msi_desc *desc)
{
	struct brcm_msi *msi = to_brcm_msi(chip);
	struct brcm_pcie *pcie = container_of(chip, struct brcm_pcie, msi.chip);
	struct msi_msg msg;
	unsigned int irq;
	int hwirq;
	u32 data;

	hwirq = brcm_msi_alloc(msi);
	if (hwirq < 0)
		return hwirq;

	irq = irq_create_mapping(msi->domain, hwirq);
	if (!irq) {
		brcm_msi_free(msi, hwirq);
		return -EINVAL;
	}

	irq_set_msi_desc(irq, desc);

	msg.address_lo = BRCM_MSI_TARGET_ADDR_LO;
	msg.address_hi = BRCM_MSI_TARGET_ADDR_HI;
	data = bpcie_readl(pcie->base + PCIE_MISC_MSI_DATA_CONFIG);
	msg.data = ((data >> 16) & (data & 0xffff)) | hwirq;
	wmb(); /* just being cautious */
	write_msi_msg(irq, &msg);

	return 0;
}


static void brcm_msi_teardown_irq(struct msi_controller *chip, unsigned int irq)
{
	struct brcm_msi *msi = to_brcm_msi(chip);
	struct irq_data *d = irq_get_irq_data(irq);

	brcm_msi_free(msi, d->hwirq);
}


static int brcm_msi_map(struct irq_domain *domain, unsigned int irq,
			irq_hw_number_t hwirq)
{
	struct brcm_pcie *pcie = domain->host_data;

	irq_set_chip_and_handler(irq, &pcie->msi.irq_chip, handle_simple_irq);
	irq_set_chip_data(irq, domain->host_data);

	return 0;
}

static const struct irq_domain_ops msi_domain_ops = {
	.map = brcm_msi_map,
};

static int brcm_pcie_enable_msi(struct brcm_pcie *pcie, int nr)
{
	static const char brcm_msi_name[] = "brcmstb_pcieX_msi";
	struct brcm_msi *msi = &pcie->msi;
	u32 data_val;
	char *name;
	int err;

	if (!pcie->suspended) {
		/* We are only here on cold boot */
		mutex_init(&msi->lock);

		msi->chip.dev = pcie->dev;
		msi->chip.setup_irq = brcm_msi_setup_irq;
		msi->chip.teardown_irq = brcm_msi_teardown_irq;

		/* We have multiple RC controllers.  We may have as many
		 * MSI controllers for them.  We want each to have a
		 * unique name, so we go to the trouble of having an
		 * irq_chip per RC (instead of one for all of them).
		 */
		name = devm_kzalloc(pcie->dev, sizeof(brcm_msi_name),
				    GFP_KERNEL);
		if (name) {
			char *p;

			strcpy(name, brcm_msi_name);
			p = strchr(name, 'X');
			if (p)
				*p = '0' + nr;
			msi->irq_chip.name = name;
		} else {
			msi->irq_chip.name = brcm_msi_name;
		}

		msi->irq_chip.irq_enable = unmask_msi_irq;
		msi->irq_chip.irq_disable = mask_msi_irq;
		msi->irq_chip.irq_mask = mask_msi_irq;
		msi->irq_chip.irq_unmask = unmask_msi_irq;

		msi->domain =
			irq_domain_add_linear(pcie->dn, BRCM_INT_PCI_MSI_NR,
					      &msi_domain_ops, pcie);
		if (!msi->domain) {
			dev_err(pcie->dev,
				"failed to create IRQ domain for MSI\n");
			return -ENOMEM;
		}

		err = devm_request_irq(pcie->dev, msi->irq, brcm_pcie_msi_irq,
				       IRQF_SHARED, msi->irq_chip.name,
				       pcie);

		if (err < 0) {
			dev_err(pcie->dev,
				"failed to request IRQ (%d) for MSI\n",	err);
			goto msi_en_err;
		}

		if (pcie->rev >= BRCM_PCIE_HW_REV_33) {
			msi->intr_base = pcie->base + PCIE_MSI_INTR2_BASE;
			/* This version of PCIe hw has only 32 intr bits
			 * starting at bit position 0.
			 */
			msi->intr_legacy_mask = 0xffffffff;
			msi->intr_legacy_offset = 0x0;
			msi->used = 0x0;

		} else {
			msi->intr_base = pcie->base + PCIE_INTR2_CPU_BASE;
			/* This version of PCIe hw has only 8 intr bits starting
			 * at bit position 24.
			 */
			msi->intr_legacy_mask = 0xff000000;
			msi->intr_legacy_offset = 24;
			msi->used = 0xffffff00;
		}
		msi->working = true;
	}

	/* If we are here, and msi->working is false, it means that we've
	 * already tried and failed to bring up MSI.  Just return 0
	 * since there is nothing to be done.
	 */
	if (!msi->working)
		return 0;

	if (pcie->rev >= BRCM_PCIE_HW_REV_33) {
		/* ffe0 -- least sig 5 bits are 0 indicating 32 msgs
		 * 6540 -- this is our arbitrary unique data value
		 */
		data_val = 0xffe06540;
	} else {
		/* fff8 -- least sig 3 bits are 0 indicating 8 msgs
		 * 6540 -- this is our arbitrary unique data value
		 */
		data_val = 0xfff86540;
	}

	/* Make sure we are not masking MSIs.  Note that MSIs can be masked,
	 * but that occurs on the PCIe EP device
	 */
	bpcie_writel(0xffffffff & msi->intr_legacy_mask,
		     msi->intr_base + MASK_CLR);

	/* The 0 bit of BRCM_MSI_TARGET_ADDR_LO is repurposed to MSI enable,
	 * which we set to 1.
	 */
	bpcie_writel(BRCM_MSI_TARGET_ADDR_LO | 1, pcie->base
		     + PCIE_MISC_MSI_BAR_CONFIG_LO);
	bpcie_writel(BRCM_MSI_TARGET_ADDR_HI, pcie->base
		     + PCIE_MISC_MSI_BAR_CONFIG_HI);
	bpcie_writel(data_val, pcie->base + PCIE_MISC_MSI_DATA_CONFIG);

	return 0;

msi_en_err:
	irq_domain_remove(msi->domain);
	return err;
}


static void brcm_pcie_setup_early(struct brcm_pcie *pcie)
{
	void __iomem *base = pcie->base;
	unsigned int scb_size_val;
	int i;

	/* reset the bridge and the endpoint device
	 * field: PCIE_BRIDGE_SW_INIT = 1
	 */
	wr_fld_rb(PCIE_RGR1_SW_INIT_1(pcie), 0x00000002, 1, 1);

	/* field: PCIE_SW_PERST = 1 */
	wr_fld_rb(PCIE_RGR1_SW_INIT_1(pcie), 0x00000001, 0, 1);

	/* delay 100us */
	udelay(100);

	/* take the bridge out of reset
	 * field: PCIE_BRIDGE_SW_INIT = 0
	 */
	wr_fld_rb(PCIE_RGR1_SW_INIT_1(pcie), 0x00000002, 1, 0);

	/* Grab the PCIe hw revision number */
	pcie->rev = bpcie_readl(base + PCIE_MISC_REVISION) & 0xffff;

	/* enable SCB_MAX_BURST_SIZE | CSR_READ_UR_MODE | SCB_ACCESS_EN */
	if (pcie->type == GENERIC)
		bpcie_writel(0x81e03000, base + PCIE_MISC_MISC_CTRL);
	else
		bpcie_writel(0x00103000, base + PCIE_MISC_MISC_CTRL);

	for (i = 0; i < pcie->num_out_wins; i++) {
		struct brcm_window *w = &pcie->out_wins[i];

		set_pcie_outbound_win(base, i, w->cpu_addr, w->size);
	}

	/* set up 4GB PCIE->SCB memory window on BAR2 */
	bpcie_writel(0x00000011, base + PCIE_MISC_RC_BAR2_CONFIG_LO);
	bpcie_writel(0x00000000, base + PCIE_MISC_RC_BAR2_CONFIG_HI);

	/* field: SCB0_SIZE, default = 0xf (1 GB) */
	scb_size_val = pcie->scb_size_vals[0] ? pcie->scb_size_vals[0] : 0xf;
	wr_fld(base + PCIE_MISC_MISC_CTRL, 0xf8000000, 27, scb_size_val);

	/* field: SCB1_SIZE, default = 0xf (1 GB) */
	if (num_memc > 1) {
		scb_size_val = pcie->scb_size_vals[1]
			? pcie->scb_size_vals[1] : 0xf;
		wr_fld(base + PCIE_MISC_MISC_CTRL, 0x07c00000,
		       22, scb_size_val);
	}

	/* field: SCB2_SIZE, default = 0xf (1 GB) */
	if (num_memc > 2) {
		scb_size_val = pcie->scb_size_vals[2]
			? pcie->scb_size_vals[2] : 0xf;
		wr_fld(base + PCIE_MISC_MISC_CTRL, 0x0000001f,
		       0, scb_size_val);
	}

	/* disable the PCIE->GISB memory window */
	bpcie_writel(0x00000000, base + PCIE_MISC_RC_BAR1_CONFIG_LO);

	/* disable the PCIE->SCB memory window */
	bpcie_writel(0x00000000, base + PCIE_MISC_RC_BAR3_CONFIG_LO);

	if (!pcie->suspended) {
		/* clear any interrupts we find on boot */
		bpcie_writel(0xffffffff, base + PCIE_INTR2_CPU_BASE + CLR);
		(void) bpcie_readl(base + PCIE_INTR2_CPU_BASE + CLR);
	}

	/* Mask all interrupts since we are not handling any yet */
	bpcie_writel(0xffffffff, base + PCIE_INTR2_CPU_BASE + MASK_SET);
	(void) bpcie_readl(base + PCIE_INTR2_CPU_BASE + MASK_SET);

	if (pcie->ssc)
		if (set_ssc(base))
			dev_err(pcie->dev, "error while configuring ssc mode\n");
	if (pcie->gen)
		set_gen(base, pcie->gen);

	/* take the EP device out of reset */
	/* field: PCIE_SW_PERST = 0 */
	wr_fld_rb(PCIE_RGR1_SW_INIT_1(pcie), 0x00000001, 0, 0);
}

static void brcm_pcie_turn_off(struct brcm_pcie *pcie)
{
	void __iomem *base = pcie->base;
	/* Reset endpoint device */
	wr_fld_rb(PCIE_RGR1_SW_INIT_1(pcie), 0x00000001, 0, 1);
	/* deassert request for L23 in case it was asserted */
	wr_fld_rb(base + PCIE_MISC_PCIE_CTRL, 0x1, 0, 0);
	/* SERDES_IDDQ = 1 */
	wr_fld_rb(base + PCIE_MISC_HARD_PCIE_HARD_DEBUG, 0x08000000,
		  27, 1);
	/* Shutdown PCIe bridge */
	wr_fld_rb(PCIE_RGR1_SW_INIT_1(pcie), 0x00000002, 1, 1);
}

static void brcm_pcie_enter_l23(struct brcm_pcie *pcie)
{
	void __iomem *base = pcie->base;
	int timeout = 1000;
	int l23;

	/* assert request for L23 */
	wr_fld_rb(base + PCIE_MISC_PCIE_CTRL, 0x1, 0, 1);
	do {
		/* poll L23 status */
		l23 = bpcie_readl(base + PCIE_MISC_PCIE_STATUS) & (1 << 6);
	} while (--timeout && !l23);

	if (!timeout)
		dev_err(pcie->dev, "failed to enter L23\n");
}

static int brcm_setup_pcie_bridge(struct brcm_pcie *pcie)
{
	static const char *link_speed[4] = { "???", "2.5", "5.0", "8.0" };
	void __iomem *base = pcie->base;
	const int limit = pcie->suspended ? 1000 : 100;
	struct clk *clk;
	unsigned int status;
	int i, j, ret;
	bool ssc_good = false;

	/* Give the RC/EP time to wake up, before trying to configure RC.
	 * Intermittently check status for link-up, up to a total of 100ms
	 * when we don't know if the device is there, and up to 1000ms if
	 * we do know the device is there.
	 */
	for (i = 1, j = 0; j < limit && !is_pcie_link_up(pcie); j += i, i = i*2)
		mdelay(i + j > limit ? limit - j : i);

	if (!is_pcie_link_up(pcie)) {
		dev_info(pcie->dev, "link down\n");
		goto fail;
	}

	/* Attempt to enable MSI if we have an interrupt for it. */
	if (pcie->msi.irq > 0) {
		ret = brcm_pcie_enable_msi(pcie, pcie->num);
		if (ret < 0) {
			dev_err(pcie->dev, "failed to enable MSI support: %d\n",
				ret);
		}
	}

	/* For config space accesses on the RC, show the right class for
	 * a PCI-PCI bridge
	 */
	wr_fld_rb(base + PCIE_RC_CFG_PRIV1_ID_VAL3, 0x00ffffff, 0, 0x060400);

	status = bpcie_readl(base + PCIE_RC_CFG_PCIE_LINK_STATUS_CONTROL);

	if (pcie->ssc) {
		if (is_ssc(base) == 0)
			ssc_good = true;
		else
			dev_err(pcie->dev, "failed to enter SSC mode\n");
	}

	dev_info(pcie->dev, "link up, %s Gbps x%u %s\n",
		 link_speed[((status & 0x000f0000) >> 16) & 0x3],
		 (status & 0x03f00000) >> 20, ssc_good ? "(SSC)" : "(!SSC)");

	/* Enable configuration request retry (see pci_scan_device()) */
	/* field RC_CRS_EN = 1
	 */
	wr_fld(base + PCIE_RC_CFG_PCIE_ROOT_CAP_CONTROL, 0x00000010, 4, 1);

	/* PCIE->SCB endian mode for BAR ield ENDIAN_MODE_BAR2 = DATA_ENDIAN
	 */
	wr_fld_rb(base + PCIE_RC_CFG_VENDOR_VENDOR_SPECIFIC_REG1, 0x0000000c, 2,
		  DATA_ENDIAN);

	/* Refclk from RC should be gated with CLKREQ# input when ASPM L0s,L1
	 * is enabled =>  setting the CLKREQ_DEBUG_ENABLE field to 1.
	 */
	wr_fld_rb(base + PCIE_MISC_HARD_PCIE_HARD_DEBUG, 0x00000002, 1, 1);

	/* Add bogus IO resource structure so that pcibios_init_resources()
	 * does not allocate the same IO region for different domains
	 */

	pcie->bridge_setup_done = true;

	return 0;
fail:
	if (IS_ENABLED(CONFIG_PM))
		brcm_pcie_turn_off(pcie);

	clk = pcie->clk;
	if (pcie->suspended)
		clk_disable(clk);
	else {
		clk_disable_unprepare(clk);
		clk_put(clk);
	}

	pcie->bridge_setup_done = false;

	return -ENODEV;
}

#ifdef CONFIG_PM_SLEEP
static int brcm_pcie_suspend(struct device *dev)
{
	struct brcm_pcie *pcie = dev_get_drvdata(dev);
	struct brcm_dev_pwr_supply *supply;
	struct list_head *pos;
	int ret;

	if (!pcie->bridge_setup_done)
		return 0;

	brcm_pcie_enter_l23(pcie);
	brcm_pcie_turn_off(pcie);
	clk_disable(pcie->clk);
	list_for_each(pos, &pcie->pwr_supplies) {
		supply = list_entry(pos, struct brcm_dev_pwr_supply,
				    node);

		ret = regulator_disable(supply->regulator);
		if (ret)
			dev_dbg(dev, "Unable to turn off %s supply\n",
				supply->name);

	}
	pcie->suspended = true;

	return 0;
}

static int brcm_pcie_resume(struct device *dev)
{
	struct brcm_pcie *pcie = dev_get_drvdata(dev);
	struct brcm_dev_pwr_supply *supply;
	struct list_head *pos;
	void __iomem *base;
	int ret;

	if (!pcie->bridge_setup_done)
		return 0;

	base = pcie->base;
	list_for_each(pos, &pcie->pwr_supplies) {
		supply = list_entry(pos, struct brcm_dev_pwr_supply,
				    node);
		ret = regulator_enable(supply->regulator);
		if (ret)
			dev_dbg(dev, "Unable to turn on %s supply\n",
				supply->name);
	}
	clk_enable(pcie->clk);

	/* Take bridge out of reset so we can access the SERDES reg */
	wr_fld_rb(PCIE_RGR1_SW_INIT_1(pcie), 0x00000002, 1, 0);

	/* SERDES_IDDQ = 0 */
	wr_fld_rb(base + PCIE_MISC_HARD_PCIE_HARD_DEBUG, 0x08000000,
		  27, 0);
	/* wait for serdes to be stable */
	udelay(100);

	brcm_pcie_setup_early(pcie);

	brcm_setup_pcie_bridge(pcie);
	pcie->suspended = false;

	return 0;
}

static const struct dev_pm_ops brcm_pcie_pm_ops = {
	.suspend_noirq = brcm_pcie_suspend,
	.resume_noirq = brcm_pcie_resume,
};
#else
#define brcm_pcie_pm_ops	NULL
#endif /* CONFIG_PM_SLEEP */

static int cfg_index(int busnr, int devfn, int reg)
{
	return ((PCI_SLOT(devfn) & 0x1f) << PCI_SLOT_SHIFT)
		| ((PCI_FUNC(devfn) & 0x07) << PCI_FUNC_SHIFT)
		| (busnr << PCI_BUSNUM_SHIFT)
		| (reg & ~3);
}

static u32 read_config(struct brcm_pcie *pcie, int cfg_idx)
{
	bpcie_writel(cfg_idx, IDX_ADDR(pcie));
	bpcie_readl(IDX_ADDR(pcie));
	return bpcie_readl(DATA_ADDR(pcie));
}

static void write_config(struct brcm_pcie *pcie, int cfg_idx, u32 val)
{
	bpcie_writel(cfg_idx, IDX_ADDR(pcie));
	bpcie_readl(IDX_ADDR(pcie));
	bpcie_writel(val, DATA_ADDR(pcie));
	bpcie_readl(DATA_ADDR(pcie));
}

static int brcm_pcie_write_config(struct pci_bus *bus, unsigned int devfn,
				  int where, int size, u32 data)
{
	struct brcm_pcie *pcie = bus->sysdata;
	u32 val = 0, mask, shift;
	void __iomem *base;
	bool rc_access;
	int idx;

	if (!is_pcie_link_up(pcie))
		return PCIBIOS_DEVICE_NOT_FOUND;

	base = pcie->base;

	rc_access = !!pci_is_root_bus(bus);

	idx = cfg_index(bus->number, devfn, where);
	WARN_ON(((where & 3) + size) > 4);

	if (rc_access && PCI_SLOT(devfn))
		return PCIBIOS_DEVICE_NOT_FOUND;

	if (size < 4) {
		/* partial word - read, modify, write */
		if (rc_access)
			val = bpcie_readl(base + (where & ~3));
		else
			val = read_config(pcie, idx);
	}

	shift = (where & 3) << 3;
	mask = (0xffffffff >> ((4 - size) << 3)) << shift;
	val = (val & ~mask) | ((data << shift) & mask);

	if (rc_access) {
		bpcie_writel(val, base + (where & ~3));
		bpcie_readl(base + (where & ~3));
	} else {
		write_config(pcie, idx, val);
	}
	return PCIBIOS_SUCCESSFUL;
}

static int brcm_pcie_read_config(struct pci_bus *bus, unsigned int devfn,
				 int where, int size, u32 *data)
{
	struct brcm_pcie *pcie = bus->sysdata;
	u32 val, mask, shift;
	void __iomem *base;
	bool rc_access;
	int idx;

	if (!is_pcie_link_up(pcie))
		return PCIBIOS_DEVICE_NOT_FOUND;

	base = pcie->base;

	rc_access = !!pci_is_root_bus(bus);
	idx = cfg_index(bus->number, devfn, where);
	WARN_ON(((where & 3) + size) > 4);

	if (rc_access && PCI_SLOT(devfn)) {
		*data = 0xffffffff;
		return PCIBIOS_FUNC_NOT_SUPPORTED;
	}

	if (rc_access)
		val = bpcie_readl(base + (where & ~3));
	else
		val = read_config(pcie, idx);

	shift = (where & 3) << 3;
	mask = (0xffffffff >> ((4 - size) << 3)) << shift;
	*data = (val & mask) >> shift;

	return PCIBIOS_SUCCESSFUL;
}

static const struct of_device_id brcm_pcie_match[] = {
	{ .compatible = "brcm,bcm7425-pcie", .data = &bcm7425_cfg },
	{ .compatible = "brcm,bcm7435-pcie", .data = &bcm7435_cfg },
	{ .compatible = "brcm,bcm7445-pcie", .data = &generic_cfg },
	{},
};
MODULE_DEVICE_TABLE(of, brcm_pcie_match);

static struct pci_ops brcm_pcie_ops = {
	.read = brcm_pcie_read_config,
	.write = brcm_pcie_write_config,
};

static int brcm_pcie_probe(struct platform_device *pdev)
{
	struct device_node *dn = pdev->dev.of_node, *mdn;
	const u32 *log2_scb_sizes, *dma_ranges;
	struct brcm_dev_pwr_supply *supply;
	const struct of_device_id *of_id;
	const struct pcie_cfg_data *data;
	struct resource_entry *win;
	struct brcm_pcie *pcie;
	struct list_head *pos;
	void __iomem *base;
	int i, rlen, ret;
	struct resource *r;
	const char *name;
	int supplies;
	LIST_HEAD(res);
	u32 tmp;

	pcie = devm_kzalloc(&pdev->dev, sizeof(struct brcm_pcie), GFP_KERNEL);
	if (!pcie)
		return -ENOMEM;

	of_id = of_match_node(brcm_pcie_match, dn);
	if (!of_id)
		return -EINVAL;

	data = of_id->data;
	pcie->reg_offsets = data->offsets;
	pcie->type = data->type;

	platform_set_drvdata(pdev, pcie);

	INIT_LIST_HEAD(&pcie->pwr_supplies);
	INIT_LIST_HEAD(&pcie->resource);

	supplies = of_property_count_strings(dn, "supply-names");
	if (supplies <= 0)
		supplies = 0;

	for (i = 0; i < supplies; i++) {
		if (of_property_read_string_index(dn, "supply-names", i,
						  &name))
			continue;
		supply = devm_kzalloc(&pdev->dev, sizeof(*supply), GFP_KERNEL);
		if (!supply)
			return -ENOMEM;

		strncpy(supply->name, name, sizeof(supply->name));
		supply->name[sizeof(supply->name) - 1] = '\0';
		supply->regulator = devm_regulator_get(&pdev->dev, name);
		if (IS_ERR(supply->regulator)) {
			dev_err(&pdev->dev, "Unable to get %s supply, err=%d\n",
				name, (int)PTR_ERR(supply->regulator));
			continue;
		}

		if (regulator_enable(supply->regulator))
			dev_err(&pdev->dev, "Unable to enable %s supply.\n",
				name);
		list_add_tail(&supply->node, &pcie->pwr_supplies);
	}

	/* 'num_memc' will be set only by the first controller, and all
	 * other controllers will use the value set by the first.
	 */
	if (num_memc == 0)
		for_each_compatible_node(mdn, NULL, "brcm,brcmstb-memc")
			if (of_device_is_available(mdn))
				num_memc++;

	r = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!r)
		return -EINVAL;

	base = devm_ioremap_resource(&pdev->dev, r);
	if (IS_ERR(base))
		return PTR_ERR(base);

	snprintf(pcie->name,
		 sizeof(pcie->name)-1, "PCIe%d", brcm_num_pci_controllers);
	pcie->num = brcm_num_pci_controllers;
	pcie->suspended = false;
	pcie->clk = devm_clk_get(&pdev->dev, "sw_pcie");
	if (IS_ERR(pcie->clk)) {
		dev_err(&pdev->dev, "could not get clock\n");
		pcie->clk = NULL;
	}
	ret = clk_prepare_enable(pcie->clk);
	if (ret) {
		dev_err(&pdev->dev, "could not enable clock\n");
		return ret;
	}
	pcie->dn = dn;
	pcie->base = base;
	pcie->dev = &pdev->dev;
	pcie->dev->of_node = dn;
	pcie->gen = 0;

	ret = of_property_read_u32(dn, "brcm,gen", &tmp);
	if (ret == 0) {
		if (tmp > 0 && tmp < 3)
			pcie->gen = (int) tmp;
		else
			dev_warn(pcie->dev, "bad DT value for prop 'brcm,gen");
	} else if (ret != -EINVAL) {
		dev_warn(pcie->dev, "error reading DT prop 'brcm,gen");
	}

	pcie->ssc = of_property_read_bool(dn, "brcm,ssc");

	/* Get the value for the log2 of the scb sizes.  Subtract 15 from
	 * each because the target register field has 0==disabled and 1==64KB.
	 */
	log2_scb_sizes = of_get_property(dn, "brcm,log2-scb-sizes", &rlen);
	if (log2_scb_sizes != NULL)
		for (i = 0; i < rlen/4; i++)
			pcie->scb_size_vals[i]
				= (int) of_read_number(log2_scb_sizes + i, 1)
					- 15;

	/* Look for the dma-ranges property.  If it exists, issue a warning
	 * as PCIe drivers may not work.  This is because the identity
	 * mapping between system memory and PCIe space is not preserved,
	 * and we need Linux to massage the dma_addr_t values it gets
	 * from dma memory allocation.  This functionality will be added
	 * in the near future.
	 */
	dma_ranges = of_get_property(dn, "dma-ranges", &rlen);
	if (dma_ranges != NULL)
		dev_warn(pcie->dev, "no identity map; PCI drivers may fail");

	pcie->irq = irq_of_parse_and_map(pdev->dev.of_node, 0);

	if (IS_ENABLED(CONFIG_PCI_MSI)) {
		ret = irq_of_parse_and_map(pdev->dev.of_node, 1);
		if (ret == 0)
			dev_warn(pcie->dev, "cannot get msi intr; MSI disabled\n");
		else
			pcie->msi.irq = ret;
	}

	ret = of_pci_get_host_bridge_resources(dn, 0, 0xff, &res, NULL);
	if (ret) {
		dev_err(pcie->dev, "ranges parsing failed\n");
		return ret;
	}

	i = 0;
	resource_list_for_each_entry(win, &res) {
		struct brcm_window *w = &pcie->out_wins[i];

		r = win->res;

		if (!r->flags)
			continue;

		switch (resource_type(r)) {
		case IORESOURCE_MEM:
			w->cpu_addr = r->start;
			w->size = resource_size(r);
			w->pcie_iomem_res.name  = "External PCIe MEM";
			w->pcie_iomem_res.flags = r->flags;
			w->pcie_iomem_res.start = r->start;
			w->pcie_iomem_res.end   = r->end;
			pcie->num_out_wins++;
			i++;
			/* Request memory region resources. */
			ret = devm_request_resource(&pdev->dev,
						    &iomem_resource,
						    &w->pcie_iomem_res);
			if (ret) {
				dev_err(&pdev->dev,
					"request PCIe memory resource failed\n");
				goto out_err_clk;
			}
			break;

		default:
			continue;
		}
	}

	brcm_pcie_setup_early(pcie);

	ret = brcm_setup_pcie_bridge(pcie);
	if (ret) {
		dev_err(&pdev->dev, "failed to setup PCIe bridge\n");
		/* If setup bridge fails, it cleans up behind itself */
		goto out_err;
	}

	pcie->bus = pci_scan_root_bus(pcie->dev, pcie->num, &brcm_pcie_ops,
				      pcie, &res);
	if (!pcie->bus) {
		ret = -ENOMEM;
		goto out_err_bus;
	}

	if (IS_ENABLED(CONFIG_PCI_MSI))
		pcie->bus->msi = &pcie->msi.chip;
	pci_bus_size_bridges(pcie->bus);
	pci_bus_assign_resources(pcie->bus);

	pci_fixup_irqs(pci_common_swizzle, of_irq_parse_and_map_pci);
	pci_bus_add_devices(pcie->bus);

	brcm_num_pci_controllers++;

	return 0;

out_err_bus:
	brcm_pcie_enter_l23(pcie);
	brcm_pcie_turn_off(pcie);
out_err_clk:
	clk_disable_unprepare(pcie->clk);
	list_for_each(pos, &pcie->pwr_supplies) {
		supply = list_entry(pos, struct brcm_dev_pwr_supply,
				    node);
		regulator_disable(supply->regulator);

	}
out_err:
	return ret;
}

static int brcm_pcie_remove(struct platform_device *pdev)
{
	return brcm_pcie_suspend(&pdev->dev);
}

static struct platform_driver brcm_pcie_driver = {
	.probe = brcm_pcie_probe,
	.remove = brcm_pcie_remove,
	.driver = {
		.name = "brcm-pcie",
		.owner = THIS_MODULE,
		.of_match_table = brcm_pcie_match,
		.pm = &brcm_pcie_pm_ops,
	},
};
module_platform_driver(brcm_pcie_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Broadcom STB PCIE RC driver");
MODULE_AUTHOR("Broadcom");
