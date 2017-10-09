/*
 *  SMI methods for use with dell-smbios
 *
 *  Copyright (c) Red Hat <mjg@redhat.com>
 *  Copyright (c) 2014 Gabriele Mazzotta <gabriele.mzt@gmail.com>
 *  Copyright (c) 2014 Pali Rohár <pali.rohar@gmail.com>
 *  Copyright (c) 2017 Dell Inc.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/gfp.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include "../../firmware/dcdbas.h"
#include "dell-smbios.h"

static int da_command_address;
static int da_command_code;
static struct calling_interface_buffer *buffer;
struct platform_device *platform_device;
static DEFINE_MUTEX(smm_mutex);

#define WSMT_EN_TOKEN	0x04EC

static const struct dmi_system_id dell_device_table[] __initconst = {
	{
		.ident = "Dell laptop",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Dell Inc."),
			DMI_MATCH(DMI_CHASSIS_TYPE, "8"),
		},
	},
	{
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Dell Inc."),
			DMI_MATCH(DMI_CHASSIS_TYPE, "9"), /*Laptop*/
		},
	},
	{
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Dell Inc."),
			DMI_MATCH(DMI_CHASSIS_TYPE, "10"), /*Notebook*/
		},
	},
	{
		.ident = "Dell Computer Corporation",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Dell Computer Corporation"),
			DMI_MATCH(DMI_CHASSIS_TYPE, "8"),
		},
	},
	{ }
};
MODULE_DEVICE_TABLE(dmi, dell_device_table);

int dell_smbios_smm_call(struct calling_interface_buffer *input)
{
	struct smi_cmd command;
	size_t size;

	size = sizeof(struct calling_interface_buffer);
	command.magic = SMI_CMD_MAGIC;
	command.command_address = da_command_address;
	command.command_code = da_command_code;
	command.ebx = virt_to_phys(buffer);
	command.ecx = 0x42534931;

	mutex_lock(&smm_mutex);
	memcpy(buffer, input, size);
	dcdbas_smi_request(&command);
	memcpy(input, buffer, size);
	mutex_unlock(&smm_mutex);
	return 0;
}

static int test_wsmt_enabled(void)
{
	struct calling_interface_token *token;

	/* if token doesn't exist, SMM will work */
	token = dell_smbios_find_token(WSMT_EN_TOKEN);
	if (!token)
		return 0;

	/* if token exists, try to access over SMM */
	buffer->class = 0;
	buffer->select = 0;
	memset(buffer, 0, sizeof(struct calling_interface_buffer));
	buffer->input[0] = token->location;
	dell_smbios_smm_call(buffer);

	/* if lookup failed, we know WSMT was enabled */
	if (buffer->output[0] != 0)
		return 1;

	/* query token status if it didn't fail */
	return (buffer->output[1] == token->value);
}

static int __init dell_smbios_smm_init(void)
{
	int ret;
	/*
	 * Allocate buffer below 4GB for SMI data--only 32-bit physical addr
	 * is passed to SMI handler.
	 */
	buffer = (void *)__get_free_page(GFP_KERNEL | GFP_DMA32);
	if (!buffer)
		return -ENOMEM;
	dell_smbios_get_smm_address(&da_command_address, &da_command_code);

	ret = test_wsmt_enabled();
	pr_debug("WSMT enable test: %d\n", ret);
	if (ret) {
		ret = -ENODEV;
		goto fail_wsmt;
	}

	platform_device = platform_device_alloc("dell-smbios", 1);
	if (!platform_device) {
		ret = -ENOMEM;
		goto fail_platform_device_alloc;
	}

	ret = platform_device_add(platform_device);
	if (ret)
		goto fail_platform_device_add;

	ret = dell_smbios_register_device(&platform_device->dev,
					  &dell_smbios_smm_call);
	if (ret)
		goto fail_register;

	return 0;

fail_register:
	platform_device_del(platform_device);

fail_platform_device_add:
	platform_device_put(platform_device);

fail_wsmt:
fail_platform_device_alloc:
	free_page((unsigned long)buffer);
	return ret;
}

static void __exit dell_smbios_smm_exit(void)
{
	if (platform_device) {
		dell_smbios_unregister_device(&platform_device->dev);
		platform_device_unregister(platform_device);
		free_page((unsigned long)buffer);
	}
}

subsys_initcall(dell_smbios_smm_init);
module_exit(dell_smbios_smm_exit);

MODULE_AUTHOR("Matthew Garrett <mjg@redhat.com>");
MODULE_AUTHOR("Gabriele Mazzotta <gabriele.mzt@gmail.com>");
MODULE_AUTHOR("Pali Rohár <pali.rohar@gmail.com>");
MODULE_AUTHOR("Mario Limonciello <mario.limonciello@dell.com>");
MODULE_DESCRIPTION("Dell SMBIOS communications over SMI");
MODULE_LICENSE("GPL");
