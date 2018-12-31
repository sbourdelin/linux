// SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB
/* Copyright (c) 2018, Mellanox Technologies inc. All rights reserved. */

#include <devlink.h>
#include "mlx5_core.h"

static int
mlx5_devlink_health_buffer_fill_syndrom(struct devlink_health_buffer *dh_buffer,
					u8 syndrom)
{
	int err;

	err = devlink_health_buffer_nest_start(dh_buffer,
					       DEVLINK_ATTR_HEALTH_BUFFER_OBJECT);
	if (err)
		return err;
	err = devlink_health_buffer_nest_start(dh_buffer,
					       DEVLINK_ATTR_HEALTH_BUFFER_OBJECT_PAIR);
	if (err)
		return err;
	err = devlink_health_buffer_put_object_name(dh_buffer, "Syndrom");
	if (err)
		return err;
	err = devlink_health_buffer_nest_start(dh_buffer,
					       DEVLINK_ATTR_HEALTH_BUFFER_OBJECT_VALUE);
	if (err)
		return err;
	err = devlink_health_buffer_put_value_u8(dh_buffer, syndrom);
	if (err)
		return err;
	devlink_health_buffer_nest_end(dh_buffer);
	devlink_health_buffer_nest_end(dh_buffer);
	devlink_health_buffer_nest_end(dh_buffer);

	return 0;
}

static int
mlx5_fw_reporter_diagnose(struct devlink_health_reporter *reporter,
			  struct devlink_health_buffer **buffers_array,
			  unsigned int buff_size, unsigned int num_buffers)
{
	struct mlx5_core_dev *dev = devlink_health_reporter_priv(reporter);
	char lines_buf[HEALTH_INFO_LINES][HEALTH_INFO_MAX_LINE] = {};
	struct devlink_health_buffer *buffer;
	u8 synd;
	int err;
	int i;

	if (!buffers_array || buff_size < HEALTH_INFO_MAX_BUFF ||
	    num_buffers < 1 || !buffers_array[0])
		return -EINVAL;

	buffer = buffers_array[0];
	mlx5_get_health_info(dev, &synd, lines_buf);
	mlx5_devlink_health_buffer_fill_syndrom(buffer, synd);

	if (!synd)
		return 0;

	err = devlink_health_buffer_nest_start(buffer,
					       DEVLINK_ATTR_HEALTH_BUFFER_OBJECT);
	if (err)
		return err;
	err = devlink_health_buffer_nest_start(buffer,
					       DEVLINK_ATTR_HEALTH_BUFFER_OBJECT_PAIR);
	if (err)
		return err;
	err = devlink_health_buffer_put_object_name(buffer, "diagnose data");
	if (err)
		return err;
	err = devlink_health_buffer_nest_start(buffer,
					       DEVLINK_ATTR_HEALTH_BUFFER_OBJECT_VALUE);
	if (err)
		return err;
	err = devlink_health_buffer_nest_start(buffer,
					       DEVLINK_ATTR_HEALTH_BUFFER_OBJECT_VALUE_ARRAY);
	if (err)
		return err;

	for (i = 0; i < HEALTH_INFO_LINES; i++) {
		err = devlink_health_buffer_nest_start(buffer, DEVLINK_ATTR_HEALTH_BUFFER_OBJECT_VALUE);
		if (err)
			return err;
		err = devlink_health_buffer_put_value_string(buffer, lines_buf[i]);
		if (err)
			return err;
		devlink_health_buffer_nest_end(buffer);
	}
	devlink_health_buffer_nest_end(buffer); /* DEVLINK_HEALTH_BUFFER_OBJECT_VALUE_ARRAY */
	devlink_health_buffer_nest_end(buffer); /* DEVLINK_HEALTH_BUFFER_OBJECT_VALUE */
	devlink_health_buffer_nest_end(buffer); /* DEVLINK_HEALTH_BUFFER_OBJECT_PAIR */
	devlink_health_buffer_nest_end(buffer); /* DEVLINK_HEALTH_BUFFER_OBJECT */

	return 0;
}

static const struct devlink_health_reporter_ops mlx5_fw_reporter_ops = {
		.name = "FW",
		.diagnose_size = HEALTH_INFO_MAX_BUFF,
		.diagnose = mlx5_fw_reporter_diagnose,
};

int mlx5_fw_reporter_create(struct mlx5_core_dev *dev)
{
	struct devlink *devlink = priv_to_devlink(dev);

	dev->fw_reporter = devlink_health_reporter_create(devlink, &mlx5_fw_reporter_ops,
							  0, false, dev);
	return PTR_ERR_OR_ZERO(dev->fw_reporter);
}

void mlx5_fw_reporter_destroy(struct mlx5_core_dev *dev)
{
	if (!dev->fw_reporter)
		return;

	devlink_health_reporter_destroy(dev->fw_reporter);
}

int mlx5_devlink_register(struct devlink *devlink, struct device *dev)
{
	return devlink_register(devlink, dev);
}

void mlx5_devlink_unregister(struct devlink *devlink)
{
	devlink_unregister(devlink);
}
