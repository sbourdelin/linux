/*
 *  Common functions for kernel modules using Dell SMBIOS
 *
 *  Copyright (c) Red Hat <mjg@redhat.com>
 *  Copyright (c) 2014 Gabriele Mazzotta <gabriele.mzt@gmail.com>
 *  Copyright (c) 2014 Pali Rohár <pali.rohar@gmail.com>
 *  Copyright (c) 2017 Dell Inc.
 *
 *  Based on documentation in the libsmbios package:
 *  Copyright (C) 2005-2014 Dell Inc.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/dmi.h>
#include <linux/err.h>
#include <linux/mutex.h>
#include <linux/wmi.h>
#include <linux/uaccess.h>
#include "dell-wmi-smbios.h"

#ifdef CONFIG_DCDBAS
#include "../../firmware/dcdbas.h"
#endif

#define DELL_WMI_SMBIOS_GUID "A80593CE-A997-11DA-B012-B622A1EF5492"
#define DELL_DESCRIPTOR_GUID "8D9DDCBC-A997-11DA-B012-B622A1EF5492"

struct calling_interface_structure {
	struct dmi_header header;
	u16 cmdIOAddress;
	u8 cmdIOCode;
	u32 supportedCmds;
	struct calling_interface_token tokens[];
} __packed;

static struct calling_interface_buffer *smi_buffer;
static struct wmi_calling_interface_buffer *internal_wmi_buffer;
static struct wmi_calling_interface_buffer *sysfs_wmi_buffer;
static DEFINE_MUTEX(buffer_mutex);

static int da_command_address;
static int da_command_code;
static int da_num_tokens;
static int has_wmi;
static struct calling_interface_token *da_tokens;

int dell_smbios_error(int value)
{
	switch (value) {
	case 0: /* Completed successfully */
		return 0;
	case -1: /* Completed with error */
		return -EIO;
	case -2: /* Function not supported */
		return -ENXIO;
	default: /* Unknown error */
		return -EINVAL;
	}
}
EXPORT_SYMBOL_GPL(dell_smbios_error);

struct calling_interface_buffer *dell_smbios_get_buffer(void)
{
	mutex_lock(&buffer_mutex);
	dell_smbios_clear_buffer();
	if (has_wmi)
		return &internal_wmi_buffer->smi;
	return smi_buffer;
}
EXPORT_SYMBOL_GPL(dell_smbios_get_buffer);

void dell_smbios_clear_buffer(void)
{
	if (has_wmi)
		memset(internal_wmi_buffer, 0,
		       sizeof(struct wmi_calling_interface_buffer));
	else
		memset(smi_buffer, 0,
		       sizeof(struct calling_interface_buffer));
}
EXPORT_SYMBOL_GPL(dell_smbios_clear_buffer);

void dell_smbios_release_buffer(void)
{
	mutex_unlock(&buffer_mutex);
}
EXPORT_SYMBOL_GPL(dell_smbios_release_buffer);

int run_wmi_smbios_call(struct wmi_calling_interface_buffer *buf)
{
	struct acpi_buffer output = {ACPI_ALLOCATE_BUFFER, NULL};
	struct acpi_buffer input;
	union acpi_object *obj;
	acpi_status status;

	input.length = sizeof(struct wmi_calling_interface_buffer);
	input.pointer = buf;

	status = wmi_evaluate_method(DELL_WMI_SMBIOS_GUID,
				     0, 1, &input, &output);
	if (ACPI_FAILURE(status)) {
		pr_err("%x/%x [%x,%x,%x,%x] call failed\n",
			buf->smi.class, buf->smi.select,
			buf->smi.input[0], buf->smi.input[1],
			buf->smi.input[2], buf->smi.input[3]);
			return -EIO;
	}
	obj = (union acpi_object *)output.pointer;
	if (obj->type != ACPI_TYPE_BUFFER) {
		pr_err("invalid type : %d\n", obj->type);
		return -EIO;
	}
	memcpy(buf, obj->buffer.pointer, input.length);

	return 0;
}

void dell_smbios_send_request(int class, int select)
{
	if (has_wmi) {
		internal_wmi_buffer->smi.class = class;
		internal_wmi_buffer->smi.select = select;
		run_wmi_smbios_call(internal_wmi_buffer);
	}

#ifdef CONFIG_DCDBAS
	else {
		if (!smi_buffer)
			return;
		struct smi_cmd command;

		smi_buffer->class = class;
		smi_buffer->select = select;
		command.magic = SMI_CMD_MAGIC;
		command.command_address = da_command_address;
		command.command_code = da_command_code;
		command.ebx = virt_to_phys(smi_buffer);
		command.ecx = 0x42534931;

		dcdbas_smi_request(&command);
	}
#endif
}
EXPORT_SYMBOL_GPL(dell_smbios_send_request);

struct calling_interface_token *dell_smbios_find_token(int tokenid)
{
	int i;

	for (i = 0; i < da_num_tokens; i++) {
		if (da_tokens[i].tokenID == tokenid)
			return &da_tokens[i];
	}

	return NULL;
}
EXPORT_SYMBOL_GPL(dell_smbios_find_token);

static BLOCKING_NOTIFIER_HEAD(dell_laptop_chain_head);

int dell_laptop_register_notifier(struct notifier_block *nb)
{
	return blocking_notifier_chain_register(&dell_laptop_chain_head, nb);
}
EXPORT_SYMBOL_GPL(dell_laptop_register_notifier);

int dell_laptop_unregister_notifier(struct notifier_block *nb)
{
	return blocking_notifier_chain_unregister(&dell_laptop_chain_head, nb);
}
EXPORT_SYMBOL_GPL(dell_laptop_unregister_notifier);

void dell_laptop_call_notifier(unsigned long action, void *data)
{
	blocking_notifier_call_chain(&dell_laptop_chain_head, action, data);
}
EXPORT_SYMBOL_GPL(dell_laptop_call_notifier);

static void __init parse_da_table(const struct dmi_header *dm)
{
	/* Final token is a terminator, so we don't want to copy it */
	int tokens = (dm->length-11)/sizeof(struct calling_interface_token)-1;
	struct calling_interface_token *new_da_tokens;
	struct calling_interface_structure *table =
		container_of(dm, struct calling_interface_structure, header);

	/* 4 bytes of table header, plus 7 bytes of Dell header, plus at least
	   6 bytes of entry */

	if (dm->length < 17)
		return;

	da_command_address = table->cmdIOAddress;
	da_command_code = table->cmdIOCode;

	new_da_tokens = krealloc(da_tokens, (da_num_tokens + tokens) *
				 sizeof(struct calling_interface_token),
				 GFP_KERNEL);

	if (!new_da_tokens)
		return;
	da_tokens = new_da_tokens;

	memcpy(da_tokens+da_num_tokens, table->tokens,
	       sizeof(struct calling_interface_token) * tokens);

	da_num_tokens += tokens;
}

static void __init find_tokens(const struct dmi_header *dm, void *dummy)
{
	switch (dm->type) {
	case 0xd4: /* Indexed IO */
	case 0xd5: /* Protected Area Type 1 */
	case 0xd6: /* Protected Area Type 2 */
		break;
	case 0xda: /* Calling interface */
		parse_da_table(dm);
		break;
	}
}

static int dell_wmi_smbios_open(struct inode *inode, struct file *file)
{
	return nonseekable_open(inode, file);
}

static int dell_wmi_smbios_release(struct inode *inode, struct file *file)
{
	return 0;
}

static long dell_wmi_smbios_ioctl(struct file *filp, unsigned int cmd,
	unsigned long arg)
{
	struct token_ioctl_buffer *tokens_buffer;
	void __user *p = (void __user *) arg;
	size_t size;
	int ret = 0;

	if (_IOC_TYPE(cmd) != DELL_WMI_SMBIOS_IOC)
		return -ENOTTY;

	switch (cmd) {
	case DELL_WMI_SMBIOS_CALL_CMD:
		size = sizeof(struct wmi_calling_interface_buffer);
		mutex_lock(&buffer_mutex);
		if (copy_from_user(sysfs_wmi_buffer, p, size)) {
			ret = -EFAULT;
			goto fail_smbios_cmd;
		}
		ret = run_wmi_smbios_call(sysfs_wmi_buffer);
		if (ret != 0)
			goto fail_smbios_cmd;
		if (copy_to_user(p, sysfs_wmi_buffer, size))
			ret = -EFAULT;
fail_smbios_cmd:
		mutex_unlock(&buffer_mutex);
		break;
	case DELL_WMI_SMBIOS_GET_NUM_TOKENS_CMD:
		if (copy_to_user(p, &da_num_tokens, sizeof(u32)))
			ret = -EFAULT;
		break;
	case DELL_WMI_SMBIOS_GET_TOKENS_CMD:
		tokens_buffer = kmalloc(sizeof(struct token_ioctl_buffer),
					GFP_KERNEL);
		size = sizeof(struct token_ioctl_buffer);
		if (copy_from_user(tokens_buffer, p, size)) {
			ret = -EFAULT;
			goto fail_get_tokens_cmd;
		}
		if (tokens_buffer->num_tokens < da_num_tokens) {
			ret = -EOVERFLOW;
			goto fail_get_tokens_cmd;
		}
		size = sizeof(struct calling_interface_token) * da_num_tokens;
		if (copy_to_user(tokens_buffer->tokens, da_tokens, size)) {
			ret = -EFAULT;
			goto fail_get_tokens_cmd;
		}
fail_get_tokens_cmd:
		kfree(tokens_buffer);
		break;
	default:
		pr_err("unsupported ioctl: %d.\n", cmd);
		ret = -ENOIOCTLCMD;
	}
	return ret;
}

/*
 * Descriptor buffer is 128 byte long and contains:
 *
 *       Name             Offset  Length  Value
 * Vendor Signature          0       4    "DELL"
 * Object Signature          4       4    " WMI"
 * WMI Interface Version     8       4    <version>
 * WMI buffer length        12       4    4096
 */
int dell_wmi_check_descriptor_buffer(struct wmi_device *wdev, u32 *version)
{
	union acpi_object *obj = NULL;
	struct wmi_device *desc_dev;
	u32 *desc_buffer;
	int ret;

	desc_dev = wmidev_get_other_guid(wdev, DELL_DESCRIPTOR_GUID);
	if (!desc_dev) {
		dev_err(&wdev->dev, "Dell WMI descriptor does not exist\n");
		return -ENODEV;
	}

	obj = wmidev_block_query(desc_dev, 0);
	if (!obj) {
		dev_err(&wdev->dev, "failed to read Dell WMI descriptor\n");
		ret = -EIO;
		goto out;
	}

	if (obj->type != ACPI_TYPE_BUFFER) {
		dev_err(&wdev->dev, "Dell descriptor has wrong type\n");
		ret = -EINVAL;
		goto out;
	}

	if (obj->buffer.length != 128) {
		dev_err(&wdev->dev,
			"Dell descriptor buffer has invalid length (%d)\n",
			obj->buffer.length);
		if (obj->buffer.length < 16) {
			ret = -EINVAL;
			goto out;
		}
	}
	desc_buffer = (u32 *)obj->buffer.pointer;

	if (strncmp(obj->string.pointer, "DELL WMI", 8) != 0)
		dev_warn(&wdev->dev, "Dell descriptor buffer has invalid signature (%8ph)\n",
			desc_buffer);

	if (desc_buffer[2] != 0 && desc_buffer[2] != 1)
		dev_warn(&wdev->dev, "Dell descriptor buffer has unknown version (%u)\n",
			desc_buffer[2]);

	if (desc_buffer[3] != 4096)
		dev_warn(&wdev->dev, "Dell descriptor buffer has invalid buffer length (%u)\n",
			desc_buffer[3]);

	*version = desc_buffer[2];
	ret = 0;

	dev_info(&wdev->dev, "Detected Dell WMI interface version %u\n",
		*version);

out:
	kfree(obj);
	put_device(&desc_dev->dev);
	return ret;
}
EXPORT_SYMBOL_GPL(dell_wmi_check_descriptor_buffer);


static int dell_smbios_wmi_probe(struct wmi_device *wdev)
{
	int ret;
	u32 interface_version;

	/* WMI buffers should be 32k */
	internal_wmi_buffer = (void *)__get_free_pages(GFP_KERNEL, 3);
	if (!internal_wmi_buffer)
		return -ENOMEM;

	sysfs_wmi_buffer = (void *)__get_free_pages(GFP_KERNEL, 3);
	if (!sysfs_wmi_buffer) {
		ret = -ENOMEM;
		goto fail_sysfs_wmi_buffer;
	}

	ret = dell_wmi_check_descriptor_buffer(wdev, &interface_version);
	if (ret)
		goto fail_wmi_probe;

#ifdef CONFIG_DCDBAS
	/* no longer need the SMI page */
	free_page((unsigned long)smi_buffer);
	smi_buffer = NULL;
#endif

	has_wmi = 1;
	return 0;

fail_wmi_probe:
	free_pages((unsigned long)sysfs_wmi_buffer, 3);

fail_sysfs_wmi_buffer:
	free_pages((unsigned long)internal_wmi_buffer, 3);
	return ret;
}

static int dell_smbios_wmi_remove(struct wmi_device *wdev)
{
	free_pages((unsigned long)internal_wmi_buffer, 3);
	free_pages((unsigned long)sysfs_wmi_buffer, 3);
	return 0;
}

static const struct wmi_device_id dell_smbios_wmi_id_table[] = {
	{ .guid_string = DELL_WMI_SMBIOS_GUID },
	{ },
};

static const struct file_operations dell_wmi_smbios_fops = {
	.owner		= THIS_MODULE,
	.unlocked_ioctl	= dell_wmi_smbios_ioctl,
	.open		= dell_wmi_smbios_open,
	.release	= dell_wmi_smbios_release,
};

static struct wmi_driver dell_wmi_smbios_driver = {
	.driver = {
		.name = "dell-wmi-smbios",
	},
	.probe = dell_smbios_wmi_probe,
	.remove = dell_smbios_wmi_remove,
	.id_table = dell_smbios_wmi_id_table,
	.file_operations = &dell_wmi_smbios_fops,
};

static int __init dell_wmi_smbios_init(void)
{
	dmi_walk(find_tokens, NULL);

	if (!da_tokens)  {
		pr_info("Unable to find dmi tokens\n");
		return -ENODEV;
	}

#ifdef CONFIG_DCDBAS
	/*
	 * Allocate buffer below 4GB for SMI data--only 32-bit physical addr
	 * is passed to SMI handler.
	 */
	smi_buffer = (void *)__get_free_page(GFP_KERNEL | GFP_DMA32);
#else
	smi_buffer = NULL;
#endif
	wmi_driver_register(&dell_wmi_smbios_driver);

	if (!smi_buffer && !has_wmi) {
		kfree(da_tokens);
		return -ENOMEM;
	}
	return 0;
}

static void __exit dell_wmi_smbios_exit(void)
{
	kfree(da_tokens);
#ifdef CONFIG_DCDBAS
	if (!has_wmi)
		free_page((unsigned long)smi_buffer);
#endif
	wmi_driver_unregister(&dell_wmi_smbios_driver);
}

subsys_initcall(dell_wmi_smbios_init);
module_exit(dell_wmi_smbios_exit);


MODULE_AUTHOR("Matthew Garrett <mjg@redhat.com>");
MODULE_AUTHOR("Gabriele Mazzotta <gabriele.mzt@gmail.com>");
MODULE_AUTHOR("Pali Rohár <pali.rohar@gmail.com>");
MODULE_AUTHOR("Mario Limonciello <mario.limonciello@dell.com>");
MODULE_DESCRIPTION("Common functions for kernel modules using Dell SMBIOS");
MODULE_LICENSE("GPL");
