/*
 * Copyright (c) 2017 NVIDIA Corporation
 *
 * This file is released under the GPL v2
 */

#define pr_fmt(fmt) "system-power: " fmt

#include <linux/system-power.h>

static DEFINE_MUTEX(system_power_lock);
static LIST_HEAD(system_power_chips);

int system_power_chip_add(struct system_power_chip *chip)
{
	if (!chip->ops || (!chip->ops->restart && !chip->ops->power_off)) {
		WARN(1, pr_fmt("must implement restart or power off\n"));
		return -EINVAL;
	}

	mutex_lock(&system_power_lock);

	INIT_LIST_HEAD(&chip->list);
	list_add_tail(&chip->list, &system_power_chips);

	mutex_unlock(&system_power_lock);

	return 0;
}
EXPORT_SYMBOL_GPL(system_power_chip_add);

int system_power_chip_remove(struct system_power_chip *chip)
{
	mutex_lock(&system_power_lock);

	list_del_init(&chip->list);

	mutex_unlock(&system_power_lock);

	return 0;
}
EXPORT_SYMBOL_GPL(system_power_chip_remove);

bool system_can_power_off(void)
{
	/* XXX for backwards compatibility */
	return pm_power_off != NULL;
}

int system_restart(char *cmd)
{
	struct system_power_chip *chip;
	int err;

	mutex_lock(&system_power_lock);

	list_for_each_entry(chip, &system_power_chips, list) {
		if (!chip->ops->restart)
			continue;

		pr_debug("trying to restart using %ps\n", chip);

		err = chip->ops->restart(chip, reboot_mode, cmd);
		if (err < 0)
			dev_warn(chip->dev, "failed to restart: %d\n", err);
	}

	mutex_unlock(&system_power_lock);

	/* XXX for backwards compatibility */
	do_kernel_restart(cmd);

	return 0;
}

int system_power_off_prepare(void)
{
	/* XXX for backwards compatibility */
	if (pm_power_off_prepare)
		pm_power_off_prepare();

	return 0;
}

int system_power_off(void)
{
	struct system_power_chip *chip;
	int err;

	mutex_lock(&system_power_lock);

	list_for_each_entry(chip, &system_power_chips, list) {
		if (!chip->ops->power_off)
			continue;

		pr_debug("trying to power off using %ps\n", chip);

		err = chip->ops->power_off(chip);
		if (err < 0)
			dev_warn(chip->dev, "failed to power off: %d\n", err);
	}

	mutex_unlock(&system_power_lock);

	/* XXX for backwards compatibility */
	if (pm_power_off)
		pm_power_off();

	return 0;
}
