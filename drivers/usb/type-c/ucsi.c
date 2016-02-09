/*
 * ucsi.c - USB Type-C Connector System Software Interface
 *
 * Copyright (C) 2016, Intel Corporation
 * Author: Heikki Krogerus <heikki.krogerus@linux.intel.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/completion.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/usb/typec.h>

#include "ucsi.h"

#define UCSI_ERROR 1
#define UCSI_BUSY 2

#define to_ucsi_connector(_port_) container_of(_port_->cap,                    \
					       struct ucsi_connector,          \
					       typec_cap)

#define cci_to_connector(_ucsi_, cci) (_ucsi_->connector +		       \
					UCSI_CCI_CONNECTOR_CHANGE(cci) - 1)

struct ucsi_connector {
	unsigned num;
	struct ucsi *ucsi;
	struct work_struct work;
	struct typec_port *port;
	struct typec_capability typec_cap;
	struct ucsi_connector_capability cap;
};

struct ucsi {
	struct device *dev;
	struct ucsi_ppm *ppm;

	int status;
	struct completion complete;
	struct ucsi_capability cap;
	struct ucsi_connector *connector;
};

static int ucsi_ack(struct ucsi *ucsi, u8 cmd)
{
	struct ucsi_control *ctrl = (void *)&ucsi->ppm->data->control;
	int ret;

	ucsi->ppm->data->control = 0;
	ctrl->cmd = UCSI_ACK_CC_CI;
	ctrl->data = cmd;

	ret = ucsi->ppm->cmd(ucsi->ppm);
	if (ret)
		return ret;

	/* Waiting for ACK also with ACK CMD for now */
	wait_for_completion(&ucsi->complete);
	return 0;
}

static int ucsi_run_cmd(struct ucsi *ucsi, void *data, size_t size)
{
	int status;
	int ret;

	dev_vdbg(ucsi->dev, "%s control 0x%llx\n", __func__,
		 ucsi->ppm->data->control);

	ret = ucsi->ppm->cmd(ucsi->ppm);
	if (ret)
		return ret;

	/* REVISIT: We may need to set UCSI_CCI_CMD_COMPLETE flag here */
	wait_for_completion(&ucsi->complete);

	status = ucsi->status;
	if (status != UCSI_ERROR && size)
		memcpy(data, ucsi->ppm->data->message_in, size);

	ret = ucsi_ack(ucsi, UCSI_ACK_CMD);
	if (ret)
		goto out;

	if (status == UCSI_ERROR) {
		u16 error;

		ucsi->ppm->data->control = UCSI_GET_ERROR_STATUS;
		ret = ucsi->ppm->cmd(ucsi->ppm);
		if (ret)
			goto out;

		wait_for_completion(&ucsi->complete);

		/* Something has really gone wrong */
		if (ucsi->status == UCSI_ERROR) {
			ret = -ENODEV;
			goto out;
		}

		memcpy(&error, ucsi->ppm->data->message_in, sizeof(error));

		ret = ucsi_ack(ucsi, UCSI_ACK_CMD);
		if (ret)
			goto out;

		switch (error) {
		case UCSI_ERROR_INVALID_CON_NUM:
			ret = -ENXIO;
			break;
		case UCSI_ERROR_INCOMPATIBLE_PARTNER:
		case UCSI_ERROR_CC_COMMUNICATION_ERR:
		case UCSI_ERROR_CONTRACT_NEGOTIATION_FAIL:
			ret = -EIO;
			break;
		case UCSI_ERROR_DEAD_BATTERY:
			dev_warn(ucsi->dev, "Dead Battery Condition!\n");
			ret = -EPERM;
			break;
		case UCSI_ERROR_UNREGONIZED_CMD:
		case UCSI_ERROR_INVALID_CMD_ARGUMENT:
		default:
			ret = -EINVAL;
			break;
		}
	}
out:
	ucsi->ppm->data->control = 0;
	return ret;
}

static int ucsi_dr_swap(struct typec_port *port)
{
	struct ucsi_connector *con = to_ucsi_connector(port);
	struct ucsi_uor_cmd *ctrl = (void *)&con->ucsi->ppm->data->control;

	ctrl->cmd = UCSI_SET_UOR;
	ctrl->con_num = con->num;
	ctrl->role = port->data_role == TYPEC_HOST ?
			UCSI_UOR_ROLE_UFP : UCSI_UOR_ROLE_DFP;
	if (port->cap->type == TYPEC_PORT_DRP)
		ctrl->role |= UCSI_UOR_ROLE_DRP;

	return ucsi_run_cmd(con->ucsi, NULL, 0);
}

static int ucsi_pr_swap(struct typec_port *port)
{
	struct ucsi_connector *con = to_ucsi_connector(port);
	struct ucsi_uor_cmd *ctrl = (void *)&con->ucsi->ppm->data->control;

	/* The command structure is identical to SET_UOR command structure */
	ctrl->cmd = UCSI_SET_PDR;
	ctrl->con_num = con->num;
	ctrl->role = port->pwr_role == TYPEC_PWR_SOURCE ?
			UCSI_UOR_ROLE_UFP : UCSI_UOR_ROLE_DFP;
	/* Always accepting power swap requests from partner for now */
	ctrl->role |= UCSI_UOR_ROLE_DRP;

	return ucsi_run_cmd(con->ucsi, NULL, 0);
}

static int ucsi_get_constat(struct ucsi_connector *con,
			    struct ucsi_connector_status *constat)
{
	struct ucsi_control *ctrl = (void *)&con->ucsi->ppm->data->control;

	ctrl->cmd = UCSI_GET_CONNECTOR_STATUS;
	ctrl->data = con->num;

	return ucsi_run_cmd(con->ucsi, constat, sizeof(*constat));
}

static int
ucsi_connect(struct ucsi_connector *con, struct ucsi_connector_status *constat)
{
	struct typec_port *port = con->port;

	port->connected = true;

	if (constat->partner_flags & UCSI_CONSTAT_PARTNER_FLAG_ALT_MODE)
		port->partner_type = TYPEC_PARTNER_ALTMODE;
	else
		port->partner_type = TYPEC_PARTNER_USB;

	switch (constat->partner_type) {
	case UCSI_CONSTAT_PARTNER_TYPE_CABLE_NO_UFP:
		/* REVISIT: We don't care about just the cable for now */
		return 0;
	case UCSI_CONSTAT_PARTNER_TYPE_DFP:
	case UCSI_CONSTAT_PARTNER_TYPE_CABLE_AND_UFP:
		port->pwr_role = TYPEC_PWR_SINK;
		port->data_role = TYPEC_DEVICE;
		break;
	case UCSI_CONSTAT_PARTNER_TYPE_UFP:
		port->pwr_role = TYPEC_PWR_SOURCE;
		port->data_role = TYPEC_HOST;
		break;
	case UCSI_CONSTAT_PARTNER_TYPE_DEBUG:
		port->partner_type = TYPEC_PARTNER_DEBUG;
		goto out;
	case UCSI_CONSTAT_PARTNER_TYPE_AUDIO:
		port->partner_type = TYPEC_PARTNER_AUDIO;
		goto out;
	}

	switch (constat->pwr_op_mode) {
	case UCSI_CONSTAT_PWR_OPMODE_NONE:
	case UCSI_CONSTAT_PWR_OPMODE_DEFAULT:
		port->pwr_opmode = TYPEC_PWR_MODE_USB;
		break;
	case UCSI_CONSTAT_PWR_OPMODE_BC:
		port->partner_type = TYPEC_PARTNER_CHARGER;
		port->pwr_opmode = TYPEC_PWR_MODE_BC1_2;
		break;
	case UCSI_CONSTAT_PWR_OPMODE_PD:
		port->pwr_opmode = TYPEC_PWR_MODE_PD;
		break;
	case UCSI_CONSTAT_PWR_OPMODE_TYPEC1_3:
		port->pwr_opmode = TYPEC_PWR_MODE_1_5A;
		break;
	case UCSI_CONSTAT_PWR_OPMODE_TYPEC3_0:
		port->pwr_opmode = TYPEC_PWR_MODE_3_0A;
		break;
	default:
		break;
	}
out:
	return typec_connect(port);
}

static void ucsi_disconnect(struct ucsi_connector *con)
{
	con->port->partner_type = TYPEC_PARTNER_NONE;
	con->port->connected = false;
	typec_disconnect(con->port);
}

static void ucsi_connector_change(struct work_struct *work)
{
	struct ucsi_connector *con = container_of(work, struct ucsi_connector,
						  work);
	struct ucsi_connector_status constat;

	ucsi_ack(con->ucsi, UCSI_ACK_EVENT);

	if (WARN_ON(ucsi_get_constat(con, &constat) != 0))
		return;

	if (constat.constat_change & UCSI_CONSTAT_CONNECT_CHANGE) {
		if (constat.connected)
			ucsi_connect(con, &constat);
		else
			ucsi_disconnect(con);
	}
}

/**
 * ucsi_interrupt - UCSI Notification Handler
 * @ucsi: Source UCSI Interface for the notifications
 *
 * Handle notifications from @ucsi.
 */
int ucsi_interrupt(struct ucsi *ucsi)
{
	u32 cci = ucsi->ppm->data->cci;

	if (!cci)
		return 0;

	if (UCSI_CCI_CONNECTOR_CHANGE(cci)) {
		struct ucsi_connector *con = cci_to_connector(ucsi, cci);

		schedule_work(&con->work);
		return 1;
	}

	ucsi->status = 0;

	/* REVISIT: We don't actually do anything with this for now */
	if (cci & UCSI_CCI_BUSY)
		ucsi->status = UCSI_BUSY;

	if (cci & UCSI_CCI_ERROR)
		ucsi->status = UCSI_ERROR;

	if (cci & UCSI_CCI_ACK_CMD || cci & UCSI_CCI_CMD_COMPLETED)
		complete(&ucsi->complete);

	return 1;
}
EXPORT_SYMBOL_GPL(ucsi_interrupt);

/**
 * ucsi_init - Initialize an UCSI Interface
 * @ucsi: The UCSI Interface
 *
 * Registers all the USB Type-C ports governed by the PPM of @ucsi and enables
 * all the notifications from the PPM.
 */
int ucsi_init(struct ucsi *ucsi)
{
	struct ucsi_control *ctrl = (void *)&ucsi->ppm->data->control;
	struct ucsi_connector *con;
	int ret;
	int i;

	/* Enable basic notifications */
	ctrl->cmd = UCSI_SET_NOTIFICATION_ENABLE;
	ctrl->data = UCSI_ENABLE_NTFY_CMD_COMPLETE | UCSI_ENABLE_NTFY_ERROR;
	ret = ucsi_run_cmd(ucsi, NULL, 0);
	if (ret)
		return ret;

	/* Get PPM capabilities */
	ctrl->cmd = UCSI_GET_CAPABILITY;
	ret = ucsi_run_cmd(ucsi, &ucsi->cap, sizeof(ucsi->cap));
	if (ret)
		return ret;

	ucsi->connector = kcalloc(ucsi->cap.num_connectors,
				  sizeof(struct ucsi_connector), GFP_KERNEL);
	if (!ucsi->connector)
		return -ENOMEM;

	for (i = 0, con = ucsi->connector; i < ucsi->cap.num_connectors;
	     i++, con++) {
		struct typec_capability *cap = &con->typec_cap;
		struct ucsi_connector_status constat;

		/* Get connector capability */
		ctrl->cmd = UCSI_GET_CONNECTOR_CAPABILITY;
		ctrl->data = i + 1;
		ret = ucsi_run_cmd(ucsi, &con->cap, sizeof(con->cap));
		if (ret)
			goto err;

		/* Register the connector */

		if (con->cap.op_mode & UCSI_CONCAP_OPMODE_DRP)
			cap->type = TYPEC_PORT_DRP;
		else if (con->cap.op_mode & UCSI_CONCAP_OPMODE_DFP)
			cap->type = TYPEC_PORT_DFP;
		else if (con->cap.op_mode & UCSI_CONCAP_OPMODE_UFP)
			cap->type = TYPEC_PORT_UFP;

		cap->usb_pd = !!(ucsi->cap.attributes &
				       UCSI_CAP_ATTR_USB_PD);
		cap->audio_accessory = !!(con->cap.op_mode &
					  UCSI_CONCAP_OPMODE_AUDIO_ACCESSORY);
		cap->debug_accessory = !!(con->cap.op_mode &
					  UCSI_CONCAP_OPMODE_DEBUG_ACCESSORY);

		/* TODO: Alt modes */

		cap->dr_swap = ucsi_dr_swap;
		cap->pr_swap = ucsi_pr_swap;

		con->port = typec_register_port(ucsi->dev, cap);
		if (IS_ERR(con->port)) {
			ret = PTR_ERR(con->port);
			goto err;
		}

		con->num = i + 1;
		con->ucsi = ucsi;
		INIT_WORK(&con->work, ucsi_connector_change);

		/* Check if the connector is connected */
		if (WARN_ON(ucsi_get_constat(con, &constat) != 0))
			continue;

		if (constat.connected)
			ucsi_connect(con, &constat);
	}

	/* Enable all notifications */
	ctrl->cmd = UCSI_SET_NOTIFICATION_ENABLE;
	ctrl->data = UCSI_ENABLE_NTFY_ALL;
	ret = ucsi_run_cmd(ucsi, NULL, 0);
	if (ret)
		goto err;

	return 0;
err:
	if (i > 0)
		for (; i >= 0; i--, con--)
			typec_unregister_port(con->port);

	kfree(ucsi->connector);
	return ret;
}
EXPORT_SYMBOL(ucsi_init);

/**
 * ucsi_register_ppm - Register UCSI PPM Interface
 * @dev: Device interface to the PPM
 * @ppm: The PPM interface
 *
 * Allocates an UCSI instance, associates it with @ppm and returns it to the
 * caller.
 */
struct ucsi *ucsi_register_ppm(struct device *dev, struct ucsi_ppm *ppm)
{
	struct ucsi *ucsi;

	ucsi = kzalloc(sizeof(*ucsi), GFP_KERNEL);
	if (!ucsi)
		return ERR_PTR(-ENOMEM);

	init_completion(&ucsi->complete);
	ucsi->dev = dev;
	ucsi->ppm = ppm;

	return ucsi;
}
EXPORT_SYMBOL_GPL(ucsi_register_ppm);

/**
 * ucsi_unregister_ppm - Unregister UCSI PPM Interface
 * @ucsi: struct ucsi associated with the PPM
 *
 * Unregister an UCSI PPM that was created with ucsi_register().
 */
void ucsi_unregister_ppm(struct ucsi *ucsi)
{
	struct ucsi_connector *con;
	int i;

	/* Disable all notifications */
	ucsi->ppm->data->control = UCSI_SET_NOTIFICATION_ENABLE;
	ucsi->ppm->cmd(ucsi->ppm);

	for (i = 0, con = ucsi->connector; i < ucsi->cap.num_connectors;
	     i++, con++)
		typec_unregister_port(con->port);

	kfree(ucsi->connector);
	kfree(ucsi);
}
EXPORT_SYMBOL_GPL(ucsi_unregister_ppm);

MODULE_AUTHOR("Heikki Krogerus <heikki.krogerus@linux.intel.com>");
MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("USB Type-C System Software Interface driver");
