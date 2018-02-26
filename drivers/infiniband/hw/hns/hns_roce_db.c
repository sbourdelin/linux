/* SPDX-License-Identifier: (GPL-2.0 WITH Linux-syscall-note) OR BSD-2-Clause */
/*
 * Copyright (c) 2017 Hisilicon Limited.
 * Copyright (c) 2007, 2008 Mellanox Technologies. All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <linux/platform_device.h>
#include <rdma/ib_umem.h>
#include "hns_roce_device.h"

int hns_roce_db_map_user(struct hns_roce_ucontext *context, unsigned long virt,
			 struct hns_roce_db *db)
{
	struct hns_roce_user_db_page *page;
	int ret = 0;

	mutex_lock(&context->page_mutex);

	list_for_each_entry(page, &context->page_list, list)
		if (page->user_virt == (virt & PAGE_MASK))
			goto found;

	page = kmalloc(sizeof(*page), GFP_KERNEL);
	if (!page) {
		ret = -ENOMEM;
		goto out;
	}

	page->user_virt = (virt & PAGE_MASK);
	page->refcount_t = 0;
	page->umem = ib_umem_get(&context->ibucontext, virt & PAGE_MASK,
				 PAGE_SIZE, 0, 0);
	if (IS_ERR(page->umem)) {
		ret = PTR_ERR(page->umem);
		kfree(page);
		goto out;
	}

	list_add(&page->list, &context->page_list);

found:
	db->dma = sg_dma_address(page->umem->sg_head.sgl) +
		  (virt & ~PAGE_MASK);
	db->u.user_page = page;
	++page->refcount_t;

out:
	mutex_unlock(&context->page_mutex);

	return ret;
}
EXPORT_SYMBOL(hns_roce_db_map_user);

void hns_roce_db_unmap_user(struct hns_roce_ucontext *context,
			    struct hns_roce_db *db)
{
	mutex_lock(&context->page_mutex);

	if (!--db->u.user_page->refcount_t) {
		list_del(&db->u.user_page->list);
		ib_umem_release(db->u.user_page->umem);
		kfree(db->u.user_page);
	}

	mutex_unlock(&context->page_mutex);
}
EXPORT_SYMBOL(hns_roce_db_unmap_user);
