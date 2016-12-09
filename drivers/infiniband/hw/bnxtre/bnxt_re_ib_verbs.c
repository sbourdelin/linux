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
 * Description: IB Verbs interpreter
 */

#include <linux/interrupt.h>
#include <linux/types.h>
#include <linux/pci.h>
#include <linux/netdevice.h>

#include <rdma/ib_verbs.h>
#include <rdma/ib_user_verbs.h>
#include <rdma/ib_umem.h>
#include <rdma/ib_addr.h>
#include <rdma/ib_mad.h>
#include <rdma/ib_cache.h>

#include "bnxt_ulp.h"

#include "bnxt_re_hsi.h"
#include "bnxt_qplib_res.h"
#include "bnxt_qplib_sp.h"
#include "bnxt_qplib_fp.h"
#include "bnxt_qplib_rcfw.h"

#include "bnxt_re.h"
#include "bnxt_re_ib_verbs.h"
#include <rdma/bnxt_re_uverbs_abi.h>

/* Protection Domains */
int bnxt_re_dealloc_pd(struct ib_pd *ib_pd)
{
	struct bnxt_re_pd *pd = to_bnxt_re(ib_pd, struct bnxt_re_pd, ib_pd);
	struct bnxt_re_dev *rdev = pd->rdev;
	int rc;

	if (ib_pd->uobject && pd->dpi.dbr) {
		struct ib_ucontext *ib_uctx = ib_pd->uobject->context;
		struct bnxt_re_ucontext *ucntx;

		/* Free DPI only if this is the first PD allocated by the
		 * application and mark the context dpi as NULL
		 */
		ucntx = to_bnxt_re(ib_uctx, struct bnxt_re_ucontext, ib_uctx);

		rc = bnxt_qplib_dealloc_dpi(&rdev->qplib_res,
					    &rdev->qplib_res.dpi_tbl,
					    &pd->dpi);
		if (rc)
			dev_err(rdev_to_dev(rdev), "Failed to deallocate HW DPI");
			/* Don't fail, continue*/
		ucntx->dpi = NULL;
	}

	rc = bnxt_qplib_dealloc_pd(&rdev->qplib_res,
				   &rdev->qplib_res.pd_tbl,
				   &pd->qplib_pd);
	if (rc) {
		dev_err(rdev_to_dev(rdev), "Failed to deallocate HW PD");
		return rc;
	}

	kfree(pd);
	return 0;
}

struct ib_pd *bnxt_re_alloc_pd(struct ib_device *ibdev,
			       struct ib_ucontext *ucontext,
			       struct ib_udata *udata)
{
	struct bnxt_re_dev *rdev = to_bnxt_re_dev(ibdev, ibdev);
	struct bnxt_re_ucontext *ucntx = to_bnxt_re(ucontext,
						    struct bnxt_re_ucontext,
						    ib_uctx);
	struct bnxt_re_pd *pd;
	int rc;

	pd = kzalloc(sizeof(*pd), GFP_KERNEL);
	if (!pd)
		return ERR_PTR(-ENOMEM);

	pd->rdev = rdev;
	if (bnxt_qplib_alloc_pd(&rdev->qplib_res.pd_tbl, &pd->qplib_pd)) {
		dev_err(rdev_to_dev(rdev), "Failed to allocate HW PD");
		rc = -ENOMEM;
		goto fail;
	}

	if (udata) {
		struct bnxt_re_pd_resp resp;

		if (!ucntx->dpi) {
			/* Allocate DPI in alloc_pd to avoid failing of
			 * ibv_devinfo and family of application when DPIs
			 * are depleted.
			 */
			if (bnxt_qplib_alloc_dpi(&rdev->qplib_res.dpi_tbl,
						 &pd->dpi, ucntx)) {
				rc = -ENOMEM;
				goto dbfail;
			}
			ucntx->dpi = &pd->dpi;
		}

		resp.pdid = pd->qplib_pd.id;
		/* Still allow mapping this DBR to the new user PD. */
		resp.dpi = ucntx->dpi->dpi;
		resp.dbr = (u64)ucntx->dpi->umdbr;

		rc = ib_copy_to_udata(udata, &resp, sizeof(resp));
		if (rc) {
			dev_err(rdev_to_dev(rdev),
				"Failed to copy user response\n");
			goto dbfail;
		}
	}

	return &pd->ib_pd;
dbfail:
	(void)bnxt_qplib_dealloc_pd(&rdev->qplib_res, &rdev->qplib_res.pd_tbl,
				    &pd->qplib_pd);
fail:
	kfree(pd);
	return ERR_PTR(rc);
}

struct ib_ucontext *bnxt_re_alloc_ucontext(struct ib_device *ibdev,
					   struct ib_udata *udata)
{
	struct bnxt_re_dev *rdev = to_bnxt_re_dev(ibdev, ibdev);
	struct bnxt_re_uctx_resp resp;
	struct bnxt_re_ucontext *uctx;
	struct bnxt_qplib_dev_attr *dev_attr = &rdev->dev_attr;
	int rc;

	dev_dbg(rdev_to_dev(rdev), "ABI version requested %d",
		ibdev->uverbs_abi_ver);

	if (ibdev->uverbs_abi_ver != BNXT_RE_ABI_VERSION) {
		dev_dbg(rdev_to_dev(rdev), " is different from the device %d ",
			BNXT_RE_ABI_VERSION);
		return ERR_PTR(-EPERM);
	}

	uctx = kzalloc(sizeof(*uctx), GFP_KERNEL);
	if (!uctx)
		return ERR_PTR(-ENOMEM);

	uctx->rdev = rdev;

	uctx->shpg = (void *)__get_free_page(GFP_KERNEL);
	if (!uctx->shpg) {
		rc = -ENOMEM;
		goto fail;
	}
	spin_lock_init(&uctx->sh_lock);

	resp.dev_id = rdev->en_dev->pdev->devfn; /*Temp, Use idr_alloc instead*/
	resp.max_qp = rdev->qplib_ctx.qpc_count;
	resp.pg_size = PAGE_SIZE;
	resp.cqe_sz = sizeof(struct cq_base);
	resp.max_cqd = dev_attr->max_cq_wqes;

	rc = ib_copy_to_udata(udata, &resp, sizeof(resp));
	if (rc) {
		dev_err(rdev_to_dev(rdev), "Failed to copy user context");
		rc = -EFAULT;
		goto cfail;
	}

	return &uctx->ib_uctx;
cfail:
	free_page((u64)uctx->shpg);
	uctx->shpg = NULL;
fail:
	kfree(uctx);
	return ERR_PTR(rc);
}

int bnxt_re_dealloc_ucontext(struct ib_ucontext *ib_uctx)
{
	struct bnxt_re_ucontext *uctx = to_bnxt_re(ib_uctx,
						   struct bnxt_re_ucontext,
						   ib_uctx);
	if (uctx->shpg)
		free_page((u64)uctx->shpg);
	kfree(uctx);
	return 0;
}

/* Helper function to mmap the virtual memory from user app */
int bnxt_re_mmap(struct ib_ucontext *ib_uctx, struct vm_area_struct *vma)
{
	struct bnxt_re_ucontext *uctx = to_bnxt_re(ib_uctx,
						   struct bnxt_re_ucontext,
						   ib_uctx);
	struct bnxt_re_dev *rdev = uctx->rdev;
	u64 pfn;

	if (vma->vm_end - vma->vm_start != PAGE_SIZE)
		return -EINVAL;

	if (vma->vm_pgoff) {
		vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
		if (io_remap_pfn_range(vma, vma->vm_start, vma->vm_pgoff,
				       PAGE_SIZE, vma->vm_page_prot)) {
			dev_err(rdev_to_dev(rdev), "Failed to map DPI");
			return -EAGAIN;
		}
	} else {
		pfn = virt_to_phys(uctx->shpg) >> PAGE_SHIFT;
		if (remap_pfn_range(vma, vma->vm_start,
				    pfn, PAGE_SIZE, vma->vm_page_prot)) {
			dev_err(rdev_to_dev(rdev),
				"Failed to map shared page");
			return -EAGAIN;
		}
	}

	return 0;
}
