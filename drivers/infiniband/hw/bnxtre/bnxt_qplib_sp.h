/*
 * Broadcom NetXtreme-E RoCE driver.
 *
 * Copyright (c) 2016, Broadcom. All rights reserved.  The term
 * Broadcom refers to Broadcom Limited and/or its subsidiaries.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * BSD license below:
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * Description: Slow Path Operators (header)
 *
 */

#ifndef __BNXT_QPLIB_SP_H__
#define __BNXT_QPLIB_SP_H__

struct bnxt_qplib_dev_attr {
	char				fw_ver[32];
	u16				max_sgid;
	u16				max_mrw;
	u32				max_qp;
#define BNXT_QPLIB_MAX_OUT_RD_ATOM	126
	u32				max_qp_rd_atom;
	u32				max_qp_init_rd_atom;
	u32				max_qp_wqes;
	u32				max_qp_sges;
	u32				max_cq;
	u32				max_cq_wqes;
	u32				max_cq_sges;
	u32				max_mr;
	u64				max_mr_size;
	u32				max_pd;
	u32				max_mw;
	u32				max_raw_ethy_qp;
	u32				max_ah;
	u32				max_fmr;
	u32				max_map_per_fmr;
	u32				max_srq;
	u32				max_srq_wqes;
	u32				max_srq_sges;
	u32				max_pkey;
	u32				max_inline_data;
	u32				l2_db_size;
	u8				tqm_alloc_reqs[MAX_TQM_ALLOC_REQ];
};

struct bnxt_qplib_pd {
	u32				id;
};

struct bnxt_qplib_gid {
	u8				data[16];
};

int bnxt_qplib_get_sgid(struct bnxt_qplib_res *res,
			struct bnxt_qplib_sgid_tbl *sgid_tbl, int index,
			struct bnxt_qplib_gid *gid);
int bnxt_qplib_del_sgid(struct bnxt_qplib_sgid_tbl *sgid_tbl,
			struct bnxt_qplib_gid *gid, bool update);
int bnxt_qplib_add_sgid(struct bnxt_qplib_sgid_tbl *sgid_tbl,
			struct bnxt_qplib_gid *gid, u8 *mac, u16 vlan_id,
			bool update, u32 *index);
int bnxt_qplib_get_pkey(struct bnxt_qplib_res *res,
			struct bnxt_qplib_pkey_tbl *pkey_tbl, u16 index,
			u16 *pkey);
int bnxt_qplib_del_pkey(struct bnxt_qplib_res *res,
			struct bnxt_qplib_pkey_tbl *pkey_tbl, u16 *pkey,
			bool update);
int bnxt_qplib_add_pkey(struct bnxt_qplib_res *res,
			struct bnxt_qplib_pkey_tbl *pkey_tbl, u16 *pkey,
			bool update);
int bnxt_qplib_get_dev_attr(struct bnxt_qplib_rcfw *rcfw,
			    struct bnxt_qplib_dev_attr *attr);
#endif /* __BNXT_QPLIB_SP_H__*/
