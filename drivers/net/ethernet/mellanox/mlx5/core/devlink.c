// SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB
/* Copyright (c) 2018 Mellanox Technologies */

#include <devlink.h>
#include <linux/mlx5/driver.h>
#include "lib/mlx5.h"

static int mlx5_devlink_get_crdump_snapshot(struct devlink *devlink, u32 id,
					    struct devlink_param_gset_ctx *ctx)
{
	struct mlx5_core_dev *dev = devlink_priv(devlink);

	ctx->val.vbool = mlx5_crdump_is_snapshot_enabled(dev);
	return 0;
}

static int mlx5_devlink_set_crdump_snapshot(struct devlink *devlink, u32 id,
					    struct devlink_param_gset_ctx *ctx)
{
	struct mlx5_core_dev *dev = devlink_priv(devlink);

	return mlx5_crdump_set_snapshot_enabled(dev, ctx->val.vbool);
}

static const struct devlink_param mlx5_devlink_params[] = {
	DEVLINK_PARAM_GENERIC(REGION_SNAPSHOT,
			      BIT(DEVLINK_PARAM_CMODE_RUNTIME) |
			      BIT(DEVLINK_PARAM_CMODE_DRIVERINIT),
			      mlx5_devlink_get_crdump_snapshot,
			      mlx5_devlink_set_crdump_snapshot, NULL),
};

int mlx5_devlink_register(struct devlink *devlink, struct device *dev)
{
	union devlink_param_value init_val;
	int err;

	err = devlink_register(devlink, dev);
	if (err) {
		dev_warn(dev,
			 "devlink register failed (err = %d)", err);
		return err;
	}

	err = devlink_params_register(devlink, mlx5_devlink_params,
				      ARRAY_SIZE(mlx5_devlink_params));
	if (err) {
		dev_err(dev, "devlink_params_register failed, err = %d\n", err);
		goto unregister;
	}

	init_val.vbool = false;
	err = devlink_param_driverinit_value_set(devlink,
						 DEVLINK_PARAM_GENERIC_ID_REGION_SNAPSHOT,
						 init_val);
	if (err)
		dev_warn(dev,
			 "devlink param init failed (err = %d)", err);

	return 0;

unregister:
	devlink_unregister(devlink);
	return err;
}

void mlx5_devlink_unregister(struct devlink *devlink)
{
	devlink_params_unregister(devlink, mlx5_devlink_params,
				  ARRAY_SIZE(mlx5_devlink_params));
	devlink_unregister(devlink);
}
