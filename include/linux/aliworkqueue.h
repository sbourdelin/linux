#ifndef ALI_WORKQUEUE_H
#define ALI_WORKQUEUE_H
/*
 * Adaptive Lock Integration
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * Copyright (C) 2015 Alibaba Group.
 *
 * Authors: Ma Ling <ling.ml@alibaba-inc.com>
 *
 */
typedef struct ali_workqueue {
	void  *wq;
} ali_workqueue_t;

struct ali_workqueue_info {
	struct ali_workqueue_info *next;
	int pending;
	void (*fn)(void *);
	void *para;
};

void ali_workqueue(struct ali_workqueue *ali_wq, struct ali_workqueue_info *ali);
void ali_workqueue_init(struct ali_workqueue *ali_wq);
#endif /* ALI_WORKQUEUE_H */
