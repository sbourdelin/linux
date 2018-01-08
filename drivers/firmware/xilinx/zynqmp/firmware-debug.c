/*
 * Xilinx Zynq MPSoC Firmware layer for debugfs APIs
 *
 *  Copyright (C) 2014-2017 Xilinx, Inc.
 *
 *  Michal Simek <michal.simek@xilinx.com>
 *  Davorin Mista <davorin.mista@aggios.com>
 *  Jolly Shah <jollys@xilinx.com>
 *  Rajan Vaja <rajanv@xilinx.com>
 *
 * SPDX-License-Identifier:	GPL-2.0+
 */

#include <linux/compiler.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/debugfs.h>
#include <linux/uaccess.h>
#include <linux/firmware/xilinx/zynqmp/firmware.h>
#include <linux/firmware/xilinx/zynqmp/firmware-debug.h>

#define DRIVER_NAME	"zynqmp-firmware"

/**
 * zynqmp_pm_self_suspend - PM call for master to suspend itself
 * @node:	Node ID of the master or subsystem
 * @latency:	Requested maximum wakeup latency (not supported)
 * @state:	Requested state (not supported)
 *
 * Return:	Returns status, either success or error+reason
 */
int zynqmp_pm_self_suspend(const u32 node,
			   const u32 latency,
			   const u32 state)
{
	return invoke_pm_fn(SELF_SUSPEND, node, latency, state, 0, NULL);
}

/**
 * zynqmp_pm_abort_suspend - PM call to announce that a prior suspend request
 *				is to be aborted.
 * @reason:	Reason for the abort
 *
 * Return:	Returns status, either success or error+reason
 */
int zynqmp_pm_abort_suspend(const enum zynqmp_pm_abort_reason reason)
{
	return invoke_pm_fn(ABORT_SUSPEND, reason, 0, 0, 0, NULL);
}

/**
 * zynqmp_pm_register_notifier - Register the PU to be notified of PM events
 * @node:	Node ID of the slave
 * @event:	The event to be notified about
 * @wake:	Wake up on event
 * @enable:	Enable or disable the notifier
 *
 * Return:	Returns status, either success or error+reason
 */
int zynqmp_pm_register_notifier(const u32 node, const u32 event,
				const u32 wake, const u32 enable)
{
	return invoke_pm_fn(REGISTER_NOTIFIER, node, event,
			    wake, enable, NULL);
}

/**
 * zynqmp_pm_argument_value - Extract argument value from a PM-API request
 * @arg:	Entered PM-API argument in string format
 *
 * Return:	Argument value in unsigned integer format on success
 *		0 otherwise
 */
static u64 zynqmp_pm_argument_value(char *arg)
{
	u64 value;

	if (!arg)
		return 0;

	if (!kstrtou64(arg, 0, &value))
		return value;

	return 0;
}

static struct dentry *zynqmp_pm_debugfs_dir;
static struct dentry *zynqmp_pm_debugfs_power;
static struct dentry *zynqmp_pm_debugfs_api_version;

/**
 * zynqmp_pm_debugfs_api_write - debugfs write function
 * @file:	User file structure
 * @ptr:	User entered PM-API string
 * @len:	Length of the userspace buffer
 * @off:	Offset within the file
 *
 * Return:	Number of bytes copied if PM-API request succeeds,
 *		the corresponding error code otherwise
 *
 * Used for triggering pm api functions by writing
 * echo <pm_api_id>    > /sys/kernel/debug/zynqmp_pm/power or
 * echo <pm_api_name>  > /sys/kernel/debug/zynqmp_pm/power
 */
static ssize_t zynqmp_pm_debugfs_api_write(struct file *file,
					   const char __user *ptr, size_t len,
					   loff_t *off)
{
	char *kern_buff, *tmp_buff;
	char *pm_api_req;
	u32 pm_id = 0;
	u64 pm_api_arg[4];
	/* Return values from PM APIs calls */
	u32 pm_api_ret[4] = {0, 0, 0, 0};
	u32 pm_api_version;

	int ret;
	int i = 0;
	const struct zynqmp_eemi_ops *eemi_ops = get_eemi_ops();

	if (!eemi_ops)
		return -ENXIO;

	if (*off != 0 || len <= 0)
		return -EINVAL;

	kern_buff = kzalloc(len, GFP_KERNEL);
	if (!kern_buff)
		return -ENOMEM;
	tmp_buff = kern_buff;

	while (i < ARRAY_SIZE(pm_api_arg))
		pm_api_arg[i++] = 0;

	ret = strncpy_from_user(kern_buff, ptr, len);
	if (ret < 0) {
		ret = -EFAULT;
		goto err;
	}

	/* Read the API name from a user request */
	pm_api_req = strsep(&kern_buff, " ");

	if (strncasecmp(pm_api_req, "REQUEST_SUSPEND", 15) == 0)
		pm_id = REQUEST_SUSPEND;
	else if (strncasecmp(pm_api_req, "SELF_SUSPEND", 12) == 0)
		pm_id = SELF_SUSPEND;
	else if (strncasecmp(pm_api_req, "FORCE_POWERDOWN", 15) == 0)
		pm_id = FORCE_POWERDOWN;
	else if (strncasecmp(pm_api_req, "ABORT_SUSPEND", 13) == 0)
		pm_id = ABORT_SUSPEND;
	else if (strncasecmp(pm_api_req, "REQUEST_WAKEUP", 14) == 0)
		pm_id = REQUEST_WAKEUP;
	else if (strncasecmp(pm_api_req, "SET_WAKEUP_SOURCE", 17) == 0)
		pm_id = SET_WAKEUP_SOURCE;
	else if (strncasecmp(pm_api_req, "SYSTEM_SHUTDOWN", 15) == 0)
		pm_id = SYSTEM_SHUTDOWN;
	else if (strncasecmp(pm_api_req, "REQUEST_NODE", 12) == 0)
		pm_id = REQUEST_NODE;
	else if (strncasecmp(pm_api_req, "RELEASE_NODE", 12) == 0)
		pm_id = RELEASE_NODE;
	else if (strncasecmp(pm_api_req, "SET_REQUIREMENT", 15) == 0)
		pm_id = SET_REQUIREMENT;
	else if (strncasecmp(pm_api_req, "SET_MAX_LATENCY", 15) == 0)
		pm_id = SET_MAX_LATENCY;
	else if (strncasecmp(pm_api_req, "GET_API_VERSION", 15) == 0)
		pm_id = GET_API_VERSION;
	else if (strncasecmp(pm_api_req, "SET_CONFIGURATION", 17) == 0)
		pm_id = SET_CONFIGURATION;
	else if (strncasecmp(pm_api_req, "GET_NODE_STATUS", 15) == 0)
		pm_id = GET_NODE_STATUS;
	else if (strncasecmp(pm_api_req,
			     "GET_OPERATING_CHARACTERISTIC", 28) == 0)
		pm_id = GET_OPERATING_CHARACTERISTIC;
	else if (strncasecmp(pm_api_req, "REGISTER_NOTIFIER", 17) == 0)
		pm_id = REGISTER_NOTIFIER;
	else if (strncasecmp(pm_api_req, "RESET_ASSERT", 12) == 0)
		pm_id = RESET_ASSERT;
	else if (strncasecmp(pm_api_req, "RESET_GET_STATUS", 16) == 0)
		pm_id = RESET_GET_STATUS;
	else if (strncasecmp(pm_api_req, "MMIO_READ", 9) == 0)
		pm_id = MMIO_READ;
	else if (strncasecmp(pm_api_req, "MMIO_WRITE", 10) == 0)
		pm_id = MMIO_WRITE;
	else if (strncasecmp(pm_api_req, "GET_CHIPID", 9) == 0)
		pm_id = GET_CHIPID;
	else if (strncasecmp(pm_api_req, "PINCTRL_GET_FUNCTION", 21) == 0)
		pm_id = PINCTRL_GET_FUNCTION;
	else if (strncasecmp(pm_api_req, "PINCTRL_SET_FUNCTION", 21) == 0)
		pm_id = PINCTRL_SET_FUNCTION;
	else if (strncasecmp(pm_api_req,
			     "PINCTRL_CONFIG_PARAM_GET", 25) == 0)
		pm_id = PINCTRL_CONFIG_PARAM_GET;
	else if (strncasecmp(pm_api_req,
			     "PINCTRL_CONFIG_PARAM_SET", 25) == 0)
		pm_id = PINCTRL_CONFIG_PARAM_SET;
	else if (strncasecmp(pm_api_req, "IOCTL", 6) == 0)
		pm_id = IOCTL;
	else if (strncasecmp(pm_api_req, "CLOCK_ENABLE", 12) == 0)
		pm_id = CLOCK_ENABLE;
	else if (strncasecmp(pm_api_req, "CLOCK_DISABLE", 13) == 0)
		pm_id = CLOCK_DISABLE;
	else if (strncasecmp(pm_api_req, "CLOCK_GETSTATE", 14) == 0)
		pm_id = CLOCK_GETSTATE;
	else if (strncasecmp(pm_api_req, "CLOCK_SETDIVIDER", 16) == 0)
		pm_id = CLOCK_SETDIVIDER;
	else if (strncasecmp(pm_api_req, "CLOCK_GETDIVIDER", 16) == 0)
		pm_id = CLOCK_GETDIVIDER;
	else if (strncasecmp(pm_api_req, "CLOCK_SETRATE", 13) == 0)
		pm_id = CLOCK_SETRATE;
	else if (strncasecmp(pm_api_req, "CLOCK_GETRATE", 13) == 0)
		pm_id = CLOCK_GETRATE;
	else if (strncasecmp(pm_api_req, "CLOCK_SETPARENT", 15) == 0)
		pm_id = CLOCK_SETPARENT;
	else if (strncasecmp(pm_api_req, "CLOCK_GETPARENT", 15) == 0)
		pm_id = CLOCK_GETPARENT;
	else if (strncasecmp(pm_api_req, "QUERY_DATA", 22) == 0)
		pm_id = QUERY_DATA;

	/* If no name was entered look for PM-API ID instead */
	else if (kstrtouint(pm_api_req, 10, &pm_id))
		ret = -EINVAL;

	/* Read node_id and arguments from the PM-API request */
	i = 0;
	pm_api_req = strsep(&kern_buff, " ");
	while ((i < ARRAY_SIZE(pm_api_arg)) && pm_api_req) {
		pm_api_arg[i++] = zynqmp_pm_argument_value(pm_api_req);
		pm_api_req = strsep(&kern_buff, " ");
	}

	switch (pm_id) {
	case GET_API_VERSION:
		eemi_ops->get_api_version(&pm_api_version);
		pr_info("%s PM-API Version = %d.%d\n", __func__,
			pm_api_version >> 16, pm_api_version & 0xffff);
		break;
	case REQUEST_SUSPEND:
		ret = eemi_ops->request_suspend(pm_api_arg[0],
						pm_api_arg[1] ? pm_api_arg[1] :
						ZYNQMP_PM_REQUEST_ACK_NO,
						pm_api_arg[2] ? pm_api_arg[2] :
						ZYNQMP_PM_MAX_LATENCY, 0);
		break;
	case SELF_SUSPEND:
		ret = zynqmp_pm_self_suspend(pm_api_arg[0],
					     pm_api_arg[1] ? pm_api_arg[1] :
					     ZYNQMP_PM_MAX_LATENCY, 0);
		break;
	case FORCE_POWERDOWN:
		ret = eemi_ops->force_powerdown(pm_api_arg[0],
						pm_api_arg[1] ? pm_api_arg[1] :
						ZYNQMP_PM_REQUEST_ACK_NO);
		break;
	case ABORT_SUSPEND:
		ret = zynqmp_pm_abort_suspend(pm_api_arg[0] ? pm_api_arg[0] :
					      ZYNQMP_PM_ABORT_REASON_UNKNOWN);
		break;
	case REQUEST_WAKEUP:
		ret = eemi_ops->request_wakeup(pm_api_arg[0],
					       pm_api_arg[1], pm_api_arg[2],
					       pm_api_arg[3] ? pm_api_arg[3] :
					       ZYNQMP_PM_REQUEST_ACK_NO);
		break;
	case SET_WAKEUP_SOURCE:
		ret = eemi_ops->set_wakeup_source(pm_api_arg[0], pm_api_arg[1],
						  pm_api_arg[2]);
		break;
	case SYSTEM_SHUTDOWN:
		ret = eemi_ops->system_shutdown(pm_api_arg[0], pm_api_arg[1]);
		break;
	case REQUEST_NODE:
		ret = eemi_ops->request_node(pm_api_arg[0],
					     pm_api_arg[1] ? pm_api_arg[1] :
					     ZYNQMP_PM_CAPABILITY_ACCESS,
					     pm_api_arg[2] ? pm_api_arg[2] : 0,
					     pm_api_arg[3] ? pm_api_arg[3] :
					     ZYNQMP_PM_REQUEST_ACK_BLOCKING);
		break;
	case RELEASE_NODE:
		ret = eemi_ops->release_node(pm_api_arg[0]);
		break;
	case SET_REQUIREMENT:
		ret = eemi_ops->set_requirement(pm_api_arg[0],
						pm_api_arg[1] ? pm_api_arg[1] :
						ZYNQMP_PM_CAPABILITY_CONTEXT,
						pm_api_arg[2] ?
						pm_api_arg[2] : 0,
						pm_api_arg[3] ? pm_api_arg[3] :
						ZYNQMP_PM_REQUEST_ACK_BLOCKING);
		break;
	case SET_MAX_LATENCY:
		ret = eemi_ops->set_max_latency(pm_api_arg[0],
						pm_api_arg[1] ? pm_api_arg[1] :
						ZYNQMP_PM_MAX_LATENCY);
		break;
	case SET_CONFIGURATION:
		ret = eemi_ops->set_configuration(pm_api_arg[0]);
		break;
	case GET_NODE_STATUS:
		ret = eemi_ops->get_node_status(pm_api_arg[0],
						&pm_api_ret[0],
						&pm_api_ret[1],
						&pm_api_ret[2]);
		if (!ret)
			pr_info("GET_NODE_STATUS:\n\tNodeId: %llu\n\tStatus: %u\n\tRequirements: %u\n\tUsage: %u\n",
				pm_api_arg[0], pm_api_ret[0],
				pm_api_ret[1], pm_api_ret[2]);
		break;
	case GET_OPERATING_CHARACTERISTIC:
		ret = eemi_ops->get_operating_characteristic(pm_api_arg[0],
				pm_api_arg[1] ? pm_api_arg[1] :
				ZYNQMP_PM_OPERATING_CHARACTERISTIC_POWER,
				&pm_api_ret[0]);
		if (!ret)
			pr_info("GET_OPERATING_CHARACTERISTIC:\n\tNodeId: %llu\n\tType: %llu\n\tResult: %u\n",
				pm_api_arg[0], pm_api_arg[1], pm_api_ret[0]);
		break;
	case REGISTER_NOTIFIER:
		ret = zynqmp_pm_register_notifier(pm_api_arg[0],
						  pm_api_arg[1] ?
						  pm_api_arg[1] : 0,
						  pm_api_arg[2] ?
						  pm_api_arg[2] : 0,
						  pm_api_arg[3] ?
						  pm_api_arg[3] : 0);
		break;
	case RESET_ASSERT:
		ret = eemi_ops->reset_assert(pm_api_arg[0], pm_api_arg[1]);
		break;
	case RESET_GET_STATUS:
		ret = eemi_ops->reset_get_status(pm_api_arg[0], &pm_api_ret[0]);
		pr_info("%s Reset status: %u\n", __func__, pm_api_ret[0]);
		break;
	case GET_CHIPID:
		ret = eemi_ops->get_chipid(&pm_api_ret[0], &pm_api_ret[1]);
		pr_info("%s idcode: %#x, version:%#x\n",
			__func__, pm_api_ret[0], pm_api_ret[1]);
		break;
	case PINCTRL_GET_FUNCTION:
		ret = eemi_ops->pinctrl_get_function(pm_api_arg[0],
						     &pm_api_ret[0]);
		pr_info("%s Current set function for the pin: %u\n",
			__func__, pm_api_ret[0]);
		break;
	case PINCTRL_SET_FUNCTION:
		ret = eemi_ops->pinctrl_set_function(pm_api_arg[0],
						     pm_api_arg[1]);
		break;
	case PINCTRL_CONFIG_PARAM_GET:
		ret = eemi_ops->pinctrl_get_config(pm_api_arg[0], pm_api_arg[1],
						   &pm_api_ret[0]);
		pr_info("%s pin: %llu, param: %llu, value: %u\n",
			__func__, pm_api_arg[0], pm_api_arg[1],
			pm_api_ret[0]);
		break;
	case PINCTRL_CONFIG_PARAM_SET:
		ret = eemi_ops->pinctrl_set_config(pm_api_arg[0],
						   pm_api_arg[1],
						   pm_api_arg[2]);
		break;
	case IOCTL:
		ret = eemi_ops->ioctl(pm_api_arg[0], pm_api_arg[1],
				      pm_api_arg[2], pm_api_arg[3],
				      &pm_api_ret[0]);
		if (pm_api_arg[1] == IOCTL_GET_RPU_OPER_MODE ||
		    pm_api_arg[1] == IOCTL_GET_PLL_FRAC_MODE ||
		    pm_api_arg[1] == IOCTL_GET_PLL_FRAC_DATA ||
		    pm_api_arg[1] == IOCTL_READ_GGS ||
		    pm_api_arg[1] == IOCTL_READ_PGGS)
			pr_info("%s Value: %u\n",
				__func__, pm_api_ret[1]);
		break;
	case CLOCK_ENABLE:
		ret = eemi_ops->clock_enable(pm_api_arg[0]);
		break;
	case CLOCK_DISABLE:
		ret = eemi_ops->clock_disable(pm_api_arg[0]);
		break;
	case CLOCK_GETSTATE:
		ret = eemi_ops->clock_getstate(pm_api_arg[0], &pm_api_ret[0]);
		pr_info("%s state: %u\n", __func__, pm_api_ret[0]);
		break;
	case CLOCK_SETDIVIDER:
		ret = eemi_ops->clock_setdivider(pm_api_arg[0], pm_api_arg[1]);
		break;
	case CLOCK_GETDIVIDER:
		ret = eemi_ops->clock_getdivider(pm_api_arg[0], &pm_api_ret[0]);
		pr_info("%s Divider Value: %d\n", __func__, pm_api_ret[0]);
		break;
	case CLOCK_SETRATE:
		ret = eemi_ops->clock_setrate(pm_api_arg[0], pm_api_arg[1]);
		break;
	case CLOCK_GETRATE:
		ret = eemi_ops->clock_getrate(pm_api_arg[0], &pm_api_ret[0]);
		pr_info("%s Rate Value: %u\n", __func__, pm_api_ret[0]);
		break;
	case CLOCK_SETPARENT:
		ret = eemi_ops->clock_setparent(pm_api_arg[0], pm_api_arg[1]);
		break;
	case CLOCK_GETPARENT:
		ret = eemi_ops->clock_getparent(pm_api_arg[0], &pm_api_ret[0]);
		pr_info("%s Parent Index: %u\n", __func__, pm_api_ret[0]);
		break;
	case QUERY_DATA:
	{
		struct zynqmp_pm_query_data qdata = {0};

		qdata.qid = pm_api_arg[0];
		qdata.arg1 = pm_api_arg[1];
		qdata.arg2 = pm_api_arg[2];
		qdata.arg3 = pm_api_arg[3];

		ret = eemi_ops->query_data(qdata, pm_api_ret);

		pr_info("%s: data[0] = 0x%08x\n", __func__, pm_api_ret[0]);
		pr_info("%s: data[1] = 0x%08x\n", __func__, pm_api_ret[1]);
		pr_info("%s: data[2] = 0x%08x\n", __func__, pm_api_ret[2]);
		pr_info("%s: data[3] = 0x%08x\n", __func__, pm_api_ret[3]);
		break;
	}
	default:
		pr_err("%s Unsupported PM-API request\n", __func__);
		ret = -EINVAL;
	}

err:
	kfree(tmp_buff);
	if (ret)
		return ret;

	return len;
}

/**
 * zynqmp_pm_debugfs_api_version_read - debugfs read function
 * @file:	User file structure
 * @ptr:	Requested pm_api_version string
 * @len:	Length of the userspace buffer
 * @off:	Offset within the file
 *
 * Return:	Length of the version string on success
 *		-EFAULT otherwise
 *
 * Used to display the pm api version.
 * cat /sys/kernel/debug/zynqmp_pm/pm_api_version
 */
static ssize_t zynqmp_pm_debugfs_api_version_read(struct file *file,
						  char __user *ptr, size_t len,
						  loff_t *off)
{
	char *kern_buff;
	int ret;
	int kern_buff_len;
	u32 pm_api_version;
	const struct zynqmp_eemi_ops *eemi_ops = get_eemi_ops();

	if (!eemi_ops || !eemi_ops->get_api_version)
		return -ENXIO;

	if (len <= 0)
		return -EINVAL;

	if (*off != 0)
		return 0;

	kern_buff = kzalloc(len, GFP_KERNEL);
	if (!kern_buff)
		return -ENOMEM;

	eemi_ops->get_api_version(&pm_api_version);
	sprintf(kern_buff, "PM-API Version = %d.%d\n",
		pm_api_version >> 16, pm_api_version & 0xffff);
	kern_buff_len = strlen(kern_buff) + 1;

	if (len > kern_buff_len)
		len = kern_buff_len;
	ret = copy_to_user(ptr, kern_buff, len);

	kfree(kern_buff);
	if (ret)
		return -EFAULT;

	*off = len + 1;

	return len;
}

/* Setup debugfs fops */
static const struct file_operations fops_zynqmp_pm_dbgfs = {
	.owner  =	THIS_MODULE,
	.write  =	zynqmp_pm_debugfs_api_write,
	.read   =	zynqmp_pm_debugfs_api_version_read,
};

/**
 * zynqmp_pm_api_debugfs_init - Initialize debugfs interface
 *
 * Return:      Returns 0 on success
 *		Corresponding error code otherwise
 */
int zynqmp_pm_api_debugfs_init(void)
{
	int err;

	/* Initialize debugfs interface */
	zynqmp_pm_debugfs_dir = debugfs_create_dir(DRIVER_NAME, NULL);
	if (!zynqmp_pm_debugfs_dir) {
		pr_err("debugfs_create_dir failed\n");
		return -ENODEV;
	}

	zynqmp_pm_debugfs_power =
		debugfs_create_file("pm", 0220,
				    zynqmp_pm_debugfs_dir, NULL,
				    &fops_zynqmp_pm_dbgfs);
	if (!zynqmp_pm_debugfs_power) {
		pr_err("debugfs_create_file power failed\n");
		err = -ENODEV;
		goto err_dbgfs;
	}

	zynqmp_pm_debugfs_api_version =
		debugfs_create_file("api_version", 0444,
				    zynqmp_pm_debugfs_dir, NULL,
				    &fops_zynqmp_pm_dbgfs);
	if (!zynqmp_pm_debugfs_api_version) {
		pr_err("debugfs_create_file api_version failed\n");
		err = -ENODEV;
		goto err_dbgfs;
	}

	return 0;

err_dbgfs:
	debugfs_remove_recursive(zynqmp_pm_debugfs_dir);
	zynqmp_pm_debugfs_dir = NULL;

	return err;
}
