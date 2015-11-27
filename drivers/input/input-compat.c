/*
 * 32bit compatibility wrappers for the input subsystem.
 *
 * Very heavily based on evdev.c - Copyright (c) 1999-2002 Vojtech Pavlik
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 */

#include <linux/export.h>
#include <asm/uaccess.h>
#include "input-compat.h"

static ktime_t input_get_time(int clk_type)
{
	switch (clk_type) {
	case EV_CLK_MONO:
		return ktime_get();
	case EV_CLK_BOOT:
		return ktime_get_boottime();
	case EV_CLK_REAL:
	default:
		return ktime_get_real();
	}
}

int input_event_from_user(const char __user *buffer,
			  struct input_value *event, int if_type)
{
	if (if_type == EV_IF_LEGACY) {
#ifdef CONFIG_COMPAT
		if (INPUT_COMPAT_TEST && !COMPAT_USE_64BIT_TIME) {
			struct input_event_compat compat_event;

			if (copy_from_user(&compat_event, buffer,
					   sizeof(struct input_event_compat)))
				return -EFAULT;

			event->type = compat_event.type;
			event->code = compat_event.code;
			event->value = compat_event.value;
		} else {
#endif
			struct input_event ev;

			if (copy_from_user(&ev, buffer,
						sizeof(struct input_event)))
				return -EFAULT;

			/* drop timestamp from userspace */
			event->type = ev.type;
			event->code = ev.code;
			event->value = ev.value;
#ifdef CONFIG_COMPAT
		}
#endif
	} else if (if_type == EV_IF_RAW || if_type == EV_IF_COMPOSITE) {
		if (copy_from_user(event, buffer, sizeof(struct input_value)))
			return -EFAULT;
	} else
		return -EINVAL;

	return 0;
}

int input_event_to_user(char __user *buffer, const struct input_value *event,
			int clk_type, int if_type)
{
	if (if_type == EV_IF_LEGACY) {
		struct timeval timestamp = ktime_to_timeval(
				input_get_time(clk_type));

#ifdef CONFIG_COMPAT
		if (INPUT_COMPAT_TEST && !COMPAT_USE_64BIT_TIME) {
			struct input_event_compat compat_event;

			compat_event.time.tv_sec = timestamp.tv_sec;
			compat_event.time.tv_usec = timestamp.tv_usec;
			compat_event.type = event->type;
			compat_event.code = event->code;
			compat_event.value = event->value;

			if (copy_to_user(buffer, &compat_event,
					 sizeof(struct input_event_compat)))
				return -EFAULT;
		} else {
#endif
			struct input_event ev;

			ev.time = timestamp;
			ev.type = event->type;
			ev.code = event->code;
			ev.value = event->value;

			if (copy_to_user(buffer, &ev,
					sizeof(struct input_event)))
				return -EFAULT;
#ifdef CONFIG_COMPAT
		}
#endif
	} else if (if_type == EV_IF_RAW || if_type == EV_IF_COMPOSITE) {
		if (copy_to_user(buffer, event, sizeof(struct input_value)))
			return -EFAULT;

		if (if_type != EV_IF_RAW) {
			/*
			 * composite interface, send timestamp event
			 *
			 * s64 and input_value are the same size, use s64
			 * directly here.
			 */
			s64 time = ktime_to_ns(input_get_time(clk_type));

			if (copy_to_user(buffer + sizeof(struct input_value),
						&time, sizeof(s64)))
				return -EFAULT;
		}
	} else
		return -EINVAL;

	return 0;
}

#ifdef CONFIG_COMPAT

int input_ff_effect_from_user(const char __user *buffer, size_t size,
			      struct ff_effect *effect)
{
	if (INPUT_COMPAT_TEST) {
		struct ff_effect_compat *compat_effect;

		if (size != sizeof(struct ff_effect_compat))
			return -EINVAL;

		/*
		 * It so happens that the pointer which needs to be changed
		 * is the last field in the structure, so we can retrieve the
		 * whole thing and replace just the pointer.
		 */
		compat_effect = (struct ff_effect_compat *)effect;

		if (copy_from_user(compat_effect, buffer,
				   sizeof(struct ff_effect_compat)))
			return -EFAULT;

		if (compat_effect->type == FF_PERIODIC &&
		    compat_effect->u.periodic.waveform == FF_CUSTOM)
			effect->u.periodic.custom_data =
				compat_ptr(compat_effect->u.periodic.custom_data);
	} else {
		if (size != sizeof(struct ff_effect))
			return -EINVAL;

		if (copy_from_user(effect, buffer, sizeof(struct ff_effect)))
			return -EFAULT;
	}

	return 0;
}

#else

int input_ff_effect_from_user(const char __user *buffer, size_t size,
			      struct ff_effect *effect)
{
	if (size != sizeof(struct ff_effect))
		return -EINVAL;

	if (copy_from_user(effect, buffer, sizeof(struct ff_effect)))
		return -EFAULT;

	return 0;
}

#endif /* CONFIG_COMPAT */

EXPORT_SYMBOL_GPL(input_event_from_user);
EXPORT_SYMBOL_GPL(input_event_to_user);
EXPORT_SYMBOL_GPL(input_ff_effect_from_user);
