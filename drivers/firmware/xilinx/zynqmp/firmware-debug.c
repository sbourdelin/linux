// SPDX-License-Identifier: GPL-2.0+
/*
 * Xilinx Zynq MPSoC Firmware layer for debugfs APIs
 *
 *  Copyright (C) 2014-2018 Xilinx, Inc.
 *
 *  Michal Simek <michal.simek@xilinx.com>
 *  Davorin Mista <davorin.mista@aggios.com>
 *  Jolly Shah <jollys@xilinx.com>
 *  Rajan Vaja <rajanv@xilinx.com>
 */

#include <linux/compiler.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/debugfs.h>
#include <linux/uaccess.h>
#include <linux/firmware/xilinx/zynqmp/firmware.h>
#include <linux/firmware/xilinx/zynqmp/firmware-debug.h>

#define DRIVER_NAME	"zynqmp-firmware"

#define PM_API_NAME_LEN			50
#define MAX_API_VERSION_RESP_LEN	50

struct pm_api_info {
	u32 api_id;
	char api_name[PM_API_NAME_LEN];
	char api_name_len;
};

#define PM_API(id)		 {id, #id, strlen(#id)}
static struct pm_api_info pm_api_list[] = {
	PM_API(PM_REQUEST_SUSPEND),
	PM_API(PM_SELF_SUSPEND),
	PM_API(PM_FORCE_POWERDOWN),
	PM_API(PM_ABORT_SUSPEND),
	PM_API(PM_REQUEST_WAKEUP),
	PM_API(PM_SET_WAKEUP_SOURCE),
	PM_API(PM_SYSTEM_SHUTDOWN),
	PM_API(PM_REQUEST_NODE),
	PM_API(PM_RELEASE_NODE),
	PM_API(PM_SET_REQUIREMENT),
	PM_API(PM_SET_MAX_LATENCY),
	PM_API(PM_GET_API_VERSION),
	PM_API(PM_SET_CONFIGURATION),
	PM_API(PM_GET_NODE_STATUS),
	PM_API(PM_GET_OPERATING_CHARACTERISTIC),
	PM_API(PM_REGISTER_NOTIFIER),
	PM_API(PM_RESET_ASSERT),
	PM_API(PM_RESET_GET_STATUS),
	PM_API(PM_MMIO_READ),
	PM_API(PM_MMIO_WRITE),
	PM_API(PM_GET_CHIPID),
	PM_API(PM_PINCTRL_GET_FUNCTION),
	PM_API(PM_PINCTRL_SET_FUNCTION),
	PM_API(PM_PINCTRL_CONFIG_PARAM_GET),
	PM_API(PM_PINCTRL_CONFIG_PARAM_SET),
	PM_API(PM_IOCTL),
	PM_API(PM_CLOCK_ENABLE),
	PM_API(PM_CLOCK_DISABLE),
	PM_API(PM_CLOCK_GETSTATE),
	PM_API(PM_CLOCK_SETDIVIDER),
	PM_API(PM_CLOCK_GETDIVIDER),
	PM_API(PM_CLOCK_SETRATE),
	PM_API(PM_CLOCK_GETRATE),
	PM_API(PM_CLOCK_SETPARENT),
	PM_API(PM_CLOCK_GETPARENT),
	PM_API(PM_QUERY_DATA),
};

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
	return invoke_pm_fn(PM_SELF_SUSPEND, node, latency, state, 0, NULL);
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
	return invoke_pm_fn(PM_ABORT_SUSPEND, reason, 0, 0, 0, NULL);
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
	return invoke_pm_fn(PM_REGISTER_NOTIFIER, node, event,
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

static int get_pm_api_id(char *pm_api_req, u32 *pm_id)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(pm_api_list) ; i++) {
		if (!strncasecmp(pm_api_req, pm_api_list[i].api_name,
				 pm_api_list[i].api_name_len)) {
			*pm_id = pm_api_list[i].api_id;
			break;
		}
	}

	/* If no name was entered look for PM-API ID instead */
	if (i == ARRAY_SIZE(pm_api_list) && kstrtouint(pm_api_req, 10, pm_id))
		return -EINVAL;

	return 0;
}

static int process_api_request(u32 pm_id, u64 *pm_api_arg, u32 *pm_api_ret)
{
	const struct zynqmp_eemi_ops *eemi_ops = get_eemi_ops();
	u32 pm_api_version;
	int ret;

	if (!eemi_ops)
		return -ENXIO;

	switch (pm_id) {
	case PM_GET_API_VERSION:
		eemi_ops->get_api_version(&pm_api_version);
		pr_info("%s PM-API Version = %d.%d\n", __func__,
			pm_api_version >> 16, pm_api_version & 0xffff);
		break;
	case PM_REQUEST_SUSPEND:
		ret = eemi_ops->request_suspend(pm_api_arg[0],
						pm_api_arg[1] ? pm_api_arg[1] :
						ZYNQMP_PM_REQUEST_ACK_NO,
						pm_api_arg[2] ? pm_api_arg[2] :
						ZYNQMP_PM_MAX_LATENCY, 0);
		break;
	case PM_SELF_SUSPEND:
		ret = zynqmp_pm_self_suspend(pm_api_arg[0],
					     pm_api_arg[1] ? pm_api_arg[1] :
					     ZYNQMP_PM_MAX_LATENCY, 0);
		break;
	case PM_FORCE_POWERDOWN:
		ret = eemi_ops->force_powerdown(pm_api_arg[0],
						pm_api_arg[1] ? pm_api_arg[1] :
						ZYNQMP_PM_REQUEST_ACK_NO);
		break;
	case PM_ABORT_SUSPEND:
		ret = zynqmp_pm_abort_suspend(pm_api_arg[0] ? pm_api_arg[0] :
					      ZYNQMP_PM_ABORT_REASON_UNKNOWN);
		break;
	case PM_REQUEST_WAKEUP:
		ret = eemi_ops->request_wakeup(pm_api_arg[0],
					       pm_api_arg[1], pm_api_arg[2],
					       pm_api_arg[3] ? pm_api_arg[3] :
					       ZYNQMP_PM_REQUEST_ACK_NO);
		break;
	case PM_SET_WAKEUP_SOURCE:
		ret = eemi_ops->set_wakeup_source(pm_api_arg[0], pm_api_arg[1],
						  pm_api_arg[2]);
		break;
	case PM_SYSTEM_SHUTDOWN:
		ret = eemi_ops->system_shutdown(pm_api_arg[0], pm_api_arg[1]);
		break;
	case PM_REQUEST_NODE:
		ret = eemi_ops->request_node(pm_api_arg[0],
					     pm_api_arg[1] ? pm_api_arg[1] :
					     ZYNQMP_PM_CAPABILITY_ACCESS,
					     pm_api_arg[2] ? pm_api_arg[2] : 0,
					     pm_api_arg[3] ? pm_api_arg[3] :
					     ZYNQMP_PM_REQUEST_ACK_BLOCKING);
		break;
	case PM_RELEASE_NODE:
		ret = eemi_ops->release_node(pm_api_arg[0]);
		break;
	case PM_SET_REQUIREMENT:
		ret = eemi_ops->set_requirement(pm_api_arg[0],
						pm_api_arg[1] ? pm_api_arg[1] :
						ZYNQMP_PM_CAPABILITY_CONTEXT,
						pm_api_arg[2] ?
						pm_api_arg[2] : 0,
						pm_api_arg[3] ? pm_api_arg[3] :
						ZYNQMP_PM_REQUEST_ACK_BLOCKING);
		break;
	case PM_SET_MAX_LATENCY:
		ret = eemi_ops->set_max_latency(pm_api_arg[0],
						pm_api_arg[1] ? pm_api_arg[1] :
						ZYNQMP_PM_MAX_LATENCY);
		break;
	case PM_SET_CONFIGURATION:
		ret = eemi_ops->set_configuration(pm_api_arg[0]);
		break;
	case PM_GET_NODE_STATUS:
		ret = eemi_ops->get_node_status(pm_api_arg[0],
						&pm_api_ret[0],
						&pm_api_ret[1],
						&pm_api_ret[2]);
		if (!ret)
			pr_info("GET_NODE_STATUS:\n\tNodeId: %llu\n\tStatus: %u\n\tRequirements: %u\n\tUsage: %u\n",
				pm_api_arg[0], pm_api_ret[0],
				pm_api_ret[1], pm_api_ret[2]);
		break;
	case PM_GET_OPERATING_CHARACTERISTIC:
		ret = eemi_ops->get_operating_characteristic(pm_api_arg[0],
				pm_api_arg[1] ? pm_api_arg[1] :
				ZYNQMP_PM_OPERATING_CHARACTERISTIC_POWER,
				&pm_api_ret[0]);
		if (!ret)
			pr_info("GET_OPERATING_CHARACTERISTIC:\n\tNodeId: %llu\n\tType: %llu\n\tResult: %u\n",
				pm_api_arg[0], pm_api_arg[1], pm_api_ret[0]);
		break;
	case PM_REGISTER_NOTIFIER:
		ret = zynqmp_pm_register_notifier(pm_api_arg[0],
						  pm_api_arg[1] ?
						  pm_api_arg[1] : 0,
						  pm_api_arg[2] ?
						  pm_api_arg[2] : 0,
						  pm_api_arg[3] ?
						  pm_api_arg[3] : 0);
		break;
	case PM_RESET_ASSERT:
		ret = eemi_ops->reset_assert(pm_api_arg[0], pm_api_arg[1]);
		break;
	case PM_RESET_GET_STATUS:
		ret = eemi_ops->reset_get_status(pm_api_arg[0], &pm_api_ret[0]);
		pr_info("%s Reset status: %u\n", __func__, pm_api_ret[0]);
		break;
	case PM_GET_CHIPID:
		ret = eemi_ops->get_chipid(&pm_api_ret[0], &pm_api_ret[1]);
		pr_info("%s idcode: %#x, version:%#x\n",
			__func__, pm_api_ret[0], pm_api_ret[1]);
		break;
	case PM_PINCTRL_GET_FUNCTION:
		ret = eemi_ops->pinctrl_get_function(pm_api_arg[0],
						     &pm_api_ret[0]);
		pr_info("%s Current set function for the pin: %u\n",
			__func__, pm_api_ret[0]);
		break;
	case PM_PINCTRL_SET_FUNCTION:
		ret = eemi_ops->pinctrl_set_function(pm_api_arg[0],
						     pm_api_arg[1]);
		break;
	case PM_PINCTRL_CONFIG_PARAM_GET:
		ret = eemi_ops->pinctrl_get_config(pm_api_arg[0], pm_api_arg[1],
						   &pm_api_ret[0]);
		pr_info("%s pin: %llu, param: %llu, value: %u\n",
			__func__, pm_api_arg[0], pm_api_arg[1],
			pm_api_ret[0]);
		break;
	case PM_PINCTRL_CONFIG_PARAM_SET:
		ret = eemi_ops->pinctrl_set_config(pm_api_arg[0],
						   pm_api_arg[1],
						   pm_api_arg[2]);
		break;
	case PM_IOCTL:
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
	case PM_CLOCK_ENABLE:
		ret = eemi_ops->clock_enable(pm_api_arg[0]);
		break;
	case PM_CLOCK_DISABLE:
		ret = eemi_ops->clock_disable(pm_api_arg[0]);
		break;
	case PM_CLOCK_GETSTATE:
		ret = eemi_ops->clock_getstate(pm_api_arg[0], &pm_api_ret[0]);
		pr_info("%s state: %u\n", __func__, pm_api_ret[0]);
		break;
	case PM_CLOCK_SETDIVIDER:
		ret = eemi_ops->clock_setdivider(pm_api_arg[0], pm_api_arg[1]);
		break;
	case PM_CLOCK_GETDIVIDER:
		ret = eemi_ops->clock_getdivider(pm_api_arg[0], &pm_api_ret[0]);
		pr_info("%s Divider Value: %d\n", __func__, pm_api_ret[0]);
		break;
	case PM_CLOCK_SETRATE:
		ret = eemi_ops->clock_setrate(pm_api_arg[0], pm_api_arg[1]);
		break;
	case PM_CLOCK_GETRATE:
		ret = eemi_ops->clock_getrate(pm_api_arg[0], &pm_api_ret[0]);
		pr_info("%s Rate Value: %u\n", __func__, pm_api_ret[0]);
		break;
	case PM_CLOCK_SETPARENT:
		ret = eemi_ops->clock_setparent(pm_api_arg[0], pm_api_arg[1]);
		break;
	case PM_CLOCK_GETPARENT:
		ret = eemi_ops->clock_getparent(pm_api_arg[0], &pm_api_ret[0]);
		pr_info("%s Parent Index: %u\n", __func__, pm_api_ret[0]);
		break;
	case PM_QUERY_DATA:
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

	return ret;
}

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
	u64 pm_api_arg[4] = {0, 0, 0, 0};
	/* Return values from PM APIs calls */
	u32 pm_api_ret[4] = {0, 0, 0, 0};

	int ret;
	int i = 0;

	if (*off != 0 || len == 0)
		return -EINVAL;

	kern_buff = memdup_user(ptr, len);
	if (IS_ERR(kern_buff))
		return PTR_ERR(kern_buff);

	tmp_buff = kern_buff;

	/* Read the API name from a user request */
	pm_api_req = strsep(&kern_buff, " ");

	ret = get_pm_api_id(pm_api_req, &pm_id);
	if (ret < 0)
		goto err;

	/* Read node_id and arguments from the PM-API request */
	pm_api_req = strsep(&kern_buff, " ");
	while ((i < ARRAY_SIZE(pm_api_arg)) && pm_api_req) {
		pm_api_arg[i++] = zynqmp_pm_argument_value(pm_api_req);
		pm_api_req = strsep(&kern_buff, " ");
	}

	ret = process_api_request(pm_id, pm_api_arg, pm_api_ret);

err:
	kfree(tmp_buff);
	if (ret)
		return ret;
	else
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
	int ret;
	u32 pm_api_version;
	const struct zynqmp_eemi_ops *eemi_ops = get_eemi_ops();
	char resp[MAX_API_VERSION_RESP_LEN];
	u32 resp_len;

	if (!eemi_ops || !eemi_ops->get_api_version)
		return -ENXIO;

	if (*off != 0)
		return 0;

	eemi_ops->get_api_version(&pm_api_version);
	resp_len = snprintf(resp, MAX_API_VERSION_RESP_LEN,
			    "PM-API Version = %d.%d\n",
			    pm_api_version >> 16,
			    pm_api_version & 0xffff);

	if (len < resp_len)
		return -EINVAL;

	ret = copy_to_user(ptr, resp, resp_len);
	if (ret)
		return -EFAULT;

	*off = resp_len + 1;

	return resp_len;
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
 * Return:      None
 */
void zynqmp_pm_api_debugfs_init(void)
{
	struct dentry *root_dir;
	struct dentry *d;

	/* Initialize debugfs interface */
	root_dir = debugfs_create_dir(DRIVER_NAME, NULL);
	if (!root_dir) {
		pr_warn("debugfs_create_dir failed\n");
		return;
	}

	d = debugfs_create_file("pm", 0220, root_dir, NULL,
				&fops_zynqmp_pm_dbgfs);
	if (!d) {
		pr_warn("debugfs_create_file power failed\n");
		goto err_dbgfs;
	}

	d = debugfs_create_file("api_version", 0444, root_dir,
				NULL, &fops_zynqmp_pm_dbgfs);
	if (!d) {
		pr_warn("debugfs_create_file api_version failed\n");
		goto err_dbgfs;
	}

	return;

err_dbgfs:
	debugfs_remove_recursive(root_dir);
}
