/* SPDX-License-Identifier: GPL-2.0 */
/* Octeon III Schedule/Synchronize/Order Unit (SSO)
 *
 * Copyright (C) 2018 Cavium, Inc.
 */
#ifndef _OCTEON3_SSO_H_
#define _OCTEON3_SSO_H_

#include <linux/bitops.h>

#define SSO_BASE		0x1670000000000ull
#define SSO_ADDR(n)		(SSO_BASE + SET_XKPHYS + NODE_OFFSET(n))
#define SSO_AQ_ADDR(n, a)	(SSO_ADDR(n) + ((a) << 3))
#define SSO_GRP_ADDR(n, g)	(SSO_ADDR(n) + ((g) << 16))

#define SSO_AW_STATUS(n)	(SSO_ADDR(n) + 0x000010e0)
#define SSO_AW_CFG(n)		(SSO_ADDR(n) + 0x000010f0)
#define SSO_ERR0(n)		(SSO_ADDR(n) + 0x00001240)
#define SSO_TAQ_ADD(n)		(SSO_ADDR(n) + 0x000020e0)
#define SSO_XAQ_AURA(n)		(SSO_ADDR(n) + 0x00002100)

#define SSO_XAQ_HEAD_PTR(n, a)	(SSO_AQ_ADDR(n, a) + 0x00080000)
#define SSO_XAQ_TAIL_PTR(n, a)	(SSO_AQ_ADDR(n, a) + 0x00090000)
#define SSO_XAQ_HEAD_NEXT(n, a)	(SSO_AQ_ADDR(n, a) + 0x000a0000)
#define SSO_XAQ_TAIL_NEXT(n, a)	(SSO_AQ_ADDR(n, a) + 0x000b0000)

#define SSO_GRP_TAQ_THR(n, g)	(SSO_GRP_ADDR(n, g) + 0x20000100)
#define SSO_GRP_PRI(n, g)	(SSO_GRP_ADDR(n, g) + 0x20000200)
#define SSO_GRP_INT(n, g)	(SSO_GRP_ADDR(n, g) + 0x20000400)
#define SSO_GRP_INT_THR(n, g)	(SSO_GRP_ADDR(n, g) + 0x20000500)
#define SSO_GRP_AQ_CNT(n, g)	(SSO_GRP_ADDR(n, g) + 0x20000700)

/* SSO interrupt numbers start here */
#define SSO_IRQ_START		0x61000

#define SSO_AW_STATUS_XAQ_BU_CACHED_MASK	GENMASK_ULL(5, 0)

#define SSO_AW_CFG_XAQ_ALOC_DIS		BIT(6)
#define SSO_AW_CFG_XAQ_BYP_DIS		BIT(4)
#define SSO_AW_CFG_STT			BIT(3)
#define SSO_AW_CFG_LDT			BIT(2)
#define SSO_AW_CFG_LDWB			BIT(1)
#define SSO_AW_CFG_RWEN			BIT(0)

#define SSO_ERR0_FPE			BIT(0)

#define SSO_TAQ_ADD_RSVD_FREE_SHIFT	16

#define SSO_XAQ_AURA_NODE_SHIFT		10

#define SSO_XAQ_PTR_MASK		GENMASK_ULL(41, 7)

#define SSO_GRP_TAQ_THR_MAX_THR_MASK	GENMASK_ULL(42, 32)
#define SSO_GRP_TAQ_THR_RSVD_THR_MASK	GENMASK_ULL(10, 0)
#define SSO_GRP_TAQ_THR_RSVD_THR_SHIFT	32

#define SSO_GRP_PRI_WEIGHT_MAXIMUM	63
#define SSO_GRP_PRI_WEIGHT_SHIFT	16

#define SSO_GRP_INT_EXE_INT		BIT(1)

#define SSO_GRP_AQ_CNT_AQ_CNT_MASK	GENMASK_ULL(32, 0)

/* SSO tag types */
#define SSO_TAG_TYPE_ORDERED            0ull
#define SSO_TAG_TYPE_ATOMIC             1ull
#define SSO_TAG_TYPE_UNTAGGED           2ull
#define SSO_TAG_TYPE_EMPTY              3ull
#define SSO_TAG_SWDID			0x60ull


/* SSO work queue bitfields */
#define SSO_GET_WORK_DID_SHIFT		40
#define SSO_GET_WORK_NODE_SHIFT		36
#define SSO_GET_WORK_GROUPED		BIT(30)
#define SSO_GET_WORK_RTNGRP		BIT(29)
#define SSO_GET_WORK_IDX_GRP_MASK_SHIFT	4
#define SSO_GET_WORK_WAITW_WAIT		BIT(3)
#define SSO_GET_WORK_WAITW_NO_WAIT	0ull

#define SSO_GET_WORK_DMA_S_SCRADDR	BIT(63)
#define SSO_GET_WORK_DMA_S_LEN_SHIFT	48
#define SSO_GET_WORK_LD_S_IO		BIT(48)
#define SSO_GET_WORK_RTN_S_NO_WORK	BIT(63)
#define SSO_GET_WORK_RTN_S_GRP_MASK	GENMASK_ULL(57, 48)
#define SSO_GET_WORK_RTN_S_GRP_SHIFT	48
#define SSO_GET_WORK_RTN_S_WQP_MASK	GENMASK_ULL(41, 0)

#endif /* _OCTEON3_SSO_H_ */
