/*
 * USB Type-C Connector System Software Interface driver
 *
 * Copyright (C) 2016, Intel Corporation
 * Author: Heikki Krogerus <heikki.krogerus@linux.intel.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/platform_device.h>
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/acpi.h>

#include "ucsi.h"

#define PPM_TIMEOUT 50
#define UCSI_ERROR 1
#define UCSI_BUSY 2

#define cci_to_connector(_ucsi_, cci) \
		(_ucsi_->connector + UCSI_CCI_CONNECTOR_CHANGE(cci) - 1)

struct ucsi_connector {
	int num;
	struct ucsi *ucsi;
	struct work_struct work;
	struct ucsi_connector_capability cap;
};

struct ucsi {
	struct device *dev;
	struct ucsi_data __iomem *data;

	int status;
	struct completion complete;
	struct ucsi_capability cap;
	struct ucsi_connector *connector;

	struct mutex ppm_lock;
	atomic_t event_pending;
};

static char data_role[7];
module_param_string(usb_data_role, data_role, sizeof(data_role), 0644);
MODULE_PARM_DESC(usb_data_role, " USB Data Role - host or device");

static int ucsi_acpi_cmd(struct ucsi *ucsi, u64 ctrl)
{
	static const u8 ucsi_uuid[] = {
		0xc2, 0x98, 0x83, 0x6f,	0xa4, 0x7c, 0xe4, 0x11,
		0xad, 0x36, 0x63, 0x10, 0x42, 0xb5, 0x00, 0x8f,
	};
	union acpi_object *obj;

	ucsi->data->control = ctrl;

	obj = acpi_evaluate_dsm(ACPI_HANDLE(ucsi->dev), ucsi_uuid, 1, 1, NULL);
	if (!obj) {
		dev_err(ucsi->dev, "%s: failed to evaluate _DSM\n", __func__);
		return -EIO;
	}

	ACPI_FREE(obj);
	return 0;
}

static void ucsi_acpi_notify(acpi_handle handle, u32 event, void *data)
{
	struct ucsi *ucsi = data;
	u32 cci = ucsi->data->cci;

	dev_dbg(ucsi->dev, "%s: cci 0x%x\n", __func__, cci);

	if (!cci) {
		if (atomic_read(&ucsi->event_pending))
			complete(&ucsi->complete);
		return;
	}

	ucsi->status = 0;

	if (UCSI_CCI_CONNECTOR_CHANGE(cci)) {
		struct ucsi_connector *con = cci_to_connector(ucsi, cci);

		if (!ucsi->connector)
			goto no_connector;

		if (!atomic_read(&ucsi->event_pending)) {
			atomic_inc(&ucsi->event_pending);
			schedule_work(&con->work);
			return;
		}
	}
no_connector:
	if (cci & UCSI_CCI_BUSY)
		ucsi->status = UCSI_BUSY;

	if (cci & UCSI_CCI_ERROR)
		ucsi->status = UCSI_ERROR;

	if (cci & UCSI_CCI_ACK_CMD || cci & UCSI_CCI_CMD_COMPLETED) {
		ucsi->data->control = 0;
		complete(&ucsi->complete);
	}
}

static int ucsi_ack(struct ucsi *ucsi, u8 cmd)
{
	struct ucsi_control *ctrl;
	u64 control;
	int ret;

	ctrl = (void *)&control;

	ctrl->cmd = UCSI_ACK_CC_CI;
	ctrl->data = cmd;

	ret = ucsi_acpi_cmd(ucsi, control);
	if (ret)
		return ret;

	/* Waiting for ACK also with ACK CMD for now */
	wait_for_completion(&ucsi->complete);
	return 0;
}

static int ucsi_run_cmd(struct ucsi *ucsi, u64 *ctrl, void *data, size_t size)
{
	int status;
	int ret;

	dev_dbg(ucsi->dev, "%s control 0x%llx\n", __func__, *ctrl);

	ret = ucsi_acpi_cmd(ucsi, *ctrl);
	if (ret)
		return ret;

	/* REVISIT: We may need to set UCSI_CCI_CMD_COMPLETE flag here */
	if (!wait_for_completion_timeout(&ucsi->complete,
					 msecs_to_jiffies(PPM_TIMEOUT)))
		return -ETIMEDOUT;

	status = ucsi->status;
	if (status != UCSI_ERROR && size)
		memcpy(data, ucsi->data->message_in, size);

	ret = ucsi_ack(ucsi, UCSI_ACK_CMD);
	if (ret)
		goto err;

	if (status == UCSI_ERROR) {
		u16 error = 0;

		ret = ucsi_acpi_cmd(ucsi, UCSI_GET_ERROR_STATUS);
		if (ret)
			goto err;

		wait_for_completion(&ucsi->complete);

		memcpy(&error, ucsi->data->message_in, sizeof(error));

		/* Something has really gone wrong */
		if (ucsi->status == UCSI_ERROR) {
			ret = -ENODEV;
			goto err;
		}

		ret = ucsi_ack(ucsi, UCSI_ACK_CMD);
		if (ret)
			goto err;

		switch (error) {
		case UCSI_ERROR_INCOMPATIBLE_PARTNER:
			ret = -EOPNOTSUPP;
			break;
		case UCSI_ERROR_CC_COMMUNICATION_ERR:
			ret = -ECOMM;
			break;
		case UCSI_ERROR_CONTRACT_NEGOTIATION_FAIL:
			ret = -EIO;
			break;
		case UCSI_ERROR_DEAD_BATTERY:
			dev_warn(ucsi->dev, "Dead Battery Condition!\n");
			ret = -EPERM;
			break;
		/* The following mean a bug in this driver */
		case UCSI_ERROR_INVALID_CON_NUM:
		case UCSI_ERROR_UNREGONIZED_CMD:
		case UCSI_ERROR_INVALID_CMD_ARGUMENT:
		default:
			dev_warn(ucsi->dev,
				 "possible UCSI driver bug - error %hu\n",
				 error);
			ret = -EINVAL;
			break;
		}
	}
err:
	*ctrl = 0;
	return ret;
}

static void ucsi_connector_change(struct work_struct *work)
{
	struct ucsi_connector *con = container_of(work, struct ucsi_connector,
						  work);
	struct ucsi_connector_status constat;
	struct ucsi *ucsi = con->ucsi;
	struct ucsi_control *ctrl;
	u64 control = 0;
	int role;
	int ret;

	if (!data_role[0])
		return;

	mutex_lock(&ucsi->ppm_lock);

	ctrl = (void *)&control;
	ctrl->cmd = UCSI_GET_CONNECTOR_STATUS;
	ctrl->data = con->num;

	ret = ucsi_run_cmd(con->ucsi, &control, &constat, sizeof(constat));
	if (ret) {
		dev_err(ucsi->dev, "%s: failed to read connector status (%d)\n",
			__func__, ret);
		goto out;
	}

	/* Ignoring disconnections and Alternate Modes */
	if (!constat.connected || !(constat.change &
	    (UCSI_CONSTAT_PARTNER_CHANGE | UCSI_CONSTAT_CONNECT_CHANGE)) ||
	    constat.partner_flags & UCSI_CONSTAT_PARTNER_FLAG_ALT_MODE)
		goto out;

	if (!strcmp(data_role, "host")) {
		role = UCSI_UOR_ROLE_DFP;
	} else if (!strcmp(data_role, "device")) {
		role = UCSI_UOR_ROLE_UFP;
	} else {
		dev_warn(ucsi->dev, "no USB Data Role \"%s\"",
			 data_role);
		goto out;
	}

	/* If the partner got our preferred data role, attempting swap */
	if (role == (constat.partner_type & 0x3)) {
		struct ucsi_uor_cmd *uor = (void *)&control;

		uor->cmd = UCSI_SET_UOR;
		uor->con_num = con->num;
		uor->role = role;
		ret = ucsi_run_cmd(ucsi, &control, NULL, 0);
		if (ret)
			dev_err(ucsi->dev, "%s: failed to swap role (%d)\n",
				__func__, ret);
	}
out:
	ucsi_ack(ucsi, UCSI_ACK_EVENT);
	atomic_dec(&ucsi->event_pending);
	mutex_unlock(&ucsi->ppm_lock);
}

static int ucsi_init(struct ucsi *ucsi)
{
	struct ucsi_connector *con;
	struct ucsi_control ctrl;
	int ret;
	int i;

	atomic_set(&ucsi->event_pending, 0);
	init_completion(&ucsi->complete);
	mutex_init(&ucsi->ppm_lock);

	mutex_lock(&ucsi->ppm_lock);

	/* Reset */
	ret = ucsi_acpi_cmd(ucsi, UCSI_PPM_RESET);
	if (ret)
		return ret;

	msleep(20);

	/*
	 * REVISIT: Executing second reset to WA an issue seen on some of the
	 * Broxton base platforms, where the first reset puts the PPM into a
	 * state where it's unable to respond to all of the commands send to it
	 */
	ret = ucsi_acpi_cmd(ucsi, UCSI_PPM_RESET);
	if (ret)
		return ret;

	msleep(20);

	/* Enable basic notifications */
	ctrl.cmd = UCSI_SET_NOTIFICATION_ENABLE;
	ctrl.length = 0;
	ctrl.data = UCSI_ENABLE_NTFY_CMD_COMPLETE | UCSI_ENABLE_NTFY_ERROR;
	ret = ucsi_run_cmd(ucsi, (void *)&ctrl, NULL, 0);
	if (ret < 0)
		return ret;

	/* Get PPM capabilities */
	ctrl.cmd = UCSI_GET_CAPABILITY;
	ret = ucsi_run_cmd(ucsi, (void *)&ctrl, &ucsi->cap, sizeof(ucsi->cap));
	if (ret < 0)
		return ret;

	if (!ucsi->cap.num_connectors)
		return -ENODEV;

	ucsi->connector = kcalloc(ucsi->cap.num_connectors,
				  sizeof(struct ucsi_connector), GFP_KERNEL);
	if (!ucsi->connector)
		return -ENOMEM;

	for (i = 0, con = ucsi->connector; i < ucsi->cap.num_connectors;
	     i++, con++) {
		/* Get connector capability */
		ctrl.cmd = UCSI_GET_CONNECTOR_CAPABILITY;
		ctrl.data = i + 1;
		ret = ucsi_run_cmd(ucsi, (void *)&ctrl, &con->cap,
				   sizeof(con->cap));
		if (ret < 0)
			goto err;

		con->num = i + 1;
		con->ucsi = ucsi;
		INIT_WORK(&con->work, ucsi_connector_change);
	}

	/* Enable all notifications */
	ctrl.cmd = UCSI_SET_NOTIFICATION_ENABLE;
	ctrl.data = UCSI_ENABLE_NTFY_ALL;
	ret = ucsi_run_cmd(ucsi, (void *)&ctrl, NULL, 0);
	if (ret < 0)
		goto err;

	mutex_unlock(&ucsi->ppm_lock);
	return 0;
err:
	/* Disable all notifications */
	ucsi_acpi_cmd(ucsi, UCSI_SET_NOTIFICATION_ENABLE);

	mutex_unlock(&ucsi->ppm_lock);

	kfree(ucsi->connector);
	ucsi->connector = NULL;

	return ret;
}

static int ucsi_acpi_probe(struct platform_device *pdev)
{
	struct resource *res;
	acpi_status status;
	struct ucsi *ucsi;
	int ret;

	ucsi = devm_kzalloc(&pdev->dev, sizeof(*ucsi), GFP_KERNEL);
	if (!ucsi)
		return -ENOMEM;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		dev_err(&pdev->dev, "missing memory resource\n");
		return -ENODEV;
	}

	ucsi->data = devm_ioremap(&pdev->dev, res->start, resource_size(res));
	if (!ucsi->data)
		return -ENOMEM;

	ucsi->dev = &pdev->dev;

	status = acpi_install_notify_handler(ACPI_HANDLE(&pdev->dev),
					     ACPI_ALL_NOTIFY,
					     ucsi_acpi_notify, ucsi);
	if (ACPI_FAILURE(status))
		return -ENODEV;

	ret = ucsi_init(ucsi);
	if (ret) {
		acpi_remove_notify_handler(ACPI_HANDLE(&pdev->dev),
					   ACPI_ALL_NOTIFY,
					   ucsi_acpi_notify);
		return ret;
	}

	platform_set_drvdata(pdev, ucsi);
	return 0;
}

static int ucsi_acpi_remove(struct platform_device *pdev)
{
	struct ucsi *ucsi = platform_get_drvdata(pdev);

	acpi_remove_notify_handler(ACPI_HANDLE(&pdev->dev),
				   ACPI_ALL_NOTIFY, ucsi_acpi_notify);

	/* Disable all notifications */
	ucsi_acpi_cmd(ucsi, UCSI_SET_NOTIFICATION_ENABLE);

	kfree(ucsi->connector);
	return 0;
}

static const struct acpi_device_id ucsi_acpi_match[] = {
	{ "PNP0CA0", 0 },
	{ },
};
MODULE_DEVICE_TABLE(acpi, ucsi_acpi_match);

static struct platform_driver ucsi_acpi_platform_driver = {
	.driver = {
		.name = "ucsi_acpi",
		.acpi_match_table = ACPI_PTR(ucsi_acpi_match),
	},
	.probe = ucsi_acpi_probe,
	.remove = ucsi_acpi_remove,
};

module_platform_driver(ucsi_acpi_platform_driver);

MODULE_AUTHOR("Heikki Krogerus <heikki.krogerus@linux.intel.com>");
MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("USB Type-C System Software Interface (UCSI) driver");
