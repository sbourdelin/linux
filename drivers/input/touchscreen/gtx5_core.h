/*
 * Goodix GTx5 Touchscreen Driver
 * Core layer of touchdriver architecture.
 *
 * Copyright (C) 2015 - 2016 Goodix, Inc.
 * Authors:  Wang Yafei <wangyafei@goodix.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be a reference
 * to you, when you are integrating the GOODiX's CTP IC into your system,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 */
#ifndef _GTX5_CORE_H_
#define _GTX5_CORE_H_

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/firmware.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/kthread.h>
#include <linux/version.h>
#include <linux/delay.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/input.h>
#include <asm/unaligned.h>
#ifdef CONFIG_OF
#include <linux/of_gpio.h>
#include <linux/regulator/consumer.h>
#endif

#include <linux/gpio/consumer.h>

/* macros definition */
#define GTX5_CORE_DRIVER_NAME	"gtx5_ts"
#define GTX5_DRIVER_VERSION	"v0.8"
#define GTX5_BUS_RETRY_TIMES	3
#define GTX5_MAX_TOUCH		10
#define GTX5_MAX_KEY		3
#define GTX5_CFG_MAX_SIZE	1024

/*
 * struct gtx5_ts_board_data -  board data
 * @avdd_name: name of analoy regulator
 * @reset_gpio: reset gpio number
 * @irq_gpio: interrupt gpio number
 * @irq_flag: irq trigger type
 * @power_on_delay_us: power on delay time (us)
 * @power_off_delay_us: power off delay time (us)
 * @swap_axis: whether swaw x y axis
 * @panel_max_id: max supported fingers
 * @panel_max_x/y/w/p: resolution and size
 * @panel_max_key: max supported keys
 * @pannel_key_map: key map
 * @fw_name: name of the firmware image
 */
struct gtx5_ts_board_data {
	const char *avdd_name;
	struct gpio_desc *reset_gpiod;
	struct gpio_desc *irq_gpiod;
	int irq;
	unsigned int  irq_flags;

	unsigned int power_on_delay_us;
	unsigned int power_off_delay_us;

	unsigned int swap_axis;
	unsigned int panel_max_id; /*max touch id*/
	unsigned int panel_max_x;
	unsigned int panel_max_y;
	unsigned int panel_max_w; /*major and minor*/
	unsigned int panel_max_key;
	unsigned int panel_key_map[GTX5_MAX_KEY];

	const char *fw_name;
	bool esd_default_on;
};

/*
 * struct gtx5_ts_config - chip config data
 * @initialized: whether initialized
 * @name: name of this config
 * @lock: mutex for config data
 * @reg_base: register base of config data
 * @length: bytes of the config
 * @delay: delay time after sending config
 * @data: config data buffer
 */
struct gtx5_ts_config {
	bool initialized;
	char name[24];
	struct mutex lock;
	unsigned int reg_base;
	unsigned int length;
	unsigned int delay; /*ms*/
	unsigned char data[GTX5_CFG_MAX_SIZE];
};

/*
 * struct gtx5_ts_cmd - command package
 * @initialized: whether initialized
 * @cmd_reg: command register
 * @length: command length in bytes
 * @cmds: command data
 */
#pragma pack(4)
struct gtx5_ts_cmd {
	u32 initialized;
	u32 cmd_reg;
	u32 length;
	u8 cmds[3];
};

#pragma pack()

/* interrupt event type */
enum ts_event_type {
	EVENT_INVALID,
	EVENT_TOUCH,
	EVENT_REQUEST,
};

/* requset event type */
enum ts_request_type {
	REQUEST_INVALID,
	REQUEST_CONFIG,
	REQUEST_BAKREF,
	REQUEST_RESET,
	REQUEST_MAINCLK,
};

/* notifier event */

enum ts_notify_event {
	NOTIFY_FWUPDATE_START,
	NOTIFY_FWUPDATE_END,
	NOTIFY_SUSPEND,
	NOTIFY_RESUME,
};

/* coordinate package */
struct gtx5_ts_coords {
	int id;
	unsigned int x, y, w, p;
};

/* touch event data */
struct gtx5_touch_data {
	/* finger */
	int touch_num;
	struct gtx5_ts_coords coords[GTX5_MAX_TOUCH];
	/* key */
	u16 key_value;
};

/* request event data */
struct gtx5_request_data {
	enum ts_request_type request_type;
};

/*
 * struct gtx5_ts_event - touch event struct
 * @event_type: touch event type, touch data or
 *	request event
 * @event_data: event data
 */
struct gtx5_ts_event {
	enum ts_event_type event_type;
	union {
		struct gtx5_touch_data touch_data;
		struct gtx5_request_data request_data;
	} event_data;
};

/*
 * struct gtx5_ts_version - firmware version
 * @valid: whether these information is valid
 * @pid: product id string
 * @vid: firmware version code
 * @cid: customer id code
 * @sensor_id: sendor id
 */
struct gtx5_ts_version {
	bool valid;
	char pid[5];
	u16 vid;
	u8 cid;
	u8 sensor_id;
};

/*
 * struct gtx5_ts_device - ts device data
 * @name: device name
 * @version: reserved
 * @bus_type: i2c or spi
 * @board_data: board data obtained from dts
 * @normal_cfg: normal config data
 * @highsense_cfg: high sense config data
 * @hw_ops: hardware operations
 * @chip_version: firmware version information
 * @sleep_cmd: sleep commang
 * @gesture_cmd: gesture command
 * @dev: device pointer,may be a i2c or spi device
 * @of_node: device node
 */
struct gtx5_ts_device {
	char *name;
	int version;
	int bus_type;

	struct gtx5_ts_board_data *board_data;
	struct gtx5_ts_config *normal_cfg;
	struct gtx5_ts_config *highsense_cfg;
	const struct gtx5_ts_hw_ops *hw_ops;

	struct gtx5_ts_version chip_version;
	struct gtx5_ts_cmd sleep_cmd;
	struct gtx5_ts_cmd gesture_cmd;

	struct device *dev;
};

/*
 * struct gtx5_ts_hw_ops -  hardware opeartions
 * @init: hardware initialization
 * @reset: hardware reset
 * @read: read data from touch device
 * @write: write data to touch device
 * @send_cmd: send command to touch device
 * @send_config: send configuration data
 * @read_version: read firmware version
 * @event_handler: touch event handler
 * @suspend: put touch device into low power mode
 * @resume: put touch device into working mode
 */
struct gtx5_ts_hw_ops {
	int (*init)(struct gtx5_ts_device *dev);
	void (*reset)(struct gtx5_ts_device *dev);
	int (*read)(struct gtx5_ts_device *dev, unsigned int addr,
		    unsigned char *data, unsigned int len);
	int (*write)(struct gtx5_ts_device *dev, unsigned int addr,
		     unsigned char *data, unsigned int len);
	int (*send_cmd)(struct gtx5_ts_device *dev,
			struct gtx5_ts_cmd  *cmd);
	int (*send_config)(struct gtx5_ts_device *dev,
			   struct gtx5_ts_config *config);
	int (*read_version)(struct gtx5_ts_device *dev,
			    struct gtx5_ts_version *version);
	int (*event_handler)(struct gtx5_ts_device *dev,
			     struct gtx5_ts_event *ts_event);
	int (*check_hw)(struct gtx5_ts_device *dev);
	int (*suspend)(struct gtx5_ts_device *dev);
	int (*resume)(struct gtx5_ts_device *dev);
};

/*
 * struct gtx5_ts_esd - esd protector structure
 * @esd_work: esd delayed work
 * @est_mutex: mutex for esd_on flag
 * @esd_on: true - turn on esd protection, false - turn
 *  off esd protection
 * @esd_mutex: protect @esd_on flag
 */
struct gtx5_ts_esd {
	struct delayed_work esd_work;
	struct mutex esd_mutex;
	struct notifier_block esd_notifier;
	struct gtx5_ts_core *ts_core;
	bool esd_on;
};

/*
 * struct godix_ts_core - core layer data struct
 * @pdev: core layer platform device
 * @ts_dev: hardware layer touch device
 * @input_dev: input device
 * @avdd: analog regulator
 * @pinctrl: pinctrl handler
 * @pin_sta_active: active/normal pin state
 * @pin_sta_suspend: suspend/sleep pin state
 * @ts_event: touch event data struct
 * @power_on: power on/off flag
 * @irq: irq number
 * @irq_enabled: irq enabled/disabled flag
 * @suspended: suspend/resume flag
 * @hw_err: indicate that hw_ops->init() failed
 * @ts_notifier: generic notifier
 * @ts_esd: esd protector structure
 * @fb_notifier: framebuffer notifier
 * @early_suspend: early suspend
 */
struct gtx5_ts_core {
	struct platform_device *pdev;
	struct gtx5_ts_device *ts_dev;
	struct input_dev *input_dev;

	struct regulator *avdd;
	struct gtx5_ts_event ts_event;
	int power_on;
	int irq;
	size_t irq_trig_cnt;

	atomic_t irq_enabled;
	atomic_t suspended;
	bool hw_err;

	struct notifier_block ts_notifier;
	struct gtx5_ts_esd ts_esd;

#ifdef CONFIG_FB
	struct notifier_block fb_notifier;
#endif
};

/* external module structures */
enum gtx5_ext_priority {
	EXTMOD_PRIO_RESERVED = 0,
	EXTMOD_PRIO_FWUPDATE,
	EXTMOD_PRIO_GESTURE,
	EXTMOD_PRIO_HOTKNOT,
	EXTMOD_PRIO_DBGTOOL,
	EXTMOD_PRIO_DEFAULT,
};

struct gtx5_ext_module;
/* external module's operations callback */
struct gtx5_ext_module_funcs {
	int (*init)(struct gtx5_ts_core *core_data,
		    struct gtx5_ext_module *module);
	int (*exit)(struct gtx5_ts_core *core_data,
		    struct gtx5_ext_module *module);

	int (*before_reset)(struct gtx5_ts_core *core_data,
			    struct gtx5_ext_module *module);
	int (*after_reset)(struct gtx5_ts_core *core_data,
			   struct gtx5_ext_module *module);

	int (*before_suspend)(struct gtx5_ts_core *core_data,
			      struct gtx5_ext_module *module);
	int (*after_suspend)(struct gtx5_ts_core *core_data,
			     struct gtx5_ext_module *module);

	int (*before_resume)(struct gtx5_ts_core *core_data,
			     struct gtx5_ext_module *module);
	int (*after_resume)(struct gtx5_ts_core *core_data,
			    struct gtx5_ext_module *module);

	int (*irq_event)(struct gtx5_ts_core *core_data,
			 struct gtx5_ext_module *module);
};

/*
 * struct gtx5_ext_module - external module struct
 * @list: list used to link into modules manager
 * @name: name of external module
 * @priority: module priority vlaue, zero is invalid
 * @funcs: operations callback
 * @priv_data: private data region
 * @kobj: kobject
 * @work: used to queue one work to do registration
 */
struct gtx5_ext_module {
	struct list_head list;
	char *name;
	enum gtx5_ext_priority priority;
	const struct gtx5_ext_module_funcs *funcs;
	void *priv_data;
	struct kobject kobj;
	struct work_struct work;
};

/*
 * struct gtx5_ext_attribute - exteranl attribute struct
 * @attr: attribute
 * @show: show interface of external attribute
 * @store: store interface of external attribute
 */
struct gtx5_ext_attribute {
	struct attribute attr;
	ssize_t (*show)(struct gtx5_ext_module *, char *);
	ssize_t (*store)(struct gtx5_ext_module *, const char *, size_t);
};

/* external attrs helper macro */
#define __EXTMOD_ATTR(_name, _mode, _show, _store)	{	\
	.attr = {.name = __stringify(_name), .mode = _mode },	\
	.show   = _show,	\
	.store  = _store,	\
}

/* external attrs helper macro, used to define external attrs */
#define DEFINE_EXTMOD_ATTR(_name, _mode, _show, _store)	\
static struct gtx5_ext_attribute ext_attr_##_name = \
	__EXTMOD_ATTR(_name, _mode, _show, _store)

/*
 * get board data pointer
 */
static inline struct gtx5_ts_board_data *board_data(
		struct gtx5_ts_core *core)
{
	return core->ts_dev->board_data;
}

/*
 * get touch hardware operations pointer
 */
static inline const struct gtx5_ts_hw_ops *ts_hw_ops(
		struct gtx5_ts_core *core)
{
	return core->ts_dev->hw_ops;
}

/*
 * checksum helper functions
 * checksum can be u8/le16/be16/le32/be32 format
 * NOTE: the caller should be responsible for the
 * legality of @data and @size parameters, so be
 * careful when call these functions.
 */
static inline u8 checksum_u8(u8 *data, u32 size)
{
	u8 checksum = 0;
	u32 i;

	for (i = 0; i < size; i++)
		checksum += data[i];
	return checksum;
}

static inline u16 checksum_le16(u8 *data, u32 size)
{
	u16 checksum = 0;
	u32 i;

	for (i = 0; i < size; i += 2)
		checksum += le16_to_cpup((__le16 *)(data + i));
	return checksum;
}

static inline u16 checksum_be16(u8 *data, u32 size)
{
	u16 checksum = 0;
	u32 i;

	for (i = 0; i < size; i += 2)
		checksum += be16_to_cpup((__be16 *)(data + i));
	return checksum;
}

static inline u32 checksum_le32(u8 *data, u32 size)
{
	u32 checksum = 0;
	u32 i;

	for (i = 0; i < size; i += 4)
		checksum += le32_to_cpup((__le32 *)(data + i));
	return checksum;
}

static inline u32 checksum_be32(u8 *data, u32 size)
{
	u32 checksum = 0;
	u32 i;

	for (i = 0; i < size; i += 4)
		checksum += be32_to_cpup((__be32 *)(data + i));
	return checksum;
}

/*
 * define event action
 * EVT_xxx macros are used in opeartions callback
 * defined in @gtx5_ext_module_funcs to control
 * the behaviors of event such as suspend/resume/
 * irq_event.
 *
 * generally there are two types of behaviors:
 *	1. you want the flow of this event be canceled,
 *	in this condition, you should return EVT_CANCEL_XXX
 *	in the operations callback.
 *		e.g. the firmware update module is updating
 *		the firmware, you want to cancel suspend flow,
 *		so you need to return EVT_CANCEL_SUSPEND in
 *		suspend callback function.
 *	2. you want the flow of this event continue, in
 *	this condition, you should return EVT_HANDLED in
 *	the callback function.
 */
#define EVT_HANDLED			0
#define EVT_CONTINUE			0
#define EVT_CANCEL			1
#define EVT_CANCEL_IRQEVT		1
#define EVT_CANCEL_SUSPEND		1
#define EVT_CANCEL_RESUME		1
#define EVT_CANCEL_RESET		1

/*
 * errno define
 * Note:
 *	1. bus read/write functions defined in hardware
 *	  layer code(e.g. gtx5_xxx_i2c.c) *must* return
 *	  -EBUS if failed to transfer data on bus.
 */
#define EBUS					1000
#define ETIMEOUT				1001
#define ECHKSUM					1002
#define EMEMCMP					1003

/**
 * gtx5_register_ext_module - interface for external module
 * to register into touch core modules structure
 *
 * @module: pointer to external module to be register
 * return: 0 ok, <0 failed
 */
int gtx5_register_ext_module(struct gtx5_ext_module *module);

/**
 * gtx5_unregister_ext_module - interface for external module
 * to unregister external modules
 *
 * @module: pointer to external module
 * return: 0 ok, <0 failed
 */
int gtx5_unregister_ext_module(struct gtx5_ext_module *module);

/**
 * gtx5_ts_irq_enable - Enable/Disable a irq

 * @core_data: pointer to touch core data
 * enable: enable or disable irq
 * return: 0 ok, <0 failed
 */
int gtx5_ts_irq_enable(struct gtx5_ts_core *core_data, bool enable);

struct kobj_type *gtx5_get_default_ktype(void);

/**
 * gtx5_ts_blocking_notify - notify clients of certain events
 *	see enum ts_notify_event in gtx5_ts_core.h
 */
int gtx5_ts_blocking_notify(enum ts_notify_event evt, void *v);

#endif
