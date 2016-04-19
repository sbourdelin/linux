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
#include "sdhci-dwc-mshc-pci.h"

/*****************************************************************************\
 *                                                                           *
 * Hardware specific clock handling                                          *
 *                                                                           *
\*****************************************************************************/
static const struct sdhci_ops *sdhci_ops;	/* Low level hw interface */

static void sdhci_set_clock_snps(struct sdhci_host *host,
						unsigned int clock)
{
	int div = 0;
	int mul = 0;
	int div_val = 0;
	int mul_val = 0;
	int mul_div_val = 0;
	int reg = 0;
	u16 clk = 0;
	u32 vendor_ptr;
	unsigned long timeout;
	u32 tx_clk_phase_val = SDHC_DEF_TX_CLK_PH_VAL;
	u32 rx_clk_phase_val = SDHC_DEF_RX_CLK_PH_VAL;

	/* DWC MSHC Specific clock settings */

	/* if clock is less than 25MHz, divided clock is used.
	 * For divided clock, we can use the standard sdhci_set_clock().
	 * For clock above 25MHz, DRP clock is used
	 * Here, we cannot use sdhci_set_clock(), we need to program
	 * TX RX CLOCK DCM DRP for appropriate clock
	 */

	vendor_ptr = sdhci_readw(host, SDHCI_UHS2_VENDOR);

	if (clock <= 25000000) {
		/* Then call generic set_clock */
		sdhci_ops->set_clock(host, clock);
	} else {

		host->mmc->actual_clock = 0;

		/* Select un-phase shifted clock before reset Tx Tuning DCM*/
		reg = sdhci_readl(host, (SDHC_GPIO_OUT + vendor_ptr));
		reg &= ~SDHC_TX_CLK_SEL_TUNED;
		sdhci_writew(host, reg, (SDHC_GPIO_OUT + vendor_ptr));
		mdelay(10);

		sdhci_writew(host, 0, SDHCI_CLOCK_CONTROL);

		if (clock == 0)
			return;

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
		/* Program the DCM DRP */
		/* Step 1: Assert DCM Reset */
		reg = sdhci_readl(host, (SDHC_GPIO_OUT + vendor_ptr));
		reg = reg | SDHC_CARD_TX_CLK_DCM_RST;
		sdhci_writew(host, reg, (SDHC_GPIO_OUT + vendor_ptr));

		/* Step 2: Program the mul and div values in DRP */
		mul_div_val = (mul_val << 8) | div_val;
		sdhci_writew(host, mul_div_val, TXRX_CLK_DCM_MUL_DIV_DRP);

		/* Step 3: issue a dummy read from DRP base 0x00 as per
		 * www.xilinx.com/support/documentation/user_guides/ug191.pdf
		 */
		reg = sdhci_readw(host, TXRX_CLK_DCM_DRP_BASE_51);

		/* Step 4: De-Assert reset to DCM  */
		reg = sdhci_readl(host, (SDHC_GPIO_OUT + vendor_ptr));
		reg &= ~SDHC_CARD_TX_CLK_DCM_RST;
		sdhci_writew(host, reg, (SDHC_GPIO_OUT + vendor_ptr));

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

		/* For some bit-files we may have to do phase shifting for
		 * Tx Clock; Let's do it here
		 */
		reg = sdhci_readl(host, (SDHC_GPIO_OUT + vendor_ptr));
		reg = reg | SDHC_TUNING_TX_CLK_DCM_RST;
		sdhci_writew(host, reg, (SDHC_GPIO_OUT + vendor_ptr));
		mdelay(10);
		reg = sdhci_readl(host, (SDHC_GPIO_OUT + vendor_ptr));
		reg &= ~SDHC_TUNING_TX_CLK_DCM_RST;
		sdhci_writew(host, reg, (SDHC_GPIO_OUT + vendor_ptr));

		/* Select working phase value if clock is <= 50MHz */
		if (clock <= 50000000) {
			/*Change the Phase value here */
			reg = sdhci_readl(host, (SDHC_GPIO_OUT + vendor_ptr));
			reg = reg | (SDHC_TUNING_TX_CLK_SEL_MASK &
			   (tx_clk_phase_val << SDHC_TUNING_TX_CLK_SEL_SHIFT));
			sdhci_writew(host, reg, (SDHC_GPIO_OUT + vendor_ptr));
			mdelay(10);

			/* Program to select phase shifted clock */
			reg = sdhci_readl(host, (SDHC_GPIO_OUT + vendor_ptr));
			reg = reg | SDHC_TX_CLK_SEL_TUNED;
			sdhci_writew(host, reg, (SDHC_GPIO_OUT + vendor_ptr));
			mdelay(10);
		}

		/* This Clock might have affected the RX CLOCK DCM
		 * used for Phase control; Reset this DCM for proper clock
		 */

		/* Program the DCM DRP */
		/* Step 1: Reset the DCM */
		reg = sdhci_readl(host, (SDHC_GPIO_OUT + vendor_ptr));
		reg = reg | SDHC_TUNING_RX_CLK_DCM_RST;
		sdhci_writew(host, reg, (SDHC_GPIO_OUT + vendor_ptr));
		mdelay(10);
		/* Step 2: De-Assert reset to DCM  */
		reg = sdhci_readl(host, (SDHC_GPIO_OUT + vendor_ptr));
		reg &= ~SDHC_TUNING_RX_CLK_DCM_RST;
		sdhci_writew(host, reg, (SDHC_GPIO_OUT + vendor_ptr));

		/* For 50Mhz, tuning is not possible.. Lets fix the sampling
		 * Phase of Rx Clock here
		 */
		if (clock <= 50000000) {
			/*Change the Phase value here */
			reg = sdhci_readl(host, (SDHC_DBOUNCE + vendor_ptr));
			reg &= ~SDHC_TUNING_RX_CLK_SEL_MASK;
			reg = reg | (SDHC_TUNING_RX_CLK_SEL_MASK &
					rx_clk_phase_val);
			sdhci_writew(host, reg, (SDHC_DBOUNCE + vendor_ptr));
		}
		mdelay(10);
	}
}

static int sdhci_pci_enable_dma_snps(struct sdhci_host *host)
{
	/* DWC MSHC Specific Dma Enabling */

	/* Call generic emable_dma */
	return sdhci_ops->enable_dma(host);
}

static void sdhci_pci_set_bus_width_snps(struct sdhci_host *host, int width)
{
	/* DWC MSHC Specific Bus Width Setting */

	/* Call generic set_bus_width */
	sdhci_ops->set_bus_width(host, width);
}

static void sdhci_reset_snps(struct sdhci_host *host, u8 mask)
{
	/* DWC MSHC Specific hci reset */

	/* Call generic reset */
	sdhci_ops->reset(host, mask);
}
static void sdhci_set_uhs_signaling_snps(struct sdhci_host *host,
	unsigned int timing)
{
	/* DWC MSHC Specific UHS-I Signaling */

	/* Call generic UHS-I signaling */
	sdhci_ops->set_uhs_signaling(host, timing);
}
static void sdhci_pci_hw_reset_snps(struct sdhci_host *host)
{
	/* DWC MSHC Specific hw reset */

	/* Call generic hw_reset */
	if (host->ops && host->ops->hw_reset)
		sdhci_ops->hw_reset(host);
}
static const struct sdhci_ops sdhci_pci_ops_snps = {
	.set_clock	= sdhci_set_clock_snps,
	.enable_dma	= sdhci_pci_enable_dma_snps,
	.set_bus_width	= sdhci_pci_set_bus_width_snps,
	.reset		= sdhci_reset_snps,
	.set_uhs_signaling = sdhci_set_uhs_signaling_snps,
	.hw_reset	   = sdhci_pci_hw_reset_snps,
};

static int snps_init_clock(struct sdhci_host *host)
{
	int div = 0;
	int mul = 0;
	int div_val = 0;
	int mul_val = 0;
	int mul_div_val = 0;
	int reg = 0;
	u32 vendor_ptr;

	vendor_ptr = sdhci_readw(host, SDHCI_UHS2_VENDOR);

	/* Configure the BCLK DRP to get 100 MHZ Clock */

	/* To get 100MHz from 100MHz input freq,
	 * mul=2 and div=2
	 * Formula: output_clock = (input clock * mul) / div
	 */
	mul = 2;
	div = 2;
	mul_val = mul - 1;
	div_val = div - 1;
	/* Program the DCM DRP */
	/* Step 1: Reset the DCM */
	reg = sdhci_readl(host, (SDHC_GPIO_OUT + vendor_ptr));
	reg = reg | SDHC_BCLK_DCM_RST;
	sdhci_writew(host, reg, (SDHC_GPIO_OUT + vendor_ptr));
	/* Step 2: Program the mul and div values in DRP */
	mul_div_val = (mul_val << 8) | div_val;
	sdhci_writew(host, mul_div_val, BCLK_DCM_MUL_DIV_DRP);
	/* Step 3: issue a dummy read from DRP base 0x00 as per
	 * http://www.xilinx.com/support/documentation/user_guides/ug191.pdf
	 */
	reg = sdhci_readw(host, BCLK_DCM_DRP_BASE_51);
	/* Step 4: De-Assert reset to DCM  */
	/* de assert reset*/
	reg = sdhci_readl(host, (SDHC_GPIO_OUT + vendor_ptr));
	reg &= ~SDHC_BCLK_DCM_RST;
	sdhci_writew(host, reg, (SDHC_GPIO_OUT + vendor_ptr));

	/* By Default Clocks to MSHC are off..
	 * Before stack applies reset; we need to turn on the clock
	 */
	sdhci_writew(host, SDHCI_CLOCK_INT_EN, SDHCI_CLOCK_CONTROL);

	return 0;

}

int sdhci_pci_probe_slot_snps(struct sdhci_pci_slot *slot)
{
	int ret = 0;
	struct sdhci_host *host;

	host = slot->host;
	sdhci_ops = host->ops;
	host->ops = &sdhci_pci_ops_snps;

	/* Board specific clock initialization */
	ret = snps_init_clock(host);
	return ret;
}
EXPORT_SYMBOL_GPL(sdhci_pci_probe_slot_snps);
