/*
 * Register definition file for Samsung MFC V10.x Interface (FIMV) driver
 *
 * Copyright (c) 2017 Samsung Electronics Co., Ltd.
 *     http://www.samsung.com/
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#ifndef _REGS_MFC_V10_H
#define _REGS_MFC_V10_H

#include <linux/sizes.h>
#include "regs-mfc-v8.h"

/* MFCv10 register definitions*/
#define S5P_FIMV_MFC_CLOCK_OFF_V10			0x7120
#define S5P_FIMV_MFC_STATE_V10				0x7124

/* MFCv10 Context buffer sizes */
#define MFC_CTX_BUF_SIZE_V10		(30 * SZ_1K)	/* 30KB */
#define MFC_H264_DEC_CTX_BUF_SIZE_V10	(2 * SZ_1M)	/* 2MB */
#define MFC_OTHER_DEC_CTX_BUF_SIZE_V10	(20 * SZ_1K)	/* 20KB */
#define MFC_H264_ENC_CTX_BUF_SIZE_V10	(100 * SZ_1K)	/* 100KB */
#define MFC_OTHER_ENC_CTX_BUF_SIZE_V10	(15 * SZ_1K)	/* 15KB */

/* MFCv10 variant defines */
#define MAX_FW_SIZE_V10		(SZ_1M)		/* 1MB */
#define MAX_CPB_SIZE_V10	(3 * SZ_1M)	/* 3MB */
#define MFC_VERSION_V10		0xA0
#define MFC_NUM_PORTS_V10	1

#endif /*_REGS_MFC_V10_H*/

