/*
 * Copyright (c) 2016, Mellanox Technologies inc.  All rights reserved.
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

#include <rdma/ib_verbs.h>
#include "uverbs.h"
#include "rdma_core.h"

/*
 * lockless - the list shouldn't change. If disassociate is carrie out during
 * this, we'll wait until all current executing commands are finished.
 */
struct uverbs_uobject_type *uverbs_get_type(struct ib_device *ibdev,
					    uint16_t type)
{
	struct uverbs_uobject_type *uobj_type;

	list_for_each_entry(uobj_type, &ibdev->type_list, type_list) {
		if (uobj_type->obj_type == type)
			return uobj_type;
	}

	return NULL;
}

int ib_uverbs_uobject_type_add(struct list_head	*head,
			       void (*free)(struct uverbs_uobject_type *uobject_type,
					    struct ib_uobject *uobject,
					    struct ib_ucontext *ucontext),
			       uint16_t	obj_type)
{
	/*
	 * Allocate a new object type for the vendor, this should be done when a
	 * vendor is initialized.
	 */
	struct uverbs_uobject_type *uobject_type;

	uobject_type = kzalloc(sizeof(*uobject_type), GFP_KERNEL);
	if (!uobject_type)
		return -ENOMEM;

	uobject_type->free = free;
	uobject_type->obj_type = obj_type;
	list_add_tail(&uobject_type->type_list, head);
	return 0;
}
EXPORT_SYMBOL(ib_uverbs_uobject_type_add);

/* Should only be called when device is destroyed (remove_one?) */
static void ib_uverbs_uobject_type_remove(struct uverbs_uobject_type *uobject_type)
{
	/*
	 * Allocate a new object type for the vendor, this should be done when a
	 * vendor is initialized.
	 */
	WARN_ON(list_empty(&uobject_type->type_list));
	list_del(&uobject_type->type_list);
	kfree(uobject_type);
}
EXPORT_SYMBOL(ib_uverbs_uobject_type_remove);

/*
 * Done when device is destroyed. No one should touch the list or use its
 * elements here.
 */
void ib_uverbs_uobject_types_remove(struct ib_device *ib_dev)
{
	struct uverbs_uobject_type *iter, *temp;

	list_for_each_entry_safe(iter, temp, &ib_dev->type_list, type_list)
		ib_uverbs_uobject_type_remove(iter);
}
EXPORT_SYMBOL(ib_uverbs_uobject_types_remove);

