// SPDX-License-Identifier: GPL-2.0
/*
 * HMS Profinet Client Driver
 *
 * Copyright (C) 2018 Arcx Inc
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/idr.h>
#include <linux/miscdevice.h>

#include <linux/anybuss-client.h>
#include <uapi/linux/hms-profinet.h>

#define PROFI_DPRAM_SIZE	512

/*
 *  --------------------------------------------------------------
 * Anybus Profinet mailbox messages - definitions
 * --------------------------------------------------------------
 */

/*
 * note that we're depending on the layout of these structures being
 * exactly as advertised - which means they need to be packed.
 */

struct msgEthConfig {
	u32 ip_addr, subnet_msk, gateway_addr;
} __packed;

struct msgMacAddr {
	u8 addr[6];
} __packed;

struct msgStr {
	char	s[128];
} __packed;

struct msgShortStr {
	char	s[64];
} __packed;

struct msgHicp {
	char	enable;
} __packed;

/*
 * --------------------------------------------------------------
 * Fieldbus Specific Area - memory locations
 * --------------------------------------------------------------
 */
#define FSA_NETWORK_STATUS	0x700
#define FSA_LAYER_STATUS	0x7B2
#define FSA_IO_CTRL_STATUS	0x7B0
#define FSA_LAYER_FAULT_CODE	0x7B4

struct profi_priv {
	struct anybuss_client *client;
	int id;
	atomic_t refcount;
	char node_name[16];
	struct miscdevice misc;
	struct device *dev;	/* just a link to the misc device */
	struct mutex enable_lock;
};

static int profinet_configure(struct anybuss_client *ab,
				struct ProfinetConfig *cfg)
{
	int ret;

	if (cfg->eth.is_valid) {
		struct msgEthConfig msg = {
			.ip_addr = cfg->eth.ip_addr,
			.subnet_msk = cfg->eth.subnet_msk,
			.gateway_addr = cfg->eth.gateway_addr,
		};
		ret = anybuss_send_msg(ab, 0x0001, &msg, sizeof(msg));
		if (ret)
			return ret;
	}
	if (cfg->dev_id.is_valid) {
		u16 ext[2] = {
			cpu_to_be16(cfg->dev_id.vendorid),
			cpu_to_be16(cfg->dev_id.deviceid)
		};
		ret = anybuss_send_ext(ab, 0x0102, ext, sizeof(ext));
		if (ret)
			return ret;
	}
	if (cfg->station_name.is_valid) {
		struct msgStr msg = { 0 };

		strncpy(msg.s, cfg->station_name.name, sizeof(msg.s));
		ret = anybuss_send_msg(ab, 0x0103, &msg, sizeof(msg));
		if (ret)
			return ret;
	}
	if (cfg->station_type.is_valid) {
		struct msgShortStr msg = { 0 };

		strncpy(msg.s, cfg->station_type.name, sizeof(msg.s));
		ret = anybuss_send_msg(ab, 0x0104, &msg, sizeof(msg));
		if (ret)
			return ret;
	}
	if (cfg->mac_addr.is_valid) {
		struct msgMacAddr msg = { 0 };

		memcpy(msg.addr, cfg->mac_addr.addr, sizeof(msg.addr));
		ret = anybuss_send_msg(ab, 0x0019, &msg, sizeof(msg));
		if (ret)
			return ret;
	}
	if (cfg->host_domain.is_valid) {
		size_t len;
		struct msgStr msg = { 0 };
		/*
		 * check if host and domain names fit in msg structure
		 */
		len =	strnlen(cfg->host_domain.hostname,
				sizeof(cfg->host_domain.hostname))
					+ 1 +
			strnlen(cfg->host_domain.domainname,
				sizeof(cfg->host_domain.domainname))
					+ 1;
		if (len > sizeof(msg.s))
			return -ENAMETOOLONG;
		strncpy(msg.s, cfg->host_domain.hostname,
			sizeof(msg.s));
		len = strnlen(msg.s, sizeof(msg.s)) + 1; /* NULL term */
		strncpy(msg.s + len, cfg->host_domain.domainname,
				sizeof(msg.s) - len);
		ret = anybuss_send_msg(ab, 0x0032, &msg, sizeof(msg));
		if (ret)
			return ret;
	}
	if (cfg->hicp.is_valid) {
		struct msgHicp msg = {
			.enable = cfg->hicp.enable ? 1 : 0,
		};
		ret = anybuss_send_msg(ab, 0x0013, &msg, sizeof(msg));
		if (ret)
			return ret;
	}
	if (cfg->web_server.is_valid) {
		ret = anybuss_send_msg(ab,
			cfg->web_server.enable ? 0x0005 : 0x0004,
			NULL, 0);
		if (ret)
			return ret;
	}
	if (cfg->ftp_server.disable) {
		ret = anybuss_send_msg(ab, 0x0006, NULL, 0);
		if (ret)
			return ret;
	}
	if (cfg->global_admin_mode.enable) {
		ret = anybuss_send_msg(ab, 0x000B, NULL, 0);
		if (ret)
			return ret;
	}
	if (cfg->vfs.disable) {
		ret = anybuss_send_msg(ab, 0x0011, NULL, 0);
		if (ret)
			return ret;
	}
	if (cfg->stop_mode.is_valid) {
		u16 action;

		switch (cfg->stop_mode.action) {
		case HMS_SMA_CLEAR:
			action = 0;
			break;
		case HMS_SMA_FREEZE:
			action = 1;
			break;
		case HMS_SMA_SET:
			action = 2;
			break;
		default:
			return -EINVAL;
		}
		action = cpu_to_be16(action);
		ret = anybuss_send_ext(ab, 0x0101, &action,
						sizeof(action));
		if (ret)
			return ret;
	}
	if (cfg->snmp_system_descr.is_valid) {
		struct msgStr msg = { 0 };

		strncpy(msg.s, cfg->snmp_system_descr.description,
					sizeof(msg.s));
		ret = anybuss_send_msg(ab, 0x0120, &msg, sizeof(msg));
		if (ret)
			return ret;
	}
	if (cfg->snmp_iface_descr.is_valid) {
		struct msgStr msg = { 0 };

		strncpy(msg.s, cfg->snmp_iface_descr.description,
					sizeof(msg.s));
		ret = anybuss_send_msg(ab, 0x0121, &msg, sizeof(msg));
		if (ret)
			return ret;
	}
	if (cfg->mib2_system_descr.is_valid) {
		struct msgStr msg = { 0 };

		strncpy(msg.s, cfg->mib2_system_descr.description,
					sizeof(msg.s));
		ret = anybuss_send_msg(ab, 0x0124, &msg, sizeof(msg));
		if (ret)
			return ret;
	}
	if (cfg->mib2_system_contact.is_valid) {
		struct msgStr msg = { 0 };

		strncpy(msg.s, cfg->mib2_system_contact.contact,
					sizeof(msg.s));
		ret = anybuss_send_msg(ab, 0x0125, &msg, sizeof(msg));
		if (ret)
			return ret;
	}
	if (cfg->mib2_system_location.is_valid) {
		struct msgStr msg = { 0 };

		strncpy(msg.s, cfg->mib2_system_location.location,
					sizeof(msg.s));
		ret = anybuss_send_msg(ab, 0x0126, &msg, sizeof(msg));
		if (ret)
			return ret;
	}
	return 0;
}

static int profinet_enable(struct profi_priv *priv,
				struct ProfinetConfig *cfg)
{
	int ret;
	struct anybuss_client *client = priv->client;

	/* Initialization Sequence, Generic Anybus Mode */
	const struct anybuss_memcfg mem_cfg = {
		.input_io = 220,
		.input_dpram = PROFI_DPRAM_SIZE,
		.input_total = PROFI_DPRAM_SIZE,
		.output_io = 220,
		.output_dpram = PROFI_DPRAM_SIZE,
		.output_total = PROFI_DPRAM_SIZE,
		.offl_mode = AB_OFFL_MODE_CLEAR,
	};
	if (mutex_lock_interruptible(&priv->enable_lock))
		return -ERESTARTSYS;
	/*
	 * switch anybus off then on, this ensures we can do a complete
	 * configuration cycle in case anybus was already on.
	 */
	anybuss_set_power(client, false);
	ret = anybuss_set_power(client, true);
	if (ret)
		goto err_init;
	ret = anybuss_start_init(client, &mem_cfg);
	if (ret)
		goto err_init;
	if (cfg)
		ret = profinet_configure(client, cfg);
	if (ret)
		goto err_init;
	ret = anybuss_finish_init(client);
	if (ret)
		goto err_init;
	mutex_unlock(&priv->enable_lock);
	return 0;
err_init:
	anybuss_set_power(client, false);
	mutex_unlock(&priv->enable_lock);
	return ret;
}

static int profinet_disable(struct profi_priv *priv)
{
	int ret;

	if (mutex_lock_interruptible(&priv->enable_lock))
		return -ERESTARTSYS;
	ret = anybuss_set_power(priv->client, false);
	mutex_unlock(&priv->enable_lock);
	return ret;
}

static int fbctrl_readw(struct anybuss_client *client, u16 addr)
{
	int ret;
	u16 val;

	ret = anybuss_read_fbctrl(client, addr, &val, sizeof(val));
	if (ret < 0)
		return ret;
	return (int)be16_to_cpu(val);
}

static ssize_t mac_addr_show(struct device *dev,
				struct device_attribute *attr,
				char *buf)
{
	struct profi_priv *priv = dev_get_drvdata(dev);
	struct msgMacAddr response;
	int ret;

	ret = anybuss_recv_msg(priv->client, 0x0010, &response,
						sizeof(response));
	if (ret)
		return ret;
	return snprintf(buf, PAGE_SIZE, "%02X:%02X:%02X:%02X:%02X:%02X\n",
		response.addr[0], response.addr[1],
		response.addr[2], response.addr[3],
		response.addr[4], response.addr[5]);
}

static DEVICE_ATTR_RO(mac_addr);

static ssize_t start_defaults_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct profi_priv *priv = dev_get_drvdata(dev);
	unsigned long num;

	if (kstrtoul(buf, 0, &num))
		return -EINVAL;
	if (num)
		profinet_enable(priv, NULL);
	return count;
}

static DEVICE_ATTR_WO(start_defaults);

static ssize_t ip_addr_show(struct device *dev,
				struct device_attribute *attr,
				char *buf)
{
	struct profi_priv *priv = dev_get_drvdata(dev);
	struct msgEthConfig response;
	int ret;

	ret = anybuss_recv_msg(priv->client, 0x0002, &response,
						sizeof(response));
	if (ret)
		return ret;
	return snprintf(buf, PAGE_SIZE, "%d.%d.%d.%d\n",
		response.ip_addr & 0xFF,
		(response.ip_addr >>  8) & 0xFF,
		(response.ip_addr >> 16) & 0xFF,
		(response.ip_addr >> 24) & 0xFF);
}

static DEVICE_ATTR_RO(ip_addr);

static ssize_t subnet_mask_show(struct device *dev,
				struct device_attribute *attr,
				char *buf)
{
	struct profi_priv *priv = dev_get_drvdata(dev);
	struct msgEthConfig response;
	int ret;

	ret = anybuss_recv_msg(priv->client, 0x0002, &response,
						sizeof(response));
	if (ret)
		return ret;
	return snprintf(buf, PAGE_SIZE, "%d.%d.%d.%d\n",
		response.subnet_msk & 0xFF,
		(response.subnet_msk >>  8) & 0xFF,
		(response.subnet_msk >> 16) & 0xFF,
		(response.subnet_msk >> 24) & 0xFF);
}

static DEVICE_ATTR_RO(subnet_mask);

static ssize_t gateway_addr_show(struct device *dev,
				struct device_attribute *attr,
				char *buf)
{
	struct profi_priv *priv = dev_get_drvdata(dev);
	struct msgEthConfig response;
	int ret;

	ret = anybuss_recv_msg(priv->client, 0x0002, &response,
						sizeof(response));
	if (ret)
		return ret;
	return snprintf(buf, PAGE_SIZE, "%d.%d.%d.%d\n",
		response.gateway_addr & 0xFF,
		(response.gateway_addr >>  8) & 0xFF,
		(response.gateway_addr >> 16) & 0xFF,
		(response.gateway_addr >> 24) & 0xFF);
}

static DEVICE_ATTR_RO(gateway_addr);

static ssize_t hostname_show(struct device *dev,
				struct device_attribute *attr,
				char *buf)
{
	struct profi_priv *priv = dev_get_drvdata(dev);
	struct msgStr response;
	int ret;

	ret = anybuss_recv_msg(priv->client, 0x0034, &response,
						sizeof(response));
	if (ret)
		return ret;
	return snprintf(buf, PAGE_SIZE, "%s\n", response.s);
}

static DEVICE_ATTR_RO(hostname);

static ssize_t domainname_show(struct device *dev,
				struct device_attribute *attr,
				char *buf)
{
	struct profi_priv *priv = dev_get_drvdata(dev);
	struct msgStr response;
	int ret, pos;

	ret = anybuss_recv_msg(priv->client, 0x0034, &response,
						sizeof(response));
	if (ret)
		return ret;
	/*
	 * domain name string located right behind null-terminated
	 * host name string.
	 */
	pos = strnlen(response.s, sizeof(response.s)) + 1;
	if (pos >= sizeof(response.s))
		return -ENAMETOOLONG;
	return snprintf(buf, PAGE_SIZE, "%s\n", response.s + pos);
}

static DEVICE_ATTR_RO(domainname);

static ssize_t network_link_on_show(struct device *dev,
				struct device_attribute *attr,
				char *buf)
{
	struct profi_priv *priv = dev_get_drvdata(dev);
	int ns;

	ns = fbctrl_readw(priv->client, FSA_NETWORK_STATUS);
	if (ns < 0)
		return ns;
	return snprintf(buf, PAGE_SIZE, "%d\n", ns & 1);
}

static DEVICE_ATTR_RO(network_link_on);

static ssize_t network_ip_in_use_show(struct device *dev,
				struct device_attribute *attr,
				char *buf)
{
	struct profi_priv *priv = dev_get_drvdata(dev);
	int ns;

	ns = fbctrl_readw(priv->client, FSA_NETWORK_STATUS);
	if (ns < 0)
		return ns;
	return snprintf(buf, PAGE_SIZE, "%d\n", (ns>>1) & 1);
}

static DEVICE_ATTR_RO(network_ip_in_use);

static ssize_t layer_status_show(struct device *dev,
				struct device_attribute *attr,
				char *buf)
{
	struct profi_priv *priv = dev_get_drvdata(dev);
	const char *s;
	int ls;

	ls = fbctrl_readw(priv->client, FSA_LAYER_STATUS);
	if (ls < 0)
		return ls;
	switch (ls) {
	case 0x0000:
		s = "not yet initialized";
		break;
	case 0x0001:
		s = "successfully initialized";
		break;
	case 0x0002:
		s = "failed to initialize";
		break;
	default:
		return -EINVAL;
	}
	return snprintf(buf, PAGE_SIZE, "%s\n", s);
}

static DEVICE_ATTR_RO(layer_status);

static ssize_t io_controller_status_show(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	struct profi_priv *priv = dev_get_drvdata(dev);
	const char *s;
	int w;

	w = fbctrl_readw(priv->client, FSA_IO_CTRL_STATUS);
	if (w < 0)
		return w;
	switch (w) {
	case 0x0000:
		s = "No connection made";
		break;
	case 0x0001:
		s = "STOP";
		break;
	case 0x0002:
		s = "RUN";
		break;
	case 0x0004:
		s = "STATION OK";
		break;
	case 0x0008:
		s = "STATION PROBLEM";
		break;
	case 0x0010:
		s = "PRIMARY";
		break;
	case 0x0020:
		s = "BACKUP";
		break;
	default:
		return -EINVAL;
	}
	return snprintf(buf, PAGE_SIZE, "%s\n", s);
}

static DEVICE_ATTR_RO(io_controller_status);

static ssize_t layer_fault_code_show(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	int fc;
	struct profi_priv *priv = dev_get_drvdata(dev);

	fc = fbctrl_readw(priv->client, FSA_LAYER_FAULT_CODE);
	if (fc < 0)
		return fc;
	return snprintf(buf, PAGE_SIZE, "%d\n", fc);
}

static DEVICE_ATTR_RO(layer_fault_code);

static struct attribute *ctrl_attrs[] = {
	&dev_attr_mac_addr.attr,
	&dev_attr_start_defaults.attr,
	&dev_attr_ip_addr.attr,
	&dev_attr_subnet_mask.attr,
	&dev_attr_gateway_addr.attr,
	&dev_attr_hostname.attr,
	&dev_attr_domainname.attr,
	&dev_attr_network_link_on.attr,
	&dev_attr_network_ip_in_use.attr,
	&dev_attr_io_controller_status.attr,
	&dev_attr_layer_status.attr,
	&dev_attr_layer_fault_code.attr,
	NULL
};

static struct attribute_group ctrl_group = { .attrs = ctrl_attrs };

struct profi_open_file {
	struct profi_priv *priv;
	int event;
};

static int profi_open(struct inode *node, struct file *filp)
{
	struct profi_open_file *of;
	struct profi_priv *priv = container_of(filp->private_data,
					struct profi_priv, misc);

	of = kzalloc(sizeof(*of), GFP_KERNEL);
	if (!of)
		return -ENOMEM;
	of->priv = priv;
	filp->private_data = of;
	atomic_inc(&priv->refcount);
	return 0;
}

static int profi_release(struct inode *node, struct file *filp)
{
	struct profi_open_file *of = filp->private_data;
	struct profi_priv *priv = of->priv;

	kfree(of);
	if (!atomic_dec_and_test(&priv->refcount))
		return 0;
	return profinet_disable(priv);
}

static long profi_ioctl(struct file *filp, unsigned int cmd,
						unsigned long arg)
{
	struct profi_open_file *of = filp->private_data;
	struct profi_priv *priv = of->priv;
	void __user *argp = (void __user *)arg;
	struct ProfinetConfig config;

	if (_IOC_TYPE(cmd) != PROFINET_IOC_MAGIC)
		return -EINVAL;
	if (!(_IOC_DIR(cmd) & _IOC_WRITE))
		return -EINVAL;
	switch (cmd) {
	case PROFINET_IOCSETCONFIG:
		if (copy_from_user(&config, argp, sizeof(config)))
			return -EFAULT;
		return profinet_enable(priv, &config);
	default:
		break;
	}
	return -ENOTTY;
}

static ssize_t
profi_read(struct file *filp, char __user *buf, size_t size,
							loff_t *offset)
{
	struct profi_open_file *of = filp->private_data;
	struct profi_priv *priv = of->priv;

	return anybuss_read_output(priv->client, &of->event, buf, size,
							offset);
}

static ssize_t
profi_write(struct file *filp, const char __user *buf, size_t size,
							loff_t *offset)
{
	struct profi_open_file *of = filp->private_data;
	struct profi_priv *priv = of->priv;

	return anybuss_write_input(priv->client, buf, size, offset);
}

static unsigned int profi_poll(struct file *filp, poll_table *wait)
{
	struct profi_open_file *of = filp->private_data;
	struct profi_priv *priv = of->priv;

	return anybuss_poll(priv->client, of->event, filp, wait);
}

static const struct file_operations fops = {
	.open = profi_open,
	.release = profi_release,
	.read = profi_read,
	.write = profi_write,
	.unlocked_ioctl = profi_ioctl,
	.poll = profi_poll,
	.llseek = generic_file_llseek,
	.owner = THIS_MODULE,
};

static DEFINE_IDA(profi_index_ida);

static int profinet_probe(struct anybuss_client *client)
{
	struct profi_priv *priv;
	struct device *dev = &client->dev;
	int err;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;
	atomic_set(&priv->refcount, 0);
	mutex_init(&priv->enable_lock);
	priv->client = client;
	priv->misc.minor = MISC_DYNAMIC_MINOR;
	priv->id = ida_simple_get(&profi_index_ida, 0, 0, GFP_KERNEL);
	if (priv->id < 0)
		return priv->id;
	snprintf(priv->node_name, sizeof(priv->node_name), "profinet%d",
				priv->id);
	priv->misc.name = priv->node_name;
	priv->misc.fops = &fops;
	priv->misc.parent = client->dev.parent;
	err = misc_register(&priv->misc);
	if (err < 0) {
		dev_err(dev, "could not register device (%d)", err);
		goto err_ida;
	}
	priv->dev = priv->misc.this_device;
	dev_set_drvdata(priv->dev, priv);
	err = sysfs_create_group(&priv->dev->kobj, &ctrl_group);
	if (err < 0) {
		dev_err(dev, "could not create sysfs group (%d)", err);
		goto err_register;
	}
	dev_info(priv->dev, "detected on %s", dev_name(&client->dev));
	anybuss_set_drvdata(client, priv);
	return 0;
err_register:
	misc_deregister(&priv->misc);
err_ida:
	ida_simple_remove(&profi_index_ida, priv->id);
	return err;
}

static int profinet_remove(struct anybuss_client *client)
{
	struct profi_priv *priv = anybuss_get_drvdata(client);

	sysfs_remove_group(&priv->dev->kobj, &ctrl_group);
	misc_deregister(&priv->misc);
	ida_simple_remove(&profi_index_ida, priv->id);
	return 0;
}

static struct anybuss_client_driver profinet_driver = {
	.probe = profinet_probe,
	.remove = profinet_remove,
	.driver		= {
		.name   = "hms-profinet",
		.owner	= THIS_MODULE,
	},
	.fieldbus_type = 0x0089,
};

static int __init profinet_init(void)
{
	return anybuss_client_driver_register(&profinet_driver);
}
module_init(profinet_init);

static void __exit profinet_exit(void)
{
	return anybuss_client_driver_unregister(&profinet_driver);
}
module_exit(profinet_exit);

MODULE_AUTHOR("Sven Van Asbroeck <svendev@arcx.com>");
MODULE_DESCRIPTION("HMS Profinet IRT Driver (Anybus-S)");
MODULE_LICENSE("GPL v2");
