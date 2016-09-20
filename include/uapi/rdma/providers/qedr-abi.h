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

struct qedr_alloc_pd_ureq {
       u64 rsvd1;
};

struct qedr_alloc_pd_uresp {
       u32 pd_id;
};

struct qedr_create_cq_ureq {
       uint64_t addr;
       size_t len;
};

struct qedr_create_cq_uresp {
       u32 db_offset;
       u16 icid;
};

struct qedr_create_qp_ureq {
       u32 qp_handle_hi;
       u32 qp_handle_lo;

       /* SQ */
       /* user space virtual address of SQ buffer */
       u64 sq_addr;

       /* length of SQ buffer */
       size_t sq_len;

       /* RQ */
       /* user space virtual address of RQ buffer */
       u64 rq_addr;

       /* length of RQ buffer */
       size_t rq_len;
};

struct qedr_create_qp_uresp {
       u32 qp_id;
       int atomic_supported;

       /* SQ */
       u32 sq_db_offset;
       u16 sq_icid;

       /* RQ */
       u32 rq_db_offset;
       u16 rq_icid;

       u32 rq_db2_offset;
};

#endif /* __QEDR_USER_H__ */

