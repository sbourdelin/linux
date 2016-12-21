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
 * Description: IB Verbs interpreter (header)
 */

#ifndef __BNXT_RE_IB_VERBS_H__
#define __BNXT_RE_IB_VERBS_H__

struct bnxt_re_pd {
	struct bnxt_re_dev	*rdev;
	struct ib_pd		ib_pd;
	struct bnxt_qplib_pd	qplib_pd;
	struct bnxt_qplib_dpi	dpi;
};

struct bnxt_re_ucontext {
	struct bnxt_re_dev	*rdev;
	struct ib_ucontext	ib_uctx;
	struct bnxt_qplib_dpi	*dpi;
	void			*shpg;
	spinlock_t		sh_lock;	/* protect shpg */
};

struct ib_pd *bnxt_re_alloc_pd(struct ib_device *ibdev,
			       struct ib_ucontext *context,
			       struct ib_udata *udata);
int bnxt_re_dealloc_pd(struct ib_pd *pd);
struct ib_ucontext *bnxt_re_alloc_ucontext(struct ib_device *ibdev,
					   struct ib_udata *udata);
int bnxt_re_dealloc_ucontext(struct ib_ucontext *context);
int bnxt_re_mmap(struct ib_ucontext *context, struct vm_area_struct *vma);
#endif /* __BNXT_RE_IB_VERBS_H__ */
