/*
 * Industrial I/O counter interface
 * Copyright (C) 2017 William Breathitt Gray
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 2, as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */
#ifndef _IIO_COUNTER_H_
#define _IIO_COUNTER_H_

#ifdef CONFIG_IIO_COUNTER

#include <linux/device.h>
#include <linux/mutex.h>
#include <linux/types.h>

#include <linux/iio/iio.h>

/**
 * struct iio_counter_signal - IIO Counter Signal node
 * @id:		[DRIVER] unique ID used to identify signal
 * @name:	[DRIVER] device-specific signal name
 * @list:	[INTERN] list of all signals currently registered to counter
 */
struct iio_counter_signal {
	int		id;
	const char	*name;

	struct list_head	list;
};

/**
 * struct iio_counter_trigger - IIO Counter Trigger node
 * @mode:		[DRIVER] current trigger mode state
 * @trigger_modes:	[DRIVER] available trigger modes
 * @num_trigger_modes:	[DRIVER] number of modes specified in @trigger_modes
 * @signal:		[DRIVER] pointer to associated signal
 * @list:		[INTERN] list of all triggers currently registered to
 *			value
 */
struct iio_counter_trigger {
	unsigned int			mode;
	const char *const		*trigger_modes;
	unsigned int			num_trigger_modes;
	struct iio_counter_signal	*signal;

	struct list_head		list;
};

/**
 * struct iio_counter_value - IIO Counter Value node
 * @id:			[DRIVER] unique ID used to identify value
 * @name:		[DRIVER] device-specific value name
 * @mode:		[DRIVER] current function mode state
 * @function_modes:	[DRIVER] available function modes
 * @num_function_modes:	[DRIVER] number of modes specified in @function_modes
 * @init_triggers:	[DRIVER] array of triggers for initialization
 * @num_init_triggers:	[DRIVER] number of triggers specified in @init_triggers
 * @function_enum:	[INTERN] used internally to generate function attributes
 * @trigger_list_lock:	[INTERN] lock for accessing @trigger_list
 * @trigger_list:	[INTERN] list of all triggers currently registered to
 *			value
 * @list:		[INTERN] list of all values currently registered to
 *			counter
 */
struct iio_counter_value {
	int			id;
	const char		*name;
	unsigned int		mode;
	const char *const	*function_modes;
	unsigned int		num_function_modes;

	struct iio_counter_trigger	*init_triggers;
	size_t				num_init_triggers;

	struct iio_enum		function_enum;
	struct mutex		trigger_list_lock;
	struct list_head	trigger_list;

	struct list_head	list;
};

struct iio_counter;

/**
 * struct iio_counter_ops - IIO Counter related callbacks
 * @signal_read:	function to request a signal value from the device.
 *			Return value will specify the type of value returned by
 *			the device. val and val2 will contain the elements
 *			making up the returned value. Note that the counter
 *			signal_list_lock is acquired before this function is
 *			called, and released after this function returns.
 * @signal_write:	function to write a signal value to the device.
 *			Parameters and locking behavior are the same as
 *			signal_read.
 * @trigger_mode_set:	function to set the trigger mode. mode is the index of
 *			the requested mode from the value trigger_modes array.
 *			Note that the counter value_list_lock and value
 *			trigger_list_lock are acquired before this function is
 *			called, and released after this function returns.
 * @trigger_mode_get:	function to get the current trigger mode. Return value
 *			will specify the index of the current mode from the
 *			value trigger_modes array. Locking behavior is the same
 *			as trigger_mode_set.
 * @value_read:		function to request a value value from the device.
 *			Return value will specify the type of value returned by
 *			the device. val and val2 will contain the elements
 *			making up the returned value. Note that the counter
 *			value_list_lock is acquired before this function is
 *			called, and released after this function returns.
 * @value_write:	function to write a value value to the device.
 *			Parameters and locking behavior are the same as
 *			value_read.
 * @value_function_set: function to set the value function mode. mode is the
 *			index of the requested mode from the value
 *			function_modes array. Note that the counter
 *			value_list_lock is acquired before this function is
 *			called, and released after this function returns.
 * @value_function_get: function to get the current value function mode. Return
 *			value will specify the index of the current mode from
 *			the value function_modes array. Locking behavior is the
 *			same as value_function_get.
 */
struct iio_counter_ops {
	int (*signal_read)(struct iio_counter *counter,
		struct iio_counter_signal *signal, int *val, int *val2);
	int (*signal_write)(struct iio_counter *counter,
		struct iio_counter_signal *signal, int val, int val2);
	int (*trigger_mode_set)(struct iio_counter *counter,
		struct iio_counter_value *value,
		struct iio_counter_trigger *trigger, unsigned int mode);
	int (*trigger_mode_get)(struct iio_counter *counter,
		struct iio_counter_value *value,
		struct iio_counter_trigger *trigger);
	int (*value_read)(struct iio_counter *counter,
		struct iio_counter_value *value, int *val, int *val2);
	int (*value_write)(struct iio_counter *counter,
		struct iio_counter_value *value, int val, int val2);
	int (*value_function_set)(struct iio_counter *counter,
		struct iio_counter_value *value, unsigned int mode);
	int (*value_function_get)(struct iio_counter *counter,
		struct iio_counter_value *value);
};

/**
 * struct iio_counter - IIO Counter data structure
 * @id:			[DRIVER] unique ID used to identify counter
 * @name:		[DRIVER] name of the device
 * @dev:		[DRIVER] device structure, should be assigned a parent
 *			and owner
 * @ops:		[DRIVER] callbacks for from driver
 * @init_signals:	[DRIVER] array of signals for initialization
 * @num_init_signals:	[DRIVER] number of signals specified in @init_signals
 * @init_values:	[DRIVER] array of values for initialization
 * @num_init_values:	[DRIVER] number of values specified in @init_values
 * @signal_list_lock:	[INTERN] lock for accessing @signal_list
 * @signal_list:	[INTERN] list of all signals currently registered to
 *			counter
 * @value_list_lock:	[INTERN] lock for accessing @value_list
 * @value_list:		[INTERN] list of all values currently registered to
 *			counter
 * @channels:		[DRIVER] channel specification structure table
 * @num_channels:	[DRIVER] number of channels specified in @channels
 * @info:		[DRIVER] callbacks and constant info from driver
 * @indio_dev:		[INTERN] industrial I/O device structure
 * @driver_data:	[DRIVER] driver data
 */
struct iio_counter {
	int				id;
	const char			*name;
	struct device			*dev;
	const struct iio_counter_ops	*ops;

	struct iio_counter_signal	*init_signals;
	size_t				num_init_signals;
	struct iio_counter_value	*init_values;
	size_t				num_init_values;

	struct mutex		signal_list_lock;
	struct list_head	signal_list;
	struct mutex		value_list_lock;
	struct list_head	value_list;

	const struct iio_chan_spec	*channels;
	size_t				num_channels;
	const struct iio_info		*info;

	struct iio_dev	*indio_dev;
	void		*driver_data;
};

int iio_counter_trigger_register(struct iio_counter_value *const value,
	struct iio_counter_trigger *const trigger);
void iio_counter_trigger_unregister(struct iio_counter_value *const value,
	struct iio_counter_trigger *const trigger);
int iio_counter_triggers_register(struct iio_counter_value *const value,
	struct iio_counter_trigger *const triggers, const size_t num_triggers);
void iio_counter_triggers_unregister(struct iio_counter_value *const value,
	struct iio_counter_trigger *triggers, size_t num_triggers);

int iio_counter_value_register(struct iio_counter *const counter,
	struct iio_counter_value *const value);
void iio_counter_value_unregister(struct iio_counter *const counter,
	struct iio_counter_value *const value);
int iio_counter_values_register(struct iio_counter *const counter,
	struct iio_counter_value *const values, const size_t num_values);
void iio_counter_values_unregister(struct iio_counter *const counter,
	struct iio_counter_value *values, size_t num_values);

int iio_counter_register(struct iio_counter *const counter);
void iio_counter_unregister(struct iio_counter *const counter);

#endif /* CONFIG_IIO_COUNTER */

#endif /* _IIO_COUNTER_H_ */
