/*
 * ACPI configfs support
 *
 * Copyright (c) 2015 Intel Corporation
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/configfs.h>
#include <linux/acpi.h>

static struct config_group *acpi_table_group;

struct acpi_user_table {
	struct config_item cfg;
	struct acpi_table_header *table;
};

static ssize_t acpi_table_data_write(struct config_item *cfg,
				     const void *data, size_t size)
{
	struct acpi_table_header *header = (struct acpi_table_header *)data;
	struct acpi_user_table *table;
	int ret;

	table = container_of(cfg, struct acpi_user_table, cfg);

	if (table->table) {
		pr_err("ACPI configfs table: table already loaded\n");
		return -EBUSY;
	}

	if (header->length != size) {
		pr_err("ACPI configfs table: invalid table length\n");
		return -EINVAL;
	}

	if (memcmp(header->signature, ACPI_SIG_SSDT, 4)) {
		pr_err("ACPI configfs table: invalid table signature\n");
		return -EINVAL;
	}

	table = container_of(cfg, struct acpi_user_table, cfg);

	table->table = kmemdup(header, header->length, GFP_KERNEL);
	if (!table->table)
		return -ENOMEM;

	ret = acpi_load_table(table->table);
	if (ret) {
		kfree(table->table);
		table->table = NULL;
	}

	add_taint(TAINT_OVERLAY_ACPI_TABLE, LOCKDEP_STILL_OK);

	return ret;
}

#define MAX_ACPI_TABLE_SIZE (128 * 1024)

CONFIGFS_BIN_ATTR_WO(acpi_table_, data, NULL, MAX_ACPI_TABLE_SIZE);

struct configfs_bin_attribute *acpi_table_bin_attrs[] = {
	&acpi_table_attr_data,
	NULL,
};

static struct config_item_type acpi_table_type = {
	.ct_owner = THIS_MODULE,
	.ct_bin_attrs = acpi_table_bin_attrs,
};

static struct config_item *acpi_table_make_item(struct config_group *group,
						const char *name)
{
	struct acpi_user_table *table;

	table = kzalloc(sizeof(*table), GFP_KERNEL);
	if (!table)
		return ERR_PTR(-ENOMEM);

	config_item_init_type_name(&table->cfg, name, &acpi_table_type);
	return &table->cfg;
}

struct configfs_group_operations acpi_table_group_ops = {
	.make_item = acpi_table_make_item,
};

static struct config_item_type acpi_tables_type = {
	.ct_owner = THIS_MODULE,
	.ct_group_ops = &acpi_table_group_ops,
};

static struct config_item_type acpi_root_group_type = {
	.ct_owner	= THIS_MODULE,
};

static struct configfs_subsystem acpi_configfs = {
	.su_group = {
		.cg_item = {
			.ci_namebuf = "acpi",
			.ci_type = &acpi_root_group_type,
		},
	},
	.su_mutex = __MUTEX_INITIALIZER(acpi_configfs.su_mutex),
};

static int __init acpi_configfs_init(void)
{
	int ret;
	struct config_group *root = &acpi_configfs.su_group;

	config_group_init(root);

	ret = configfs_register_subsystem(&acpi_configfs);
	if (ret)
		return ret;

	acpi_table_group = configfs_register_default_group(root, "table",
							   &acpi_tables_type);
	return PTR_ERR_OR_ZERO(acpi_table_group);
}
module_init(acpi_configfs_init);

static void __exit acpi_configfs_exit(void)
{
	configfs_unregister_default_group(acpi_table_group);
	configfs_unregister_subsystem(&acpi_configfs);
}
module_exit(acpi_configfs_exit);

MODULE_AUTHOR("Octavian Purdila <octavian.purdila@intel.com>");
MODULE_DESCRIPTION("ACPI configfs support");
MODULE_LICENSE("GPL v2");
