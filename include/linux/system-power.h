/*
 * Copyright (c) 2017 NVIDIA Corporation
 *
 * This file is released under the GPL v2
 */

#ifndef SYSTEM_POWER_H
#define SYSTEM_POWER_H

#include <linux/device.h>
#include <linux/list.h>
#include <linux/reboot.h>

struct system_power_chip;

struct system_power_ops {
	int (*restart)(struct system_power_chip *chip, enum reboot_mode mode,
		       char *cmd);
	int (*power_off_prepare)(struct system_power_chip *chip);
	int (*power_off)(struct system_power_chip *chip);
};

struct system_power_chip {
	const struct system_power_ops *ops;
	struct list_head list;
	struct device *dev;
};

int system_power_chip_add(struct system_power_chip *chip);
int system_power_chip_remove(struct system_power_chip *chip);

bool system_can_power_off(void);

int system_restart(char *cmd);
int system_power_off_prepare(void);
int system_power_off(void);

#endif
