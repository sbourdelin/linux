/* QLogic qed NIC Driver
 * Copyright (c) 2015 QLogic Corporation
 *
 * This software is available under the terms of the GNU General Public License
 * (GPL) Version 2, available from the file COPYING in the main directory of
 * this source tree.
 */
#ifndef __QEDR_USER_H__
#define __QEDR_USER_H__

#define QEDR_ABI_VERSION		(6)

/* user kernel communication data structures. */

struct qedr_alloc_ucontext_resp {
	u64 db_pa;
	u32 db_size;

	u32 max_send_wr;
	u32 max_recv_wr;
	u32 max_srq_wr;
	u32 sges_per_send_wr;
	u32 sges_per_recv_wr;
	u32 sges_per_srq_wr;
	int max_cqes;
};
#endif /* __QEDR_USER_H__ */
