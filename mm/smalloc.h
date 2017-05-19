/*
 * smalloc.h: Header for Sealable Memory Allocator
 *
 * (C) Copyright 2017 Huawei Technologies Co. Ltd.
 * Author: Igor Stoppa <igor.stoppa@huawei.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; version 2
 * of the License.
 */

#ifndef _SMALLOC_H
#define _SMALLOC_H

#include <linux/list.h>
#include <linux/mutex.h>

typedef uint64_t align_t;

enum seal_t {
	SMALLOC_UNSEALED,
	SMALLOC_SEALED,
};

#define __SMALLOC_ALIGNED__ __aligned(sizeof(align_t))

#define NODE_HEADER					\
	struct {					\
		__SMALLOC_ALIGNED__ struct {		\
			struct list_head list;		\
			align_t *free;			\
			unsigned long available_words;	\
		};					\
	}

#define NODE_HEADER_SIZE sizeof(NODE_HEADER)

struct smalloc_pool {
	struct list_head list;
	struct mutex lock;
	enum seal_t seal;
};

struct smalloc_node {
	NODE_HEADER;
	__SMALLOC_ALIGNED__ align_t data[];
};

#define smalloc_seal(pool) \
	smalloc_seal_set(SMALLOC_SEALED, pool)

#define smalloc_unseal(pool) \
	smalloc_seal_set(SMALLOC_UNSEALED, pool)

struct smalloc_pool *smalloc_create(void);
int smalloc_destroy(struct smalloc_pool *pool);
int smalloc_initialize(struct smalloc_pool *pool);
void *smalloc(unsigned long size, struct smalloc_pool *pool);
void smalloc_seal_set(enum seal_t seal, struct smalloc_pool *pool);
#endif
