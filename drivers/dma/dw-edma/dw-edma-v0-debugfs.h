/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2018 Synopsys, Inc. and/or its affiliates.
 * Synopsys DesignWare eDMA v0 core
 */

#ifndef _DW_EDMA_V0_DEBUG_FS_H
#define _DW_EDMA_V0_DEBUG_FS_H

#include <linux/dma/edma.h>

#ifdef CONFIG_DEBUG_FS
int dw_edma_v0_debugfs_on(struct dw_edma_chip *chip);
void dw_edma_v0_debugfs_off(void);
#else
static inline int dw_edma_v0_debugfs_on(struct dw_edma_chip *chip);
{
	return 0;
}

static inline void dw_edma_v0_debugfs_off(void);
#endif /* CONFIG_DEBUG_FS */

#endif /* _DW_EDMA_V0_DEBUG_FS_H */
