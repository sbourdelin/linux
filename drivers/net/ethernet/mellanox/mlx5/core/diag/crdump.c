// SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB
/* Copyright (c) 2018 Mellanox Technologies */

#include <linux/proc_fs.h>
#include <linux/mlx5/driver.h>
#include <net/devlink.h>
#include "mlx5_core.h"
#include "lib/pci_vsc.h"

#define BAD_ACCESS			0xBADACCE5
#define MLX5_PROTECTED_CR_SCAN_CRSPACE	0x7
#define MAX_NUM_OF_DUMPS_TO_STORE	(8)

static const char *region_cr_space_str = "cr-space";

struct mlx5_fw_crdump {
	u32			size;
	struct devlink_region	*region_crspace;
};

bool mlx5_crdump_enbaled(struct mlx5_core_dev *dev)
{
	struct mlx5_priv *priv = &dev->priv;

	return (!!priv->health.crdump);
}

static int mlx5_crdump_fill(struct mlx5_core_dev *dev)
{
	struct devlink *devlink = priv_to_devlink(dev);
	struct mlx5_priv *priv = &dev->priv;
	struct mlx5_fw_crdump *crdump = priv->health.crdump;
	int i, ret = 0;
	u32 *cr_data;
	u32 id;

	cr_data = kvmalloc(crdump->size, GFP_KERNEL);
	if (!cr_data)
		return -ENOMEM;

	for (i = 0; i < (crdump->size / 4); i++)
		cr_data[i] = BAD_ACCESS;

	ret = mlx5_vsc_gw_read_block_fast(dev, cr_data, crdump->size);
	if (ret <= 0)
		goto free_data;

	if (crdump->size != ret) {
		mlx5_core_warn(dev, "failed to read full dump, read %d out of %u\n",
			       ret, crdump->size);
		ret = -EINVAL;
		goto free_data;
	}

	/* Get the available snapshot ID for the dumps */
	id = devlink_region_shapshot_id_get(devlink);
	ret = devlink_region_snapshot_create(crdump->region_crspace,
					     crdump->size, (u8 *)cr_data,
					     id, &kvfree);
	if (ret) {
		mlx5_core_warn(dev, "crdump: devlink create %s snapshot id %d err %d\n",
			       region_cr_space_str, id, ret);
		goto free_data;
	} else {
		mlx5_core_info(dev, "crdump: added snapshot %d to devlink region %s\n",
			       id, region_cr_space_str);
	}
	return 0;

free_data:
	kvfree(cr_data);
	return ret;
}

int mlx5_crdump_collect(struct mlx5_core_dev *dev)
{
	int ret = 0;

	if (!mlx5_crdump_enbaled(dev))
		return -ENODEV;

	ret = mlx5_vsc_gw_lock(dev);
	if (ret) {
		mlx5_core_warn(dev, "crdump: failed to lock vsc gw err %d\n",
			       ret);
		return ret;
	}

	ret = mlx5_vsc_gw_set_space(dev, MLX5_VSC_SPACE_SCAN_CRSPACE, NULL);
	if (ret)
		goto unlock;

	ret = mlx5_crdump_fill(dev);

unlock:
	mlx5_vsc_gw_unlock(dev);
	return ret;
}

int mlx5_crdump_init(struct mlx5_core_dev *dev)
{
	struct devlink *devlink = priv_to_devlink(dev);
	struct mlx5_priv *priv = &dev->priv;
	struct mlx5_fw_crdump *crdump;
	u32 space_size;
	int ret;

	if (!mlx5_core_is_pf(dev) || !mlx5_vsc_accessible(dev) ||
	    mlx5_crdump_enbaled(dev))
		return 0;

	ret = mlx5_vsc_gw_lock(dev);
	if (ret)
		return ret;

	/* Check if space is supported and get space size */
	ret = mlx5_vsc_gw_set_space(dev, MLX5_VSC_SPACE_SCAN_CRSPACE,
				    &space_size);
	if (ret) {
		/* Unlock and mask error since space is not supported */
		mlx5_vsc_gw_unlock(dev);
		return 0;
	}

	if (!space_size) {
		mlx5_core_warn(dev, "Invalid Crspace size, zero\n");
		mlx5_vsc_gw_unlock(dev);
		return -EINVAL;
	}

	ret = mlx5_vsc_gw_unlock(dev);
	if (ret)
		return ret;

	crdump = kzalloc(sizeof(*crdump), GFP_KERNEL);
	if (!crdump)
		return -ENOMEM;

	/* Create cr-space region */
	crdump->size = space_size;
	crdump->region_crspace =
		devlink_region_create(devlink,
				      region_cr_space_str,
				      MAX_NUM_OF_DUMPS_TO_STORE,
				      space_size);
	if (IS_ERR(crdump->region_crspace)) {
		mlx5_core_warn(dev,
			       "crdump: create devlink region %s err %ld\n",
			       region_cr_space_str,
			       PTR_ERR(crdump->region_crspace));
		ret = PTR_ERR(crdump->region_crspace);
		goto free_crdump;
	}
	priv->health.crdump = crdump;
	return 0;

free_crdump:
	kfree(crdump);
	return ret;
}

void mlx5_crdump_cleanup(struct mlx5_core_dev *dev)
{
	struct mlx5_priv *priv = &dev->priv;
	struct mlx5_fw_crdump *crdump = priv->health.crdump;

	if (!crdump)
		return;

	devlink_region_destroy(crdump->region_crspace);
	kfree(crdump);
	priv->health.crdump = NULL;
}
