/*
 * Copyright (c) 2016, Mellanox Technologies. All rights reserved.
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

#ifndef __MLX5_EN_SWITCHDEV__H__
#define __MLX5_EN_SWITCHDEV__H__

#ifdef CONFIG_MLX5_EN_SWITCHDEV

extern const struct switchdev_ops mlx5e_switchdev_ops;

void mlx5e_destroy_offloads_flow_table(struct mlx5e_priv *priv);
int mlx5e_create_offloads_flow_table(struct mlx5e_priv *priv);
void mlx5e_switchdev_init(struct net_device *dev);

#else
static inline void mlx5e_destroy_offloads_flow_table(struct mlx5e_priv *priv)
{
}

static inline int mlx5e_create_offloads_flow_table(struct mlx5e_priv *priv)
{
	return 0;
}

static inline void mlx5e_switchdev_init(struct net_device *dev)
{
}
#endif

#endif /* __MLX5_EN_SWITCHDEV__H__ */

