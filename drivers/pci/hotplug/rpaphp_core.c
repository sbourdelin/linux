// SPDX-License-Identifier: GPL-2.0+
/*
 * PCI Hot Plug Controller Driver for RPA-compliant PPC64 platform.
 * Copyright (C) 2003 Linda Xie <lxie@us.ibm.com>
 *
 * All rights reserved.
 *
 * Send feedback to <lxie@us.ibm.com>
 *
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/pci.h>
#include <linux/pci_hotplug.h>
#include <linux/smp.h>
#include <linux/init.h>
#include <linux/vmalloc.h>
#include <asm/firmware.h>
#include <asm/eeh.h>       /* for eeh_add_device() */
#include <asm/rtas.h>		/* rtas_call */
#include <asm/pci-bridge.h>	/* for pci_controller */
#include "../pci.h"		/* for pci_add_new_bus */
				/* and pci_do_scan_bus */
#include "rpaphp.h"

bool rpaphp_debug;
LIST_HEAD(rpaphp_slot_head);
EXPORT_SYMBOL_GPL(rpaphp_slot_head);

#define DRIVER_VERSION	"0.1"
#define DRIVER_AUTHOR	"Linda Xie <lxie@us.ibm.com>"
#define DRIVER_DESC	"RPA HOT Plug PCI Controller Driver"

#define MAX_LOC_CODE 128

MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_LICENSE("GPL");

module_param_named(debug, rpaphp_debug, bool, 0644);

/**
 * set_attention_status - set attention LED
 * @hotplug_slot: target &hotplug_slot
 * @value: LED control value
 *
 * echo 0 > attention -- set LED OFF
 * echo 1 > attention -- set LED ON
 * echo 2 > attention -- set LED ID(identify, light is blinking)
 */
static int set_attention_status(struct hotplug_slot *hotplug_slot, u8 value)
{
	int rc;
	struct slot *slot = to_slot(hotplug_slot);

	switch (value) {
	case 0:
	case 1:
	case 2:
		break;
	default:
		value = 1;
		break;
	}

	rc = rtas_set_indicator(DR_INDICATOR, slot->index, value);
	if (!rc)
		slot->attention_status = value;

	return rc;
}

/**
 * get_power_status - get power status of a slot
 * @hotplug_slot: slot to get status
 * @value: pointer to store status
 */
static int get_power_status(struct hotplug_slot *hotplug_slot, u8 *value)
{
	int retval, level;
	struct slot *slot = to_slot(hotplug_slot);

	retval = rtas_get_power_level(slot->power_domain, &level);
	if (!retval)
		*value = level;
	return retval;
}

/**
 * get_attention_status - get attention LED status
 * @hotplug_slot: slot to get status
 * @value: pointer to store status
 */
static int get_attention_status(struct hotplug_slot *hotplug_slot, u8 *value)
{
	struct slot *slot = to_slot(hotplug_slot);
	*value = slot->attention_status;
	return 0;
}

static int get_adapter_status(struct hotplug_slot *hotplug_slot, u8 *value)
{
	struct slot *slot = to_slot(hotplug_slot);
	int rc, state;

	rc = rpaphp_get_sensor_state(slot, &state);

	*value = NOT_VALID;
	if (rc)
		return rc;

	if (state == EMPTY)
		*value = EMPTY;
	else if (state == PRESENT)
		*value = slot->state;

	return 0;
}

static enum pci_bus_speed get_max_bus_speed(struct slot *slot)
{
	enum pci_bus_speed speed;
	switch (slot->type) {
	case 1:
	case 2:
	case 3:
	case 4:
	case 5:
	case 6:
		speed = PCI_SPEED_33MHz;	/* speed for case 1-6 */
		break;
	case 7:
	case 8:
		speed = PCI_SPEED_66MHz;
		break;
	case 11:
	case 14:
		speed = PCI_SPEED_66MHz_PCIX;
		break;
	case 12:
	case 15:
		speed = PCI_SPEED_100MHz_PCIX;
		break;
	case 13:
	case 16:
		speed = PCI_SPEED_133MHz_PCIX;
		break;
	default:
		speed = PCI_SPEED_UNKNOWN;
		break;
	}

	return speed;
}

/* Verify the existence of 'drc_name' and/or 'drc_type' within the
 * current node.
 */

int rpaphp_check_drc_props(struct device_node *dn, char *drc_name,
			char *drc_type)
{
	return arch_find_drc_match(dn, NULL, drc_type, drc_name,
				true, false, NULL);
}
EXPORT_SYMBOL_GPL(rpaphp_check_drc_props);

/**
 * rpaphp_add_slot -- declare a hotplug slot to the hotplug subsystem.
 * @dn: device node of slot
 *
 * This subroutine will register a hotpluggable slot with the
 * PCI hotplug infrastructure. This routine is typically called
 * during boot time, if the hotplug slots are present at boot time,
 * or is called later, by the dlpar add code, if the slot is
 * being dynamically added during runtime.
 *
 * If the device node points at an embedded (built-in) slot, this
 * routine will just return without doing anything, since embedded
 * slots cannot be hotplugged.
 *
 * To remove a slot, it suffices to call rpaphp_deregister_slot().
 */

static int rpaphp_add_slot_cb(struct device_node *dn,
			u32 drc_index, char *drc_name, char *drc_type,
			u32 drc_power_domain, void *data)
{
	struct slot *slot;
	int retval = 0;

	slot = alloc_slot_struct(dn, drc_index, drc_name, drc_power_domain);
	if (!slot)
		return -ENOMEM;

	slot->type = simple_strtoul(drc_type, NULL, 10);
	if (retval)
		return -EINVAL;

	dbg("Found drc-index:0x%x drc-name:%s drc-type:%s\n",
		drc_index, drc_name, drc_type);

	retval = rpaphp_enable_slot(slot);
	if (!retval)
		retval = rpaphp_register_slot(slot);

	if (retval)
		dealloc_slot_struct(slot);

	return retval;
}

int rpaphp_add_slot(struct device_node *dn)
{
	if (!dn->name || strcmp(dn->name, "pci"))
		return 0;

	return arch_find_drc_match(dn, rpaphp_add_slot_cb,
			NULL, NULL, false, true, NULL);
}
EXPORT_SYMBOL_GPL(rpaphp_add_slot);

static void __exit cleanup_slots(void)
{
	struct slot *slot, *next;

	/*
	 * Unregister all of our slots with the pci_hotplug subsystem,
	 * and free up all memory that we had allocated.
	 */

	list_for_each_entry_safe(slot, next, &rpaphp_slot_head,
				 rpaphp_slot_list) {
		list_del(&slot->rpaphp_slot_list);
		pci_hp_deregister(&slot->hotplug_slot);
		dealloc_slot_struct(slot);
	}
	return;
}

static int __init rpaphp_init(void)
{
	struct device_node *dn;

	info(DRIVER_DESC " version: " DRIVER_VERSION "\n");

	for_each_node_by_name(dn, "pci")
		rpaphp_add_slot(dn);

	return 0;
}

static void __exit rpaphp_exit(void)
{
	cleanup_slots();
}

static int enable_slot(struct hotplug_slot *hotplug_slot)
{
	struct slot *slot = to_slot(hotplug_slot);
	int state;
	int retval;

	if (slot->state == CONFIGURED)
		return 0;

	retval = rpaphp_get_sensor_state(slot, &state);
	if (retval)
		return retval;

	if (state == PRESENT) {
		pci_lock_rescan_remove();
		pci_hp_add_devices(slot->bus);
		pci_unlock_rescan_remove();
		slot->state = CONFIGURED;
	} else if (state == EMPTY) {
		slot->state = EMPTY;
	} else {
		err("%s: slot[%s] is in invalid state\n", __func__, slot->name);
		slot->state = NOT_VALID;
		return -EINVAL;
	}

	slot->bus->max_bus_speed = get_max_bus_speed(slot);
	return 0;
}

static int disable_slot(struct hotplug_slot *hotplug_slot)
{
	struct slot *slot = to_slot(hotplug_slot);
	if (slot->state == NOT_CONFIGURED)
		return -EINVAL;

	pci_lock_rescan_remove();
	pci_hp_remove_devices(slot->bus);
	pci_unlock_rescan_remove();
	vm_unmap_aliases();

	slot->state = NOT_CONFIGURED;
	return 0;
}

const struct hotplug_slot_ops rpaphp_hotplug_slot_ops = {
	.enable_slot = enable_slot,
	.disable_slot = disable_slot,
	.set_attention_status = set_attention_status,
	.get_power_status = get_power_status,
	.get_attention_status = get_attention_status,
	.get_adapter_status = get_adapter_status,
};

module_init(rpaphp_init);
module_exit(rpaphp_exit);
