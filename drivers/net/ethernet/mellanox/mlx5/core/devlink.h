/* SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB */
/* Copyright (c) 2018, Mellanox Technologies inc. All rights reserved. */

#ifndef __MLX5_DEVLINK_H__
#define __MLX5_DEVLINK_H__

#include <net/devlink.h>
#include "mlx5_core.h"

struct mlx5_fw_reporter_ctx {
	u8 err_synd;
	int miss_counter;
};

int mlx5_devlink_register(struct devlink *devlink, struct device *dev);
void mlx5_devlink_unregister(struct devlink *devlink);
int mlx5_fw_reporters_create(struct mlx5_core_dev *dev);
void mlx5_fw_reporters_destroy(struct mlx5_core_dev *dev);
void mlx5_fw_reporter_err_work(struct work_struct *work);
void mlx5_fw_fatal_reporter_work(struct work_struct *work);

#endif /* __MLX5_DEVLINK_H__ */
