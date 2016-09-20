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

struct ib_ucontext *qedr_alloc_ucontext(struct ib_device *, struct ib_udata *);
int qedr_dealloc_ucontext(struct ib_ucontext *);

int qedr_mmap(struct ib_ucontext *, struct vm_area_struct *vma);
int qedr_del_gid(struct ib_device *device, u8 port_num,
		 unsigned int index, void **context);
int qedr_add_gid(struct ib_device *device, u8 port_num,
		 unsigned int index, const union ib_gid *gid,
		 const struct ib_gid_attr *attr, void **context);
#endif
