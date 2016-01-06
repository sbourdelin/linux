/*
 * Copyright (c) 2015 Mellanox Technologies Ltd. All rights reserved.
 * Copyright (c) 2015 System Fabric Works, Inc. All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *	   Redistribution and use in source and binary forms, with or
 *	   without modification, are permitted provided that the following
 *	   conditions are met:
 *
 *		- Redistributions of source code must retain the above
 *		  copyright notice, this list of conditions and the following
 *		  disclaimer.
 *
 *		- Redistributions in binary form must reproduce the above
 *		  copyright notice, this list of conditions and the following
 *		  disclaimer in the documentation and/or other materials
 *		  provided with the distribution.
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

#ifndef RVT_POOL_H
#define RVT_POOL_H

struct rvt_type_info {
	char			*name;
	size_t			size;
	void			(*cleanup)(void *obj);
	enum rvt_pool_flags	flags;
	u32			max_index;
	u32			min_index;
	size_t			key_offset;
	size_t			key_size;
	struct kmem_cache	*cache;
};

extern struct rvt_type_info rvt_type_info[];

/* initialize slab caches for managed objects */
int __init rvt_cache_init(void);

/* cleanup slab caches for managed objects */
void __exit rvt_cache_exit(void);

/* initialize a pool of objects with given limit on
   number of elements. gets parameters from rvt_type_info
   pool elements will be allocated out of a slab cache */
int rvt_pool_init(struct rvt_dev *rvt, struct rvt_pool *pool,
		  enum rvt_elem_type type, u32 max_elem);

/* free resources from object pool */
int rvt_pool_cleanup(struct rvt_pool *pool);

/* allocate an object from pool */
void *rvt_alloc(struct rvt_pool *pool);

/* assign an index to an indexed object and insert object into
   pool's rb tree */
void rvt_add_index(void *elem);

/* drop an index and remove object from rb tree */
void rvt_drop_index(void *elem);

/* assign a key to a keyed object and insert object into
   pool's rb tree */
void rvt_add_key(void *elem, void *key);

/* remove elem from rb tree */
void rvt_drop_key(void *elem);

/* lookup an indexed object from index. takes a reference on object */
void *rvt_pool_get_index(struct rvt_pool *pool, u32 index);

/* lookup keyed object from key. takes a reference on the object */
void *rvt_pool_get_key(struct rvt_pool *pool, void *key);

/* cleanup an object when all references are dropped */
void rvt_elem_release(struct kref *kref);

/* take a reference on an object */
#define rvt_add_ref(elem) kref_get(&(elem)->pelem.ref_cnt)

/* drop a reference on an object */
#define rvt_drop_ref(elem) kref_put(&(elem)->pelem.ref_cnt, rvt_elem_release)

#endif /* RVT_POOL_H */
