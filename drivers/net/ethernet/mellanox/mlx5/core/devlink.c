// SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB
/* Copyright (c) 2018, Mellanox Technologies inc. All rights reserved. */

#include <devlink.h>
#include "mlx5_core.h"
#include "diag/fw_tracer.h"

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

int mlx5_devlink_health_buffer_fill_trace(struct devlink_health_buffer *dh_buffer,
					  char *trace)
{
	int nest = 0;
	int err = 0;
	int i;

	err = devlink_health_buffer_nest_start(dh_buffer,
					       DEVLINK_ATTR_HEALTH_BUFFER_OBJECT);
	if (err)
		goto nest_cancel;
	nest++;

	err = devlink_health_buffer_nest_start(dh_buffer,
					       DEVLINK_ATTR_HEALTH_BUFFER_OBJECT_PAIR);
	if (err)
		goto nest_cancel;
	nest++;

	err = devlink_health_buffer_put_object_name(dh_buffer, "trace");
	if (err)
		goto nest_cancel;

	err = devlink_health_buffer_nest_start(dh_buffer,
					       DEVLINK_ATTR_HEALTH_BUFFER_OBJECT_VALUE);
	if (err)
		goto nest_cancel;
	nest++;

	err = devlink_health_buffer_put_value_string(dh_buffer, trace);
	if (err)
		goto nest_cancel;

	for (i = 0; i < nest; i++)
		devlink_health_buffer_nest_end(dh_buffer);
	return 0;
nest_cancel:
	for (i = 0; i < nest; i++)
		devlink_health_buffer_nest_cancel(dh_buffer);

	return err;
}

int mlx5_fw_tracer_get_saved_traces_objects(struct mlx5_fw_tracer *tracer,
					    struct devlink_health_buffer **buffers_array,
					    unsigned int num_buffers)
{
	u32 saved_traces_index = tracer->sbuff.saved_traces_index;
	char *saved_traces = tracer->sbuff.traces_buff;
	u32 index, start_index, end_index;
	u32 dh_buffer_index = 0;
	int err = 0;

	if (!saved_traces[0])
		return -ENOMSG;

	if (saved_traces[saved_traces_index * TRACE_STR_LINE])
		start_index = saved_traces_index;
	else
		start_index = 0;
	end_index = (saved_traces_index - 1) & (SAVED_TRACES_NUM - 1);

	index = start_index;
	while (index <= end_index) {
		err = mlx5_devlink_health_buffer_fill_trace(buffers_array[dh_buffer_index],
							    saved_traces + index * TRACE_STR_LINE);
		if (err) {
			dh_buffer_index++;
			if (dh_buffer_index == num_buffers)
				break;
		} else {
			index++;
		}
	}

	return err;
}

static int
mlx5_fw_reporter_objdump(struct devlink_health_reporter *reporter,
			 struct devlink_health_buffer **buffers_array,
			 unsigned int buff_size, unsigned int num_buffers,
			 void *priv_ctx)
{
	struct mlx5_core_dev *dev = devlink_health_reporter_priv(reporter);
	struct devlink_health_buffer *buffer;
	int err;

	if (!buffers_array || buff_size < TRACE_STR_LINE || num_buffers < 1)
		return -EINVAL;

	err = mlx5_fw_tracer_trigger_core_dump_general(dev);
	if (err)
		return err;

	buffer = buffers_array[0];
	if (priv_ctx) {
		struct mlx5_fw_reporter_ctx *fw_reporter_ctx = priv_ctx;

		err = mlx5_devlink_health_buffer_fill_syndrom(buffer,
							      fw_reporter_ctx->err_synd);
		if (err)
			return err;
	}

	return mlx5_fw_tracer_get_saved_traces_objects(dev->tracer, buffers_array,
						       num_buffers);
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

void mlx5_fw_reporter_err_work(struct work_struct *work)
{
	struct mlx5_fw_reporter_ctx fw_reporter_ctx;
	struct mlx5_core_health *health;
	struct mlx5_core_dev *dev;
	struct mlx5_priv *priv;

	health = container_of(work, struct mlx5_core_health, report_work);
	priv = container_of(health, struct mlx5_priv, health);
	dev = container_of(priv, struct mlx5_core_dev, priv);

	fw_reporter_ctx.err_synd = health->synd;
	fw_reporter_ctx.miss_counter = health->miss_counter;
	if (fw_reporter_ctx.err_synd)
		devlink_health_report(dev->fw_reporter, "FW syndrom reported",
				      &fw_reporter_ctx);
	else if (fw_reporter_ctx.miss_counter)
		devlink_health_report(dev->fw_reporter, "FW miss counter reported",
				      &fw_reporter_ctx);
}

static const struct devlink_health_reporter_ops mlx5_fw_reporter_ops = {
		.name = "FW",
		.objdump_size = SAVED_TRACES_BUFFER_SIZE_BYTE,
		.diagnose_size = HEALTH_INFO_MAX_BUFF,
		.objdump = mlx5_fw_reporter_objdump,
		.diagnose = mlx5_fw_reporter_diagnose,
};

void mlx5_fw_fatal_reporter_work(struct work_struct *work)
{
	struct mlx5_core_health *health;
	struct delayed_work *dwork;
	struct mlx5_core_dev *dev;
	struct mlx5_priv *priv;

	dwork = container_of(work, struct delayed_work, work);
	health = container_of(dwork, struct mlx5_core_health, recover_work);
	priv = container_of(health, struct mlx5_priv, health);
	dev = container_of(priv, struct mlx5_core_dev, priv);

	devlink_health_report(dev->fw_fatal_reporter, "FW recovery", NULL);
}

static int
mlx5_fw_fatal_reporter_recover(struct devlink_health_reporter *reporter,
			       void *priv_ctx)
{
	struct mlx5_core_dev *dev = devlink_health_reporter_priv(reporter);
	u8 nic_state;

	nic_state = mlx5_get_nic_state(dev);
	if (nic_state == MLX5_NIC_IFC_INVALID) {
		dev_err(&dev->pdev->dev, "health recovery flow aborted since the nic state is invalid\n");
		return -ECANCELED;
	}
	dev_err(&dev->pdev->dev, "starting health recovery flow\n");

	mlx5_recover_device(dev);

	return 0;
}

static const struct devlink_health_reporter_ops mlx5_fw_fatal_reporter_ops = {
		.name = "FW_fatal",
		.recover = mlx5_fw_fatal_reporter_recover,
};

#define MLX5_REPORTER_FW_GRACEFUL_PERIOD 120000
int mlx5_fw_reporters_create(struct mlx5_core_dev *dev)
{
	struct devlink *devlink = priv_to_devlink(dev);

	dev->fw_reporter =
		devlink_health_reporter_create(devlink, &mlx5_fw_reporter_ops,
					       0, false, dev);
	if (IS_ERR(dev->fw_reporter))
		return PTR_ERR(dev->fw_reporter);

	dev->fw_fatal_reporter =
		devlink_health_reporter_create(devlink, &mlx5_fw_fatal_reporter_ops,
					       MLX5_REPORTER_FW_GRACEFUL_PERIOD,
					       true, dev);
	return PTR_ERR_OR_ZERO(dev->fw_fatal_reporter);
}

void mlx5_fw_reporters_destroy(struct mlx5_core_dev *dev)
{
	if (dev->fw_reporter)
		devlink_health_reporter_destroy(dev->fw_reporter);
	if (dev->fw_fatal_reporter)
		devlink_health_reporter_destroy(dev->fw_fatal_reporter);
}

int mlx5_devlink_register(struct devlink *devlink, struct device *dev)
{
	return devlink_register(devlink, dev);
}

void mlx5_devlink_unregister(struct devlink *devlink)
{
	devlink_unregister(devlink);
}
