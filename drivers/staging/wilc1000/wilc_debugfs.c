// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2012 - 2018 Microchip Technology Inc., and its subsidiaries.
 * All rights reserved.
 */

#if defined(WILC_DEBUGFS)
#include <linux/module.h>
#include <linux/debugfs.h>

#include "wilc_wlan_if.h"

static struct dentry *wilc_dir;

#define DEBUG           BIT(0)
#define INFO            BIT(1)
#define WRN             BIT(2)
#define ERR             BIT(3)

#define DBG_LEVEL_ALL	(DEBUG | INFO | WRN | ERR)
static atomic_t WILC_DEBUG_LEVEL = ATOMIC_INIT(ERR);

ssize_t wilc_debug_level_read(struct file *file, char __user *userbuf,
			      size_t count, loff_t *ppos)
{
	char buf[128];
	int res = 0;

	/* only allow read from start */
	if (*ppos > 0)
		return 0;

	res = scnprintf(buf, sizeof(buf), "Debug Level: %x\n",
			atomic_read(&WILC_DEBUG_LEVEL));

	return simple_read_from_buffer(userbuf, count, ppos, buf, res);
}

ssize_t wilc_debug_level_write(struct file *filp, const char __user *buf,
			       size_t count, loff_t *ppos)
{
	int flag = 0;
	int ret;

	ret = kstrtouint_from_user(buf, count, 16, &flag);
	if (ret)
		return ret;

	if (flag > DBG_LEVEL_ALL) {
		pr_info("%s, value (0x%08x) is out of range, stay previous flag (0x%08x)\n",
			__func__, flag, atomic_read(&WILC_DEBUG_LEVEL));
		return -EINVAL;
	}

	atomic_set(&WILC_DEBUG_LEVEL, (int)flag);

	if (flag == 0)
		pr_info("Debug-level disabled\n");
	else
		pr_info("Debug-level enabled\n");

	return count;
}

int wilc_debugfs_init(const struct file_operations *fops)
{
	wilc_dir = debugfs_create_dir("wilc_wifi", NULL);
	debugfs_create_file("wilc_debug_level", 0666, wilc_dir, NULL, fops);

	return 0;
}

void wilc_debugfs_remove(void)
{
	debugfs_remove_recursive(wilc_dir);
}

#endif
