/*
 * Copyright (c) 2017, Linaro Ltd.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#ifndef _LINUX_INTERCONNECT_CONSUMER_H
#define _LINUX_INTERCONNECT_CONSUMER_H

struct interconnect_node;
struct interconnect_path;

#if IS_ENABLED(CONFIG_INTERCONNECT)

struct interconnect_path *interconnect_get(const char *sdev, const int sid,
					   const char *ddev, const int did);

void interconnect_put(struct interconnect_path *path);

/**
 * struct creq - interconnect consumer request
 * @avg_bw: the average requested bandwidth in kbps
 * @max_bw: the maximum (peak) bandwidth in kpbs
 */
struct interconnect_creq {
	u32 avg_bw;
	u32 max_bw;
};

/**
 * interconnect_set() - set constraints on a path between two endpoints
 * @path: reference to the path returned by interconnect_get()
 * @creq: request from the consumer, containing its requirements
 *
 * This function is used by an interconnect consumer to express its own needs
 * in term of bandwidth and QoS for a previously requested path between two
 * endpoints. The requests are aggregated and each node is updated accordingly.
 *
 * Returns 0 on success, or an approproate error code otherwise.
 */
int interconnect_set(struct interconnect_path *path,
		     struct interconnect_creq *creq);

#else

inline struct interconnect_path *interconnect_get(const char *sdev,
							 const int sid,
							 const char *ddev,
							 const int did)
{
	return ERR_PTR(-ENOTSUPP);
}

inline void interconnect_put(struct interconnect_path *path)
{
}

inline int interconnect_set(struct interconnect_path *path, u32 bandwidth)
{
	return -ENOTSUPP
}

#endif /* CONFIG_INTERCONNECT */

#endif /* _LINUX_INTERCONNECT_CONSUMER_H */
