/**************************************************************************
 *
 *  BRIEF MODULE DESCRIPTION
 *     PCI init for Ralink RT2880 solution
 *
 *  Copyright 2007 Ralink Inc. (bruce_chang@ralinktech.com.tw)
 *
 *  This program is free software; you can redistribute  it and/or modify it
 *  under  the terms of  the GNU General  Public License as published by the
 *  Free Software Foundation;  either version 2 of the  License, or (at your
 *  option) any later version.
 *
 *  THIS  SOFTWARE  IS PROVIDED   ``AS  IS'' AND   ANY  EXPRESS OR IMPLIED
 *  WARRANTIES,   INCLUDING, BUT NOT  LIMITED  TO, THE IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN
 *  NO  EVENT  SHALL   THE AUTHOR  BE    LIABLE FOR ANY   DIRECT, INDIRECT,
 *  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 *  NOT LIMITED   TO, PROCUREMENT OF  SUBSTITUTE GOODS  OR SERVICES; LOSS OF
 *  USE, DATA,  OR PROFITS; OR  BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
 *  ANY THEORY OF LIABILITY, WHETHER IN  CONTRACT, STRICT LIABILITY, OR TORT
 *  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 *  THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 *  You should have received a copy of the  GNU General Public License along
 *  with this program; if not, write  to the Free Software Foundation, Inc.,
 *  675 Mass Ave, Cambridge, MA 02139, USA.
 *
 *
 **************************************************************************
 * May 2007 Bruce Chang
 * Initial Release
 *
 * May 2009 Bruce Chang
 * support RT2880/RT3883 PCIe
 *
 * May 2011 Bruce Chang
 * support RT6855/MT7620 PCIe
 *
 **************************************************************************
 */

#include <linux/bitops.h>
#include <linux/types.h>
#include <linux/pci.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/version.h>
#include <asm/pci.h>
#include <asm/io.h>
#include <asm/mips-cm.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/of.h>
#include <linux/of_pci.h>
#include <linux/platform_device.h>

#include <ralink_regs.h>
#include <mt7621.h>

/*
 * These functions and structures provide the BIOS scan and mapping of the PCI
 * devices.
 */

#define PCI_MAX_CONTROLLERS		3

#define RALINK_PCIE0_CLK_EN		BIT(24)
#define RALINK_PCIE1_CLK_EN		BIT(25)
#define RALINK_PCIE2_CLK_EN		BIT(26)

#define RALINK_PCI_CONFIG_ADDR		0x20
#define RALINK_PCI_CONFIG_DATA_VIRTUAL_REG	0x24
#define RALINK_PCI_MEMBASE		0x0028
#define RALINK_PCI_IOBASE		0x002C
#define RALINK_PCIE0_RST		BIT(24)
#define RALINK_PCIE1_RST		BIT(25)
#define RALINK_PCIE2_RST		BIT(26)
#define RALINK_PCIE0_IRQ		BIT(20)
#define RALINK_PCIE1_IRQ		BIT(21)
#define RALINK_PCIE2_IRQ		BIT(22)

#define RALINK_PCI_PCICFG_ADDR		0x0000
#define RALINK_PCI_PCIMSK_ADDR		0x000C
#define RALINK_PCI_BASE	0xBE140000

struct pcie_controller_data {
	u32 offset;
	u32 clk_en;
	u32 rst;
	u32 irq;
};

static struct pcie_controller_data pcie_controllers[] = {
	{
		.offset = 0x2000,
		.clk_en = RALINK_PCIE0_CLK_EN,
		.rst = RALINK_PCIE0_RST,
		.irq = RALINK_PCIE0_IRQ,
	},
	{
		.offset = 0x3000,
		.clk_en = RALINK_PCIE1_CLK_EN,
		.rst = RALINK_PCIE1_RST,
		.irq = RALINK_PCIE1_IRQ,
	},
	{
		.offset = 0x4000,
		.clk_en = RALINK_PCIE2_CLK_EN,
		.rst = RALINK_PCIE2_RST,
		.irq = RALINK_PCIE2_IRQ,
	},
};

#define RALINK_PCI_BAR0SETUP_ADDR(dev)	(pcie_controllers[(dev)].offset + 0x0010)
#define RALINK_PCI_IMBASEBAR0_ADDR(dev)	(pcie_controllers[(dev)].offset + 0x0018)
#define RALINK_PCI_ID(dev)		(pcie_controllers[(dev)].offset + 0x0030)
#define RALINK_PCI_CLASS(dev)		(pcie_controllers[(dev)].offset + 0x0034)
#define RALINK_PCI_SUBID(dev)		(pcie_controllers[(dev)].offset + 0x0038)
#define RALINK_PCI_STATUS(dev)		(pcie_controllers[(dev)].offset + 0x0050)
#define RALINK_PCI_DERR(dev)		(pcie_controllers[(dev)].offset + 0x0060)
#define RALINK_PCI_ECRC(dev)		(pcie_controllers[(dev)].offset + 0x0064)

#define RALINK_PCIEPHY_P0P1_CTL_OFFSET	0x9000
#define RALINK_PCIEPHY_P2_CTL_OFFSET	0xA000

#define RALINK_PCI_MM_MAP_BASE		0x60000000
#define RALINK_PCI_IO_MAP_BASE		0x1e160000

#define RALINK_CLKCFG1			0x30
#define RALINK_RSTCTRL			0x34
#define RALINK_GPIOMODE			0x60
#define RALINK_PCIE_CLK_GEN		0x7c
#define RALINK_PCIE_CLK_GEN1		0x80
#define PPLL_CFG1			0x9c
#define PPLL_DRV			0xa0
/* SYSC_REG_SYSTEM_CONFIG1 bits */
#define RALINK_PCI_HOST_MODE_EN		BIT(7)
#define RALINK_PCIE_RC_MODE_EN		BIT(8)
//RALINK_RSTCTRL bit
#define RALINK_PCIE_RST			BIT(23)
#define RALINK_PCI_RST			BIT(24)
//RALINK_CLKCFG1 bit
#define RALINK_PCI_CLK_EN		BIT(19)
#define RALINK_PCIE_CLK_EN		BIT(21)
//RALINK_GPIOMODE bit
#define PCI_SLOTx2			BIT(11)
#define PCI_SLOTx1			(2<<11)
//MTK PCIE PLL bit
#define PDRV_SW_SET			BIT(31)
#define LC_CKDRVPD_			BIT(19)

#define MEMORY_BASE 0x0
static int pcie_link_status = 0;

static void __iomem *mt7621_pci_base;

static inline void mt7621_pcie_assert_sysrst(u32 val)
{
	if (rt_sysc_r32(SYSC_REG_CHIP_REV) == 0x00030101)
		rt_sysc_m32(0, val, RALINK_RSTCTRL);
	else
		rt_sysc_m32(val, 0, RALINK_RSTCTRL);
}

static inline void mt7621_pcie_deassert_sysrst(u32 val)
{
	if (rt_sysc_r32(SYSC_REG_CHIP_REV) == 0x00030101)
		rt_sysc_m32(val, 0, RALINK_RSTCTRL);
	else
		rt_sysc_m32(0, val, RALINK_RSTCTRL);
}

static u32 mt7621_pci_reg_read(u32 reg)
{
	return readl(mt7621_pci_base + reg);
}

static void mt7621_pci_reg_write(u32 val, u32 reg)
{
	writel(val, mt7621_pci_base + reg);
}

static inline u32 mt7621_pci_get_cfgaddr(unsigned int bus, unsigned int slot,
					 unsigned int func, unsigned int where)
{
	return ((bus << 16) | (slot << 11) | (func << 8) | (where & 0xfc) |
		0x80000000);
}

static int
pci_config_read(struct pci_bus *bus, unsigned int devfn,
		int where, int size, u32 *val)
{
	u32 address_reg, data_reg;
	u32 address;

	address_reg = RALINK_PCI_CONFIG_ADDR;
	data_reg = RALINK_PCI_CONFIG_DATA_VIRTUAL_REG;

	address = (((where & 0xF00) >> 8) << 24) |
		   mt7621_pci_get_cfgaddr(bus->number, PCI_SLOT(devfn),
					  PCI_FUNC(devfn), where);

	writel(address, mt7621_pci_base + address_reg);

	switch (size) {
	case 1:
		*val = readb(mt7621_pci_base + data_reg + (where & 0x3));
		break;
	case 2:
		*val = readw(mt7621_pci_base + data_reg + (where & 0x3));
		break;
	case 4:
		*val = readl(mt7621_pci_base + data_reg);
		break;
	}

	return PCIBIOS_SUCCESSFUL;
}

static int
pci_config_write(struct pci_bus *bus, unsigned int devfn,
		 int where, int size, u32 val)
{
	u32 address_reg, data_reg;
	u32 address;

	address_reg = RALINK_PCI_CONFIG_ADDR;
	data_reg = RALINK_PCI_CONFIG_DATA_VIRTUAL_REG;

	address = (((where & 0xF00) >> 8) << 24) |
		   mt7621_pci_get_cfgaddr(bus->number, PCI_SLOT(devfn),
					  PCI_FUNC(devfn), where);

	writel(address, mt7621_pci_base + address_reg);

	switch (size) {
	case 1:
		writeb((u8)val, mt7621_pci_base + data_reg + (where & 0x3));
		break;
	case 2:
		writew((u16)val, mt7621_pci_base + data_reg + (where & 0x3));
		break;
	case 4:
		writel(val, mt7621_pci_base + data_reg);
		break;
	}

	return PCIBIOS_SUCCESSFUL;
}

struct pci_ops mt7621_pci_ops = {
	.read		= pci_config_read,
	.write		= pci_config_write,
};

static struct resource mt7621_res_pci_mem1;
static struct resource mt7621_res_pci_io1;
static struct pci_controller mt7621_controller = {
	.pci_ops	= &mt7621_pci_ops,
	.mem_resource	= &mt7621_res_pci_mem1,
	.io_resource	= &mt7621_res_pci_io1,
};

static u32
read_config(unsigned int dev, u32 reg)
{
	u32 address_reg, data_reg, address;

	address_reg = RALINK_PCI_CONFIG_ADDR;
	data_reg = RALINK_PCI_CONFIG_DATA_VIRTUAL_REG;
	address = (((reg & 0xF00) >> 8) << 24) |
		   mt7621_pci_get_cfgaddr(0, dev, 0, reg);
	writel(address, mt7621_pci_base + address_reg);
	return readl(mt7621_pci_base + data_reg);
}

static void
write_config(unsigned int dev, u32 reg, u32 val)
{
	u32 address_reg, data_reg, address;

	address_reg = RALINK_PCI_CONFIG_ADDR;
	data_reg = RALINK_PCI_CONFIG_DATA_VIRTUAL_REG;
	address = (((reg & 0xF00) >> 8) << 24) |
		   mt7621_pci_get_cfgaddr(0, dev, 0, reg);
	writel(address, mt7621_pci_base + address_reg);
	writel(val, mt7621_pci_base + data_reg);
}

int
pcibios_map_irq(const struct pci_dev *dev, u8 slot, u8 pin)
{
	u16 cmd;
	u32 val;
	int irq;

	if (dev->bus->number == 0) {
		write_config(slot, PCI_BASE_ADDRESS_0, MEMORY_BASE);
		val = read_config(slot, PCI_BASE_ADDRESS_0);
		printk("BAR0 at slot %d = %x\n", slot, val);
	}

	pci_write_config_byte(dev, PCI_CACHE_LINE_SIZE, 0x14);  //configure cache line size 0x14
	pci_write_config_byte(dev, PCI_LATENCY_TIMER, 0xFF);  //configure latency timer 0x10
	pci_read_config_word(dev, PCI_COMMAND, &cmd);
	cmd = cmd | PCI_COMMAND_MASTER | PCI_COMMAND_IO | PCI_COMMAND_MEMORY;
	pci_write_config_word(dev, PCI_COMMAND, cmd);

	irq = of_irq_parse_and_map_pci(dev, slot, pin);

	pci_write_config_byte(dev, PCI_INTERRUPT_LINE, irq);
	return irq;
}

void
set_pcie_phy(u32 offset, int start_b, int bits, int val)
{
	u32 reg = mt7621_pci_reg_read(offset);

	reg &= ~(((1 << bits) - 1) << start_b);
	reg |= val << start_b;
	mt7621_pci_reg_write(reg, offset);
}

void
bypass_pipe_rst(void)
{
	/* PCIe Port 0 */
	set_pcie_phy((RALINK_PCIEPHY_P0P1_CTL_OFFSET + 0x02c), 12, 1, 0x01);	// rg_pe1_pipe_rst_b
	set_pcie_phy((RALINK_PCIEPHY_P0P1_CTL_OFFSET + 0x02c),  4, 1, 0x01);	// rg_pe1_pipe_cmd_frc[4]
	/* PCIe Port 1 */
	set_pcie_phy((RALINK_PCIEPHY_P0P1_CTL_OFFSET + 0x12c), 12, 1, 0x01);	// rg_pe1_pipe_rst_b
	set_pcie_phy((RALINK_PCIEPHY_P0P1_CTL_OFFSET + 0x12c),  4, 1, 0x01);	// rg_pe1_pipe_cmd_frc[4]
	/* PCIe Port 2 */
	set_pcie_phy((RALINK_PCIEPHY_P2_CTL_OFFSET + 0x02c), 12, 1, 0x01);	// rg_pe1_pipe_rst_b
	set_pcie_phy((RALINK_PCIEPHY_P2_CTL_OFFSET + 0x02c),  4, 1, 0x01);	// rg_pe1_pipe_cmd_frc[4]
}

void
set_phy_for_ssc(void)
{
	unsigned long reg = rt_sysc_r32(SYSC_REG_SYSTEM_CONFIG0);

	reg = (reg >> 6) & 0x7;
	/* Set PCIe Port0 & Port1 PHY to disable SSC */
	/* Debug Xtal Type */
	set_pcie_phy((RALINK_PCIEPHY_P0P1_CTL_OFFSET + 0x400),  8, 1, 0x01);	// rg_pe1_frc_h_xtal_type
	set_pcie_phy((RALINK_PCIEPHY_P0P1_CTL_OFFSET + 0x400),  9, 2, 0x00);	// rg_pe1_h_xtal_type
	set_pcie_phy((RALINK_PCIEPHY_P0P1_CTL_OFFSET + 0x000),  4, 1, 0x01);	// rg_pe1_frc_phy_en	//Force Port 0 enable control
	set_pcie_phy((RALINK_PCIEPHY_P0P1_CTL_OFFSET + 0x100),  4, 1, 0x01);	// rg_pe1_frc_phy_en	//Force Port 1 enable control
	set_pcie_phy((RALINK_PCIEPHY_P0P1_CTL_OFFSET + 0x000),  5, 1, 0x00);	// rg_pe1_phy_en	//Port 0 disable
	set_pcie_phy((RALINK_PCIEPHY_P0P1_CTL_OFFSET + 0x100),  5, 1, 0x00);	// rg_pe1_phy_en	//Port 1 disable
	if (reg <= 5 && reg >= 3) {	// 40MHz Xtal
		set_pcie_phy((RALINK_PCIEPHY_P0P1_CTL_OFFSET + 0x490),  6, 2, 0x01);	// RG_PE1_H_PLL_PREDIV	//Pre-divider ratio (for host mode)
		printk("***** Xtal 40MHz *****\n");
	} else {			// 25MHz | 20MHz Xtal
		set_pcie_phy((RALINK_PCIEPHY_P0P1_CTL_OFFSET + 0x490),  6, 2, 0x00);	// RG_PE1_H_PLL_PREDIV	//Pre-divider ratio (for host mode)
		if (reg >= 6) {
			printk("***** Xtal 25MHz *****\n");
			set_pcie_phy((RALINK_PCIEPHY_P0P1_CTL_OFFSET + 0x4bc),  4, 2, 0x01);	// RG_PE1_H_PLL_FBKSEL	//Feedback clock select
			set_pcie_phy((RALINK_PCIEPHY_P0P1_CTL_OFFSET + 0x49c),  0, 31, 0x18000000);	// RG_PE1_H_LCDDS_PCW_NCPO	//DDS NCPO PCW (for host mode)
			set_pcie_phy((RALINK_PCIEPHY_P0P1_CTL_OFFSET + 0x4a4),  0, 16, 0x18d);	// RG_PE1_H_LCDDS_SSC_PRD	//DDS SSC dither period control
			set_pcie_phy((RALINK_PCIEPHY_P0P1_CTL_OFFSET + 0x4a8),  0, 12, 0x4a);	// RG_PE1_H_LCDDS_SSC_DELTA	//DDS SSC dither amplitude control
			set_pcie_phy((RALINK_PCIEPHY_P0P1_CTL_OFFSET + 0x4a8), 16, 12, 0x4a);	// RG_PE1_H_LCDDS_SSC_DELTA1	//DDS SSC dither amplitude control for initial
		} else {
			printk("***** Xtal 20MHz *****\n");
		}
	}
	set_pcie_phy((RALINK_PCIEPHY_P0P1_CTL_OFFSET + 0x4a0),  5, 1, 0x01);	// RG_PE1_LCDDS_CLK_PH_INV	//DDS clock inversion
	set_pcie_phy((RALINK_PCIEPHY_P0P1_CTL_OFFSET + 0x490), 22, 2, 0x02);	// RG_PE1_H_PLL_BC
	set_pcie_phy((RALINK_PCIEPHY_P0P1_CTL_OFFSET + 0x490), 18, 4, 0x06);	// RG_PE1_H_PLL_BP
	set_pcie_phy((RALINK_PCIEPHY_P0P1_CTL_OFFSET + 0x490), 12, 4, 0x02);	// RG_PE1_H_PLL_IR
	set_pcie_phy((RALINK_PCIEPHY_P0P1_CTL_OFFSET + 0x490),  8, 4, 0x01);	// RG_PE1_H_PLL_IC
	set_pcie_phy((RALINK_PCIEPHY_P0P1_CTL_OFFSET + 0x4ac), 16, 3, 0x00);	// RG_PE1_H_PLL_BR
	set_pcie_phy((RALINK_PCIEPHY_P0P1_CTL_OFFSET + 0x490),  1, 3, 0x02);	// RG_PE1_PLL_DIVEN
	if (reg <= 5 && reg >= 3) {	// 40MHz Xtal
		set_pcie_phy((RALINK_PCIEPHY_P0P1_CTL_OFFSET + 0x414),  6, 2, 0x01);	// rg_pe1_mstckdiv		//value of da_pe1_mstckdiv when force mode enable
		set_pcie_phy((RALINK_PCIEPHY_P0P1_CTL_OFFSET + 0x414),  5, 1, 0x01);	// rg_pe1_frc_mstckdiv	//force mode enable of da_pe1_mstckdiv
	}
	/* Enable PHY and disable force mode */
	set_pcie_phy((RALINK_PCIEPHY_P0P1_CTL_OFFSET + 0x000),  5, 1, 0x01);	// rg_pe1_phy_en	//Port 0 enable
	set_pcie_phy((RALINK_PCIEPHY_P0P1_CTL_OFFSET + 0x100),  5, 1, 0x01);	// rg_pe1_phy_en	//Port 1 enable
	set_pcie_phy((RALINK_PCIEPHY_P0P1_CTL_OFFSET + 0x000),  4, 1, 0x00);	// rg_pe1_frc_phy_en	//Force Port 0 disable control
	set_pcie_phy((RALINK_PCIEPHY_P0P1_CTL_OFFSET + 0x100),  4, 1, 0x00);	// rg_pe1_frc_phy_en	//Force Port 1 disable control

	/* Set PCIe Port2 PHY to disable SSC */
	/* Debug Xtal Type */
	set_pcie_phy((RALINK_PCIEPHY_P2_CTL_OFFSET + 0x400),  8, 1, 0x01);	// rg_pe1_frc_h_xtal_type
	set_pcie_phy((RALINK_PCIEPHY_P2_CTL_OFFSET + 0x400),  9, 2, 0x00);	// rg_pe1_h_xtal_type
	set_pcie_phy((RALINK_PCIEPHY_P2_CTL_OFFSET + 0x000),  4, 1, 0x01);	// rg_pe1_frc_phy_en	//Force Port 0 enable control
	set_pcie_phy((RALINK_PCIEPHY_P2_CTL_OFFSET + 0x000),  5, 1, 0x00);	// rg_pe1_phy_en	//Port 0 disable
	if (reg <= 5 && reg >= 3) {	// 40MHz Xtal
		set_pcie_phy((RALINK_PCIEPHY_P2_CTL_OFFSET + 0x490),  6, 2, 0x01);	// RG_PE1_H_PLL_PREDIV	//Pre-divider ratio (for host mode)
	} else {			// 25MHz | 20MHz Xtal
		set_pcie_phy((RALINK_PCIEPHY_P2_CTL_OFFSET + 0x490),  6, 2, 0x00);	// RG_PE1_H_PLL_PREDIV	//Pre-divider ratio (for host mode)
		if (reg >= 6) {		// 25MHz Xtal
			set_pcie_phy((RALINK_PCIEPHY_P2_CTL_OFFSET + 0x4bc),  4, 2, 0x01);	// RG_PE1_H_PLL_FBKSEL	//Feedback clock select
			set_pcie_phy((RALINK_PCIEPHY_P2_CTL_OFFSET + 0x49c),  0, 31, 0x18000000);	// RG_PE1_H_LCDDS_PCW_NCPO	//DDS NCPO PCW (for host mode)
			set_pcie_phy((RALINK_PCIEPHY_P2_CTL_OFFSET + 0x4a4),  0, 16, 0x18d);	// RG_PE1_H_LCDDS_SSC_PRD	//DDS SSC dither period control
			set_pcie_phy((RALINK_PCIEPHY_P2_CTL_OFFSET + 0x4a8),  0, 12, 0x4a);	// RG_PE1_H_LCDDS_SSC_DELTA	//DDS SSC dither amplitude control
			set_pcie_phy((RALINK_PCIEPHY_P2_CTL_OFFSET + 0x4a8), 16, 12, 0x4a);	// RG_PE1_H_LCDDS_SSC_DELTA1	//DDS SSC dither amplitude control for initial
		}
	}
	set_pcie_phy((RALINK_PCIEPHY_P2_CTL_OFFSET + 0x4a0),  5, 1, 0x01);	// RG_PE1_LCDDS_CLK_PH_INV	//DDS clock inversion
	set_pcie_phy((RALINK_PCIEPHY_P2_CTL_OFFSET + 0x490), 22, 2, 0x02);	// RG_PE1_H_PLL_BC
	set_pcie_phy((RALINK_PCIEPHY_P2_CTL_OFFSET + 0x490), 18, 4, 0x06);	// RG_PE1_H_PLL_BP
	set_pcie_phy((RALINK_PCIEPHY_P2_CTL_OFFSET + 0x490), 12, 4, 0x02);	// RG_PE1_H_PLL_IR
	set_pcie_phy((RALINK_PCIEPHY_P2_CTL_OFFSET + 0x490),  8, 4, 0x01);	// RG_PE1_H_PLL_IC
	set_pcie_phy((RALINK_PCIEPHY_P2_CTL_OFFSET + 0x4ac), 16, 3, 0x00);	// RG_PE1_H_PLL_BR
	set_pcie_phy((RALINK_PCIEPHY_P2_CTL_OFFSET + 0x490),  1, 3, 0x02);	// RG_PE1_PLL_DIVEN
	if (reg <= 5 && reg >= 3) {	// 40MHz Xtal
		set_pcie_phy((RALINK_PCIEPHY_P2_CTL_OFFSET + 0x414),  6, 2, 0x01);	// rg_pe1_mstckdiv		//value of da_pe1_mstckdiv when force mode enable
		set_pcie_phy((RALINK_PCIEPHY_P2_CTL_OFFSET + 0x414),  5, 1, 0x01);	// rg_pe1_frc_mstckdiv	//force mode enable of da_pe1_mstckdiv
	}
	/* Enable PHY and disable force mode */
	set_pcie_phy((RALINK_PCIEPHY_P2_CTL_OFFSET + 0x000),  5, 1, 0x01);	// rg_pe1_phy_en	//Port 0 enable
	set_pcie_phy((RALINK_PCIEPHY_P2_CTL_OFFSET + 0x000),  4, 1, 0x00);	// rg_pe1_frc_phy_en	//Force Port 0 disable control
}

void setup_cm_memory_region(struct resource *mem_resource)
{
	resource_size_t mask;
	if (mips_cps_numiocu(0)) {
		/* FIXME: hardware doesn't accept mask values with 1s after
		 * 0s (e.g. 0xffef), so it would be great to warn if that's
		 * about to happen */
		mask = ~(mem_resource->end - mem_resource->start);

		write_gcr_reg1_base(mem_resource->start);
		write_gcr_reg1_mask(mask | CM_GCR_REGn_MASK_CMTGT_IOCU0);
		printk("PCI coherence region base: 0x%08llx, mask/settings: 0x%08llx\n",
			(unsigned long long)read_gcr_reg1_base(),
			(unsigned long long)read_gcr_reg1_mask());
	}
}

static void mt7621_pci_disable(u8 controller)
{
	mt7621_pcie_assert_sysrst(pcie_controllers[controller].rst);
	rt_sysc_m32(pcie_controllers[controller].clk_en, 0, RALINK_CLKCFG1);
	pcie_link_status &= ~(1 << controller);
}

static void mt7621_pci_enable_irqs(u8 controller)
{
	u32 mask;

	if ((mt7621_pci_reg_read(RALINK_PCI_STATUS(controller)) & 0x1) == 0) {
		printk("PCIE0 no card, disable it(RST&CLK)\n");
		mt7621_pci_disable(controller);
		return;
	}

	pcie_link_status |= (1 << controller);
	mask = mt7621_pci_reg_read(RALINK_PCI_PCIMSK_ADDR);
	mask |= pcie_controllers[controller].irq;
	mt7621_pci_reg_write(mask, RALINK_PCI_PCIMSK_ADDR);
}

static int mt7621_pci_probe(struct platform_device *pdev)
{
	int i;
	u32 mask;
	u32 val;

	mt7621_pci_base = (void __iomem *)RALINK_PCI_BASE;
	iomem_resource.start = 0;
	iomem_resource.end = ~0;
	ioport_resource.start = 0;
	ioport_resource.end = ~0;

	val = (RALINK_PCIE0_RST | RALINK_PCIE1_RST | RALINK_PCIE2_RST);
	mt7621_pcie_assert_sysrst(val);

	*(unsigned int *)(0xbe000060) &= ~(0x3<<10 | 0x3<<3);
	*(unsigned int *)(0xbe000060) |= 0x1<<10 | 0x1<<3;
	mdelay(100);
	*(unsigned int *)(0xbe000600) |= 0x1<<19 | 0x1<<8 | 0x1<<7; // use GPIO19/GPIO8/GPIO7 (PERST_N/UART_RXD3/UART_TXD3)
	mdelay(100);
	*(unsigned int *)(0xbe000620) &= ~(0x1<<19 | 0x1<<8 | 0x1<<7);		// clear DATA

	mdelay(100);

	val = (RALINK_PCIE0_RST | RALINK_PCIE1_RST | RALINK_PCIE2_RST);
	mt7621_pcie_deassert_sysrst(val);

	if ((*(unsigned int *)(0xbe00000c)&0xFFFF) == 0x0101) // MT7621 E2
		bypass_pipe_rst();
	set_phy_for_ssc();

	for (i = 0; i < PCI_MAX_CONTROLLERS; i++) {
		val = read_config(0, 0x70c);
		printk("Port %d N_FTS = %x\n", i, val);
	}

	rt_sysc_m32(0, RALINK_PCIE_RST, RALINK_RSTCTRL);
	rt_sysc_m32(0x30, 2 << 4, SYSC_REG_SYSTEM_CONFIG1);

	rt_sysc_m32(0x80000000, 0, RALINK_PCIE_CLK_GEN);
	rt_sysc_m32(0x7f000000, 0xa << 24, RALINK_PCIE_CLK_GEN1);
	rt_sysc_m32(0, 0x80000000, RALINK_PCIE_CLK_GEN);

	mdelay(50);
	rt_sysc_m32(RALINK_PCIE_RST, 0, RALINK_RSTCTRL);

	/* Use GPIO control instead of PERST_N */
	*(unsigned int *)(0xbe000620) |= 0x1<<19 | 0x1<<8 | 0x1<<7;		// set DATA
	mdelay(1000);


	for (i = 0; i < PCI_MAX_CONTROLLERS; i++)
		mt7621_pci_enable_irqs(i);

	if (pcie_link_status == 0)
		return 0;

/*
pcie(2/1/0) link status	pcie2_num	pcie1_num	pcie0_num
3'b000			x		x		x
3'b001			x		x		0
3'b010			x		0		x
3'b011			x		1		0
3'b100			0		x		x
3'b101			1		x		0
3'b110			1		0		x
3'b111			2		1		0
*/
	mask = mt7621_pci_reg_read(RALINK_PCI_PCICFG_ADDR);
	mask &= ~0x00ff0000;
	mask |= (0x1 << 16); // port0
	mask |= (0x0 << 20); // port1

	if (pcie_link_status != 2)
		mask |= (0x1 << 24); // port2

	mt7621_pci_reg_write(mask, RALINK_PCI_PCICFG_ADDR);

/*
	ioport_resource.start = mt7621_res_pci_io1.start;
	ioport_resource.end = mt7621_res_pci_io1.end;
*/

	mt7621_pci_reg_write(0xffffffff, RALINK_PCI_MEMBASE); //RALINK_PCI_MM_MAP_BASE;
	mt7621_pci_reg_write(RALINK_PCI_IO_MAP_BASE, RALINK_PCI_IOBASE);

	//PCIe0
	if ((pcie_link_status & 0x1) != 0) {
		/* open 7FFF:2G; ENABLE */
		mt7621_pci_reg_write(0x7FFF0001, RALINK_PCI_BAR0SETUP_ADDR(0));
		mt7621_pci_reg_write(MEMORY_BASE, RALINK_PCI_IMBASEBAR0_ADDR(0));
		mt7621_pci_reg_write(0x06040001, RALINK_PCI_CLASS(0));
		printk("PCIE0 enabled\n");
	}

	//PCIe1
	if ((pcie_link_status & 0x2) != 0) {
		/* open 7FFF:2G; ENABLE */
		mt7621_pci_reg_write(0x7FFF0001, RALINK_PCI_BAR0SETUP_ADDR(1));
		mt7621_pci_reg_write(MEMORY_BASE, RALINK_PCI_IMBASEBAR0_ADDR(1));
		mt7621_pci_reg_write(0x06040001, RALINK_PCI_CLASS(1));
		printk("PCIE1 enabled\n");
	}

	//PCIe2
	if ((pcie_link_status & 0x4) != 0) {
		/* open 7FFF:2G; ENABLE */
		mt7621_pci_reg_write(0x7FFF0001, RALINK_PCI_BAR0SETUP_ADDR(2));
		mt7621_pci_reg_write(MEMORY_BASE, RALINK_PCI_IMBASEBAR0_ADDR(2));
		mt7621_pci_reg_write(0x06040001, RALINK_PCI_CLASS(2));
		printk("PCIE2 enabled\n");
	}

	switch (pcie_link_status) {
	case 7:
		val = read_config(2, 0x4);
		write_config(2, 0x4, (val | 0x4));
		val = read_config(2, 0x70c);
		val &= ~(0xff)<<8;
		val |= 0x50<<8;
		write_config(2, 0x70c, val);
	case 3:
	case 5:
	case 6:
		val = read_config(1, 0x4);
		write_config(1, 0x4, (val | 0x4));
		val = read_config(1, 0x70c);
		val &= ~(0xff)<<8;
		val |= 0x50<<8;
		write_config(1, 0x70c, val);
	default:
		val = read_config(0, 0x4);
		write_config(0, 0x4, (val | 0x4)); //bus master enable
		val = read_config(0, 0x70c);
		val &= ~(0xff)<<8;
		val |= 0x50<<8;
		write_config(0, 0x70c, val);
	}

	pci_load_of_ranges(&mt7621_controller, pdev->dev.of_node);
	setup_cm_memory_region(mt7621_controller.mem_resource);
	register_pci_controller(&mt7621_controller);
	return 0;

}

int pcibios_plat_dev_init(struct pci_dev *dev)
{
	return 0;
}

static const struct of_device_id mt7621_pci_ids[] = {
	{ .compatible = "mediatek,mt7621-pci" },
	{},
};
MODULE_DEVICE_TABLE(of, mt7621_pci_ids);

static struct platform_driver mt7621_pci_driver = {
	.probe = mt7621_pci_probe,
	.driver = {
		.name = "mt7621-pci",
		.of_match_table = of_match_ptr(mt7621_pci_ids),
	},
};

static int __init mt7621_pci_init(void)
{
	return platform_driver_register(&mt7621_pci_driver);
}

arch_initcall(mt7621_pci_init);
