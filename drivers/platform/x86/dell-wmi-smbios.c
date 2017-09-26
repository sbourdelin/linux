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
#include "dell-wmi-smbios.h"

#ifdef CONFIG_DCDBAS
#include "../../firmware/dcdbas.h"
#endif

#define DELL_WMI_SMBIOS_GUID "A80593CE-A997-11DA-B012-B622A1EF5492"

struct calling_interface_structure {
	struct dmi_header header;
	u16 cmdIOAddress;
	u8 cmdIOCode;
	u32 supportedCmds;
	struct calling_interface_token tokens[];
} __packed;

static struct calling_interface_buffer *smi_buffer;
static struct wmi_calling_interface_buffer *wmi_buffer;
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
		return &wmi_buffer->smi;
	return smi_buffer;
}
EXPORT_SYMBOL_GPL(dell_smbios_get_buffer);

void dell_smbios_clear_buffer(void)
{
	if (has_wmi)
		memset(wmi_buffer, 0,
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
		wmi_buffer->smi.class = class;
		wmi_buffer->smi.select = select;
		run_wmi_smbios_call(wmi_buffer);
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

static int dell_smbios_wmi_probe(struct wmi_device *wdev)
{
	/* WMI buffer should be 32k */
	wmi_buffer = (void *)__get_free_pages(GFP_KERNEL, 3);
	if (!wmi_buffer)
		return -ENOMEM;

#ifdef CONFIG_DCDBAS
	/* no longer need the SMI page */
	free_page((unsigned long)smi_buffer);
	smi_buffer = NULL;
#endif

	has_wmi = 1;
	return 0;
}

static int dell_smbios_wmi_remove(struct wmi_device *wdev)
{
	free_pages((unsigned long)wmi_buffer, 3);
	return 0;
}

static const struct wmi_device_id dell_smbios_wmi_id_table[] = {
	{ .guid_string = DELL_WMI_SMBIOS_GUID },
	{ },
};

static struct wmi_driver dell_wmi_smbios_driver = {
	.driver = {
		.name = "dell-wmi-smbios",
	},
	.probe = dell_smbios_wmi_probe,
	.remove = dell_smbios_wmi_remove,
	.id_table = dell_smbios_wmi_id_table,
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
