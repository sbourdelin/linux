/*
 * Synopsys DesignWare Multimedia Card Interface Platform driver
 *
 * Copyright (C) 2012, Samsung Electronics Co., Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef _DW_MMC_PLTFM_H_
#define _DW_MMC_PLTFM_H_

extern int dw_mci_pltfm_register(struct platform_device *pdev,
				const struct dw_mci_drv_data *drv_data);
extern int dw_mci_pltfm_remove(struct platform_device *pdev);
extern void dw_mci_pltfm_prepare_command(struct dw_mci *host, u32 *cmdr);
extern const struct dev_pm_ops dw_mci_pltfm_pmops;
#endif /* _DW_MMC_PLTFM_H_ */
