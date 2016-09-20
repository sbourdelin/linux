/* QLogic qed NIC Driver
 * Copyright (c) 2015 QLogic Corporation
 *
 * This software is available under the terms of the GNU General Public License
 * (GPL) Version 2, available from the file COPYING in the main directory of
 * this source tree.
 */
#ifndef __QEDR_VERBS_H__
#define __QEDR_VERBS_H__

int qedr_query_device(struct ib_device *ibdev,
		      struct ib_device_attr *attr, struct ib_udata *udata);
int qedr_query_port(struct ib_device *, u8 port, struct ib_port_attr *props);

int qedr_query_gid(struct ib_device *, u8 port, int index, union ib_gid *gid);

int qedr_query_pkey(struct ib_device *, u8 port, u16 index, u16 *pkey);

struct ib_ucontext *qedr_alloc_ucontext(struct ib_device *, struct ib_udata *);
int qedr_dealloc_ucontext(struct ib_ucontext *);

int qedr_mmap(struct ib_ucontext *, struct vm_area_struct *vma);
int qedr_del_gid(struct ib_device *device, u8 port_num,
		 unsigned int index, void **context);
int qedr_add_gid(struct ib_device *device, u8 port_num,
		 unsigned int index, const union ib_gid *gid,
		 const struct ib_gid_attr *attr, void **context);
struct ib_pd *qedr_alloc_pd(struct ib_device *,
			    struct ib_ucontext *, struct ib_udata *);
int qedr_dealloc_pd(struct ib_pd *pd);

struct ib_cq *qedr_create_cq(struct ib_device *ibdev,
			     const struct ib_cq_init_attr *attr,
			     struct ib_ucontext *ib_ctx,
			     struct ib_udata *udata);
int qedr_resize_cq(struct ib_cq *, int cqe, struct ib_udata *);
int qedr_destroy_cq(struct ib_cq *);
int qedr_arm_cq(struct ib_cq *ibcq, enum ib_cq_notify_flags flags);
struct ib_qp *qedr_create_qp(struct ib_pd *, struct ib_qp_init_attr *attrs,
			     struct ib_udata *);
int qedr_modify_qp(struct ib_qp *, struct ib_qp_attr *attr,
		   int attr_mask, struct ib_udata *udata);
int qedr_query_qp(struct ib_qp *, struct ib_qp_attr *qp_attr,
		  int qp_attr_mask, struct ib_qp_init_attr *);
int qedr_destroy_qp(struct ib_qp *ibqp);

#endif
