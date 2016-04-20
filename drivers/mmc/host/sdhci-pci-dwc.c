/*
 * Copyright (C) 2016 Synopsys, Inc.
 *
 * Author: Manjunath M B <manjumb@synopsys.com>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/pci.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/moduleparam.h>

#include "sdhci.h"
#include "sdhci-pci.h"
#include "sdhci-pci-dwc.h"

/*****************************************************************************\
 *                                                                           *
 * Hardware specific clock handling                                          *
 *                                                                           *
\*****************************************************************************/
static const struct sdhci_ops *sdhci_ops;	/* Low level hw interface */

static void snps_reset_dcm(struct sdhci_host *host,
				unsigned int mask, unsigned char reset)
{
	u32 reg = 0;
	u16 vendor_ptr = 0;

	vendor_ptr = sdhci_readw(host, SDHCI_UHS2_VENDOR);

	reg = sdhci_readl(host, (SDHC_GPIO_OUT + vendor_ptr));

	if (reset == 1)
		reg |= mask;
	else
		reg &= ~mask;

	sdhci_writel(host, reg, (SDHC_GPIO_OUT + vendor_ptr));
}

static void sdhci_set_clock_snps(struct sdhci_host *host,
						unsigned int clock)
{
	int div = 0;
	int mul = 0;
	int div_val = 0;
	int mul_val = 0;
	int mul_div_val = 0;
	u32 reg = 0;
	u16 clk = 0;
	u16 vendor_ptr = 0;
	u32 mask = 0;
	unsigned long timeout;
	u32 tx_clk_phase_val = SDHC_DEF_TX_CLK_PH_VAL;
	u32 rx_clk_phase_val = SDHC_DEF_RX_CLK_PH_VAL;

	/*
	 * if clock is less than 25MHz, divided clock is used.
	 * For divided clock, we can use the standard sdhci_set_clock().
	 * For clock above 25MHz, DRP clock is used
	 * Here, we cannot use sdhci_set_clock(), we need to program
	 * TX RX CLOCK DCM DRP for appropriate clock
	 */

	if (clock <= 25000000) {
		/* Then call generic set_clock */
		if (sdhci_ops->set_clock)
			sdhci_ops->set_clock(host, clock);
	} else {

		host->mmc->actual_clock = 0;
		vendor_ptr = sdhci_readw(host, SDHCI_UHS2_VENDOR);

		/* Select un-phase shifted clock before reset Tx Tuning DCM*/
		reg = sdhci_readl(host, (SDHC_GPIO_OUT + vendor_ptr));
		reg &= ~SDHC_TX_CLK_SEL_TUNED;
		sdhci_writel(host, reg, (SDHC_GPIO_OUT + vendor_ptr));
		mdelay(10);

		sdhci_writew(host, 0, SDHCI_CLOCK_CONTROL);

		/* Lets chose the Mulitplier value to be 0x2 */
		mul = 0x2;
		for (div = 1; div <= 32; div++) {
			if (((host->max_clk * mul) / div)
					<= clock)
				break;
		}
		/*
		 * Set Programmable Clock Mode in the Clock
		 * Control register.
		 */
		div_val = div - 1;
		mul_val = mul - 1;

		host->mmc->actual_clock = (host->max_clk * mul) / div;
		/*
		 * Program the DCM DRP
		 * Step 1: Assert DCM Reset
		 * Step 2: Program the mul and div values in DRP
		 * Step 3: Read from DRP base 0x00 to restore DCM output as per
		 * www.xilinx.com/support/documentation/user_guides/ug191.pdf
		 * Step 4: De-Assert reset to DCM
		 */

		mask = SDHC_CARD_TX_CLK_DCM_RST;
		snps_reset_dcm(host, mask, 1);

		mul_div_val = (mul_val << 8) | div_val;
		sdhci_writew(host, mul_div_val, TXRX_CLK_DCM_MUL_DIV_DRP);

		reg = sdhci_readl(host, TXRX_CLK_DCM_DRP_BASE_51);

		snps_reset_dcm(host, mask, 0);

		clk |= SDHCI_PROG_CLOCK_MODE | SDHCI_CLOCK_INT_EN;
		sdhci_writew(host, clk, SDHCI_CLOCK_CONTROL);

		/* Wait max 20 ms */
		timeout = 20;
		while (!((clk = sdhci_readw(host, SDHCI_CLOCK_CONTROL))
					& SDHCI_CLOCK_INT_STABLE)) {
			if (timeout == 0) {
				pr_err("%s: Internal clock never stabilised\n",
						mmc_hostname(host->mmc));
				return;
			}
			timeout--;
			mdelay(1);
		}

		clk |= SDHCI_CLOCK_CARD_EN;
		sdhci_writew(host, clk, SDHCI_CLOCK_CONTROL);

		/*
		 * This Clock might have affected the TX CLOCK DCM and RX CLOCK
		 * DCM which are used for Phase control; Reset these DCM's
		 * for proper clock output
		 *
		 * Step 1: Reset the DCM
		 * Step 2: De-Assert reset to DCM
		 */

		mask = SDHC_TUNING_TX_CLK_DCM_RST | SDHC_TUNING_RX_CLK_DCM_RST;
		snps_reset_dcm(host, mask, 1);
		mdelay(10);
		snps_reset_dcm(host, mask, 0);

		/* Select working phase value if clock is <= 50MHz */
		if (clock <= 50000000) {
			/*Change the Tx Phase value here */
			reg = sdhci_readl(host, (SDHC_GPIO_OUT + vendor_ptr));
			reg |= (SDHC_TUNING_TX_CLK_SEL_MASK &
			   (tx_clk_phase_val << SDHC_TUNING_TX_CLK_SEL_SHIFT));

			sdhci_writel(host, reg, (SDHC_GPIO_OUT + vendor_ptr));
			mdelay(10);

			/* Program to select phase shifted clock */
			reg |= SDHC_TX_CLK_SEL_TUNED;
			sdhci_writel(host, reg, (SDHC_GPIO_OUT + vendor_ptr));

			/*
			 * For 50Mhz, tuning is not possible.
			 * Lets fix the sampling Phase of Rx Clock here.
			 */
			reg = sdhci_readl(host, (SDHC_DBOUNCE + vendor_ptr));
			reg &= ~SDHC_TUNING_RX_CLK_SEL_MASK;
			reg |= (SDHC_TUNING_RX_CLK_SEL_MASK &
					rx_clk_phase_val);
			sdhci_writel(host, reg, (SDHC_DBOUNCE + vendor_ptr));
		}
		mdelay(10);
	}
}

static int snps_init_clock(struct sdhci_host *host)
{
	int div = 0;
	int mul = 0;
	int div_val = 0;
	int mul_val = 0;
	int mul_div_val = 0;
	int reg = 0;
	u32 mask = 0;

	/* Configure the BCLK DRP to get 100 MHZ Clock */

	/*
	 * To get 100MHz from 100MHz input freq,
	 * mul=2 and div=2
	 * Formula: output_clock = (input clock * mul) / div
	 */
	mul = 2;
	div = 2;
	mul_val = mul - 1;
	div_val = div - 1;
	/*
	 * Program the DCM DRP
	 * Step 1: Assert DCM Reset
	 * Step 2: Program the mul and div values in DRP
	 * Step 3: Read from DRP base 0x00 to restore DCM output as per
	 * www.xilinx.com/support/documentation/user_guides/ug191.pdf
	 * Step 4: De-Assert reset to DCM
	 */
	mask = SDHC_BCLK_DCM_RST;
	snps_reset_dcm(host, mask, 1);

	mul_div_val = (mul_val << 8) | div_val;
	sdhci_writew(host, mul_div_val, BCLK_DCM_MUL_DIV_DRP);

	reg = sdhci_readl(host, BCLK_DCM_DRP_BASE_51);

	snps_reset_dcm(host, mask, 0);

	/*
	 * By Default Clocks to Controller are OFF.
	 * Before stack applies reset; we need to turn on the clock
	 */
	sdhci_writew(host, SDHCI_CLOCK_INT_EN, SDHCI_CLOCK_CONTROL);

	return 0;

}
static struct sdhci_ops sdhci_pci_ops_snps = {
	.set_clock	= sdhci_set_clock_snps,
};

int sdhci_pci_probe_slot_snps(struct sdhci_pci_slot *slot)
{
	int ret = 0;
	struct sdhci_host *host;

	host = slot->host;
	sdhci_ops = host->ops;

	sdhci_pci_ops_snps.enable_dma		= sdhci_ops->enable_dma;
	sdhci_pci_ops_snps.set_bus_width	= sdhci_ops->set_bus_width;
	sdhci_pci_ops_snps.reset		= sdhci_ops->reset;
	sdhci_pci_ops_snps.set_uhs_signaling	= sdhci_ops->set_uhs_signaling;
	sdhci_pci_ops_snps.hw_reset		= sdhci_ops->hw_reset;

	host->ops = &sdhci_pci_ops_snps;

	/* Board specific clock initialization */
	ret = snps_init_clock(host);

	return ret;
}
EXPORT_SYMBOL_GPL(sdhci_pci_probe_slot_snps);
