/* Copyright (c) 2013-2017, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * UFS ioctl - this architecture can be used to configure driver and
 * device/host parameters, that are otherwise unavailable for such operation
 */

#include <scsi/ufs/ioctl.h>

#include "ufshcd-ioctl.h"

static int ufshcd_ioctl_query_desc(struct ufs_hba *hba,
	struct ufs_ioctl_query_data *ioctl_data,
	void *data_ptr, u8 lun, int length, bool write)
{
	int err = 0;
	u8 index;

	/* LUN indexed descriptors */
	if (ioctl_data->idn == QUERY_DESC_IDN_UNIT) {
		if (ufs_is_valid_unit_desc_lun(lun)) {
			index = lun;
		} else {
			err = -EINVAL;
			goto out;
		}
	/* LUN independent descriptors */
	} else if (ioctl_data->idn < QUERY_DESC_IDN_MAX) {
		index = 0;
	/* Invalid descriptors */
	} else {
		err = -EINVAL;
		goto out;
	}

	err = ufshcd_query_descriptor(hba, ioctl_data->opcode,
		ioctl_data->idn, index, 0, data_ptr, &length);

	if (!err && !write)
		ioctl_data->buf_size =
			min_t(int, ioctl_data->buf_size, length);
	else
		ioctl_data->buf_size = 0;

out:
	if (err)
		dev_err(hba->dev, "Query Descriptor failed (error: %d)", err);

	return err;
}

static int ufshcd_ioctl_query_attr(struct ufs_hba *hba,
	struct ufs_ioctl_query_data *ioctl_data,
	void *data_ptr, u8 lun, bool write)
{
	int err = 0;
	u8 index;

	/* LUN indexed attributes */
	if (ioctl_data->idn == QUERY_ATTR_IDN_DYN_CAP_NEEDED ||
	    ioctl_data->idn == QUERY_ATTR_IDN_PRG_BLK_NUM) {
		if (ufs_is_valid_unit_desc_lun(lun)) {
			index = lun;
		} else {
			err = -EINVAL;
			goto out;
		}
	/* LUN independent attributes */
	} else if (ioctl_data->idn < QUERY_ATTR_IDN_MAX) {
		index = 0;
	/* Invalid attribiutes */
	} else {
		err = -EINVAL;
		goto out;
	}

	err = ufshcd_query_attr(hba, ioctl_data->opcode,
		ioctl_data->idn, index, 0, data_ptr);

	if (!err && !write)
		ioctl_data->buf_size =
			min_t(int, ioctl_data->buf_size, sizeof(u32));
	else
		ioctl_data->buf_size = 0;

out:
	if (err)
		dev_err(hba->dev, "Query Attribute failed (error: %d)", err);

	return err;
}

static int ufshcd_ioctl_query_flag(struct ufs_hba *hba,
	struct ufs_ioctl_query_data *ioctl_data, void *data_ptr,
	bool write)
{
	int err = 0;

	/*
	 * Some flags are added to reserved space between flags in
	 * more or less recent UFS Specs. If it's reserved for current
	 * device, we will get an R/W error during operation and return it.
	 */
	if (ioctl_data->idn >= QUERY_FLAG_IDN_MAX) {
		err = -EINVAL;
		goto out;
	}

	err = ufshcd_query_flag(hba, ioctl_data->opcode,
				ioctl_data->idn, data_ptr);

	if (!err && !write)
		ioctl_data->buf_size =
			min_t(int, ioctl_data->buf_size, sizeof(bool));
	else
		ioctl_data->buf_size = 0;

out:
	if (err)
		dev_err(hba->dev, "Query Flag failed (error: %d)", err);

	return err;
}

/**
 * ufshcd_query_ioctl - perform user read queries
 * @hba: per-adapter instance
 * @lun: used for lun specific queries
 * @buffer: user space buffer for reading and submitting query data and params
 *
 * Returns 0 for success or negative error code otherwise
 *
 * Expected/Submitted buffer structure is struct ufs_ioctl_query_data.
 * It will read the opcode, idn and buf_length parameters, and put the
 * response in the buffer field while updating the used size in buf_length.
 */
static int ufshcd_query_ioctl(struct ufs_hba *hba, u8 lun, void __user *buffer)
{
	/*
	 * ioctl_data->buffer is untouchable - it's IO user data ptr.
	 *
	 * We might and will copy reply to it or request from it, but it's
	 * still pointer to user-space data, so treat it as __user argument.
	 */
	struct ufs_ioctl_query_data *ioctl_data;
	int err = 0;
	void *data_ptr = NULL;
	int length = 0;
	size_t data_size = sizeof(*ioctl_data);
	bool write = false;

	if (!buffer)
		return -EINVAL;

	/* Prepare space in kernel for users request */
	ioctl_data = kzalloc(data_size, GFP_KERNEL);

	if (!ioctl_data) {
		err = -ENOMEM;
		goto out;
	}

	/* extract params from user buffer */
	if (copy_from_user(ioctl_data, buffer, data_size)) {
		err = -EFAULT;
		goto out_release_mem;
	}

	if (!ioctl_data->buf_size) {
		/* Noting to do */
		err = 0;
		goto out_release_mem;
	}

	/* Prepare to handle query */
	switch (ioctl_data->opcode) {
	case UPIU_QUERY_OPCODE_WRITE_DESC:
		write = true;
	case UPIU_QUERY_OPCODE_READ_DESC:
		ufshcd_map_desc_id_to_length(hba, ioctl_data->idn, &length);
		break;
	case UPIU_QUERY_OPCODE_WRITE_ATTR:
		write = true;
	case UPIU_QUERY_OPCODE_READ_ATTR:
		length = sizeof(u32);
		break;
	case UPIU_QUERY_OPCODE_SET_FLAG:
	case UPIU_QUERY_OPCODE_CLEAR_FLAG:
	case UPIU_QUERY_OPCODE_TOGGLE_FLAG:
		write = true;
	case UPIU_QUERY_OPCODE_READ_FLAG:
		length = sizeof(bool);
		break;
	default:
		length = 0;
		break;
	}

	if (!length) {
		err = -EINVAL;
		goto out_release_mem;
	}

	/* We have to allocate memory at this level */
	data_ptr = kzalloc(length, GFP_KERNEL);

	if (!data_ptr) {
		err = -ENOMEM;
		goto out_release_mem;
	}

	if (write) {
		length = min_t(int, length, ioctl_data->buf_size);
		err = copy_from_user(data_ptr, ioctl_data->buffer, length);

		if (err)
			goto out_release_mem;
	}

	/* Verify legal parameters & send query */
	switch (ioctl_data->opcode) {
	case UPIU_QUERY_OPCODE_WRITE_DESC:
	case UPIU_QUERY_OPCODE_READ_DESC:
		err = ufshcd_ioctl_query_desc(hba, ioctl_data, data_ptr,
					      lun, length, write);
		break;
	case UPIU_QUERY_OPCODE_WRITE_ATTR:
	case UPIU_QUERY_OPCODE_READ_ATTR:
		err = ufshcd_ioctl_query_attr(hba, ioctl_data, data_ptr,
					      lun, write);
		break;
	case UPIU_QUERY_OPCODE_SET_FLAG:
	case UPIU_QUERY_OPCODE_CLEAR_FLAG:
	case UPIU_QUERY_OPCODE_TOGGLE_FLAG:
	case UPIU_QUERY_OPCODE_READ_FLAG:
		err = ufshcd_ioctl_query_flag(hba, ioctl_data,
					      data_ptr, write);
		break;
	default:
		err = -EINVAL;
		goto out_release_mem;
	}

	if (err)
		goto out_release_mem;

	/* Copy basic data to user */
	err = copy_to_user(buffer, ioctl_data, data_size);

	/*
	 * We copy result of query to ptr copied
	 * from user-space in the beginning
	 * if there is anything to be copied to user.
	 */
	if (!err && ioctl_data->buf_size)
		err = copy_to_user(ioctl_data->buffer, data_ptr,
			ioctl_data->buf_size);

out_release_mem:
	kfree(ioctl_data);
	kfree(data_ptr);
out:
	if (err)
		dev_err(hba->dev, "User Query failed (error: %d)", err);

	return err;
}

static int ufshcd_auto_hibern8_ioctl(struct ufs_hba *hba, void __user *buffer)
{
	struct ufs_ioctl_auto_hibern8_data *ioctl_data;
	int err = 0;
	u32 status = 0;

	if (!(hba->capabilities & MASK_AUTO_HIBERN8_SUPPORT))
		return -ENOTSUPP;

	if (!buffer)
		return -EINVAL;

	ioctl_data = kzalloc(sizeof(struct ufs_ioctl_auto_hibern8_data),
				GFP_KERNEL);
	if (!ioctl_data) {
		err = -ENOMEM;
		goto out;
	}

	/* extract params from user buffer */
	if (copy_from_user(ioctl_data, buffer, sizeof(*ioctl_data))) {
		err = -EFAULT;
		goto out_release_mem;
	}

	if (ioctl_data->write) {
		if (ioctl_data->timer_val > UFSHCD_AHIBERN8_TIMER_MASK ||
		    (ioctl_data->scale >= UFSHCD_AHIBERN8_SCALE_MAX)) {
			err = -EINVAL;
			goto out_release_mem;
		}

		/* Write valid state to host */
		ufshcd_setup_auto_hibern8(hba, ioctl_data->scale,
			ioctl_data->timer_val);
	} else {
		status = ufshcd_read_auto_hibern8_state(hba);
		ioctl_data->scale =
			(status & UFSHCD_AHIBERN8_SCALE_MASK) >> 10;
		ioctl_data->timer_val =
			(status & UFSHCD_AHIBERN8_TIMER_MASK);

		/* Copy state to user */
		err = copy_to_user(buffer, ioctl_data, sizeof(*ioctl_data));
	}

out_release_mem:
	kfree(ioctl_data);
out:
	if (err)
		dev_err(hba->dev, "Auto-Hibern8 request failed (error: %d)",
			err);

	return err;
}

static int ufshcd_task_mgmt_ioctl(struct ufs_hba *hba, u8 lun,
	void __user *buffer)
{
	struct ufs_ioctl_task_mgmt_data *ioctl_data;
	int err = 0;

	if (!buffer)
		return -EINVAL;

	if (!ufs_is_valid_unit_desc_lun(lun))
		return -EINVAL;

	ioctl_data = kzalloc(sizeof(struct ufs_ioctl_task_mgmt_data),
				GFP_KERNEL);
	if (!ioctl_data) {
		err = -ENOMEM;
		goto out;
	}

	/* Extract params from user buffer */
	if (copy_from_user(ioctl_data, buffer, sizeof(*ioctl_data))) {
		err = -EFAULT;
		goto out_release_mem;
	}

	err = ufshcd_issue_tm_cmd(hba, lun, ioctl_data->task_id,
		ioctl_data->task_func, &ioctl_data->response);

	if (err)
		goto out_release_mem;

	/* Copy response to user */
	if (copy_to_user(buffer, ioctl_data, sizeof(*ioctl_data)))
		err = -EFAULT;

out_release_mem:
	kfree(ioctl_data);
out:
	if (err)
		dev_err(hba->dev, "User Task Management failed (error: %d)",
			err);

	return err;
}

/**
 * ufshcd_ioctl - ufs ioctl callback registered in scsi_host
 * @dev: scsi device required for per LUN queries
 * @cmd: command opcode
 * @buffer: user space buffer for transferring data
 *
 * Supported commands:
 * UFS_IOCTL_QUERY
 * UFS_IOCTL_AUTO_HIBERN8
 * UFS_IOCTL_TASK_MANAGEMENT
 */
int ufshcd_ioctl(struct scsi_device *dev, int cmd, void __user *buffer)
{
	struct ufs_hba *hba = shost_priv(dev->host);
	int err = 0;

	if (WARN_ON(!hba))
		return -ENODEV;

	switch (cmd) {
	case UFS_IOCTL_QUERY:
		pm_runtime_get_sync(hba->dev);
		err = ufshcd_query_ioctl(hba, ufshcd_scsi_to_upiu_lun(dev->lun),
				buffer);
		pm_runtime_put_sync(hba->dev);
		break;
	case UFS_IOCTL_AUTO_HIBERN8:
		pm_runtime_get_sync(hba->dev);
		err = ufshcd_auto_hibern8_ioctl(hba, buffer);
		pm_runtime_put_sync(hba->dev);
		break;
	case UFS_IOCTL_TASK_MANAGEMENT:
		pm_runtime_get_sync(hba->dev);
		err = ufshcd_task_mgmt_ioctl(hba,
			ufshcd_scsi_to_upiu_lun(dev->lun), buffer);
		pm_runtime_put_sync(hba->dev);
		break;
	default:
		err = -EOPNOTSUPP;
		break;
	}

	if (err)
		dev_err(hba->dev, "UFS ioctl() failed (cmd=%04x error: %d)",
			cmd, err);

	return err;
}
