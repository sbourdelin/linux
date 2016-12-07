/*
 * Synopsys DesignWare Core Enterprise Ethernet (XLGMAC) Driver
 *
 * Copyright (C) 2015-2016 Synopsys, Inc. (www.synopsys.com)
 *
 * Author: Jie Deng <jiedeng@synopsys.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include "dwc-eth.h"
#include "dwc-eth-regacc.h"
#include "dwc-xlgmac.h"

static int xlgmac_mdio_wait_until_free(struct dwc_eth_pdata *pdata)
{
	unsigned int timeout;

	TRACE("-->");

	/* Wait till the bus is free */
	timeout = XLGMAC_MDIO_RD_TIMEOUT;
	while (DWC_ETH_IOREAD_BITS(pdata, MAC_MDIOSCCDR, BUSY) && timeout) {
		cpu_relax();
		timeout--;
	}

	DBGPR("  mido_rd_time=%#x\n", (XLGMAC_MDIO_RD_TIMEOUT - timeout));

	if (!timeout) {
		dev_err(pdata->dev, "timeout waiting for bus to be free\n");
		return -ETIMEDOUT;
	}

	TRACE("<--");

	return 0;
}

static int xlgmac_read_mmd_regs(struct dwc_eth_pdata *pdata,
				int prtad, int mmd_reg)
{
	int mmd_data;
	int ret;
	unsigned int scar;
	unsigned int sccdr = 0;

	TRACE("-->");

	mutex_lock(&pdata->pcs_mutex);

	ret = xlgmac_mdio_wait_until_free(pdata);
	if (ret)
		return ret;

	/* Updating desired bits for read operation */
	scar = DWC_ETH_IOREAD(pdata, MAC_MDIOSCAR);
	scar = scar & (0x3e00000UL);
	scar = scar | ((prtad) << MAC_MDIOSCAR_PA_POS) |
		((mmd_reg) << MAC_MDIOSCAR_RA_POS);
	DWC_ETH_IOWRITE(pdata, MAC_MDIOSCAR, scar);

	/* Initiate the read */
	sccdr = sccdr | ((0x1) << MAC_MDIOSCCDR_BUSY_POS) |
		((0x5) << MAC_MDIOSCCDR_CR_POS) |
		((0x1) << MAC_MDIOSCCDR_SADDR_POS) |
		((0x3) << MAC_MDIOSCCDR_CMD_POS);
	DWC_ETH_IOWRITE(pdata, MAC_MDIOSCCDR, sccdr);

	ret = xlgmac_mdio_wait_until_free(pdata);
	if (ret)
		return ret;

	/* Read the data */
	mmd_data = DWC_ETH_IOREAD_BITS(pdata, MAC_MDIOSCCDR, SDATA);

	mutex_unlock(&pdata->pcs_mutex);

	TRACE("<--");

	return mmd_data;
}

static int xlgmac_write_mmd_regs(struct dwc_eth_pdata *pdata,
				 int prtad, int mmd_reg, int mmd_data)
{
	int ret;
	unsigned int scar;
	unsigned int sccdr = 0;

	TRACE("-->");

	mutex_lock(&pdata->pcs_mutex);

	ret = xlgmac_mdio_wait_until_free(pdata);
	if (ret)
		return ret;

	/* Updating desired bits for write operation */
	scar = DWC_ETH_IOREAD(pdata, MAC_MDIOSCAR);
	scar = scar & (0x3e00000UL);
	scar = scar | ((prtad) << MAC_MDIOSCAR_PA_POS) |
		((mmd_reg) << MAC_MDIOSCAR_RA_POS);
	DWC_ETH_IOWRITE(pdata, MAC_MDIOSCAR, scar);

	/* Initiate Write */
	sccdr = sccdr | ((0x1) << MAC_MDIOSCCDR_BUSY_POS) |
		((0x5) << MAC_MDIOSCCDR_CR_POS) |
		((0x1) << MAC_MDIOSCCDR_SADDR_POS) |
		((0x1) << MAC_MDIOSCCDR_CMD_POS) |
		((mmd_data) << MAC_MDIOSCCDR_SDATA_POS);
	DWC_ETH_IOWRITE(pdata, MAC_MDIOSCCDR, sccdr);

	ret = xlgmac_mdio_wait_until_free(pdata);
	if (ret)
		return ret;

	mutex_unlock(&pdata->pcs_mutex);

	TRACE("<--");

	return 0;
}

static struct dwc_eth_hw_ops xlgmac_hw_ops = {
	.read_mmd_regs = xlgmac_read_mmd_regs,
	.write_mmd_regs = xlgmac_write_mmd_regs,
};

void xlgmac_init_hw_ops(struct dwc_eth_hw_ops *hw_ops)
{
	hw_ops = &xlgmac_hw_ops;
}
