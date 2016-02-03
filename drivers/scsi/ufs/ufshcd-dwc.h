/*
 * UFS Host driver for Synopsys Designware Core
 *
 * Copyright (C) 2015-2016 Synopsys, Inc. (www.synopsys.com)
 *
 * Authors: Joao Pinto <jpinto@synopsys.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#ifndef _UFSHCD_DWC_H
#define _UFSHCD_DWC_H

void ufshcd_dwc_program_clk_div(struct ufs_hba *hba, u32);
int ufshcd_dwc_link_is_up(struct ufs_hba *hba);
int ufshcd_dwc_connection_setup(struct ufs_hba *hba);
int ufshcd_dwc_setup_20bit_rmmi_lane0(struct ufs_hba *hba);
int ufshcd_dwc_setup_20bit_rmmi_lane1(struct ufs_hba *hba);
int ufshcd_dwc_setup_20bit_rmmi(struct ufs_hba *hba);
int ufshcd_dwc_setup_40bit_rmmi(struct ufs_hba *hba);
int ufshcd_dwc_setup_mphy(struct ufs_hba *hba);
int ufshcd_dwc_configuration(struct ufs_hba *hba);

#endif /* End of Header */
