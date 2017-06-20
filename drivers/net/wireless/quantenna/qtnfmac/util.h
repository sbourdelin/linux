/*
 * Copyright (c) 2015 Quantenna Communications
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION
 * OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#ifndef QTNFMAC_UTIL_H
#define QTNFMAC_UTIL_H

#include <linux/kernel.h>
#include "core.h"

static inline void qtnf_list_init(struct qtnf_list *list)
{
	if (unlikely(!list))
		return;

	INIT_LIST_HEAD(&list->head);
	atomic_set(&list->size, 0);
}

static inline size_t qtnf_list_size(const struct qtnf_list *list)
{
	return atomic_read(&list->size);
}

static inline int qtnf_list_empty(const struct qtnf_list *list)
{
	return list_empty(&list->head);
}

struct qtnf_sta_node *qtnf_sta_list_lookup(struct qtnf_list *list,
					   const u8 *mac);
struct qtnf_sta_node *qtnf_sta_list_lookup_index(struct qtnf_list *list,
						 size_t index);
struct qtnf_sta_node *qtnf_sta_list_add(struct qtnf_list *list, const u8 *mac);
int qtnf_sta_list_del(struct qtnf_list *list, const u8 *mac);
void qtnf_sta_list_free(struct qtnf_list *list);

struct qtnf_vif *qtnf_vlan_list_lookup(struct qtnf_list *list,
				       const u16 vlanid);
struct qtnf_vif *qtnf_vlan_list_add(struct qtnf_list *list, const u16 vlanid);
int qtnf_vlan_list_del(struct qtnf_list *list, const u16 vlanid);

#endif /* QTNFMAC_UTIL_H */
