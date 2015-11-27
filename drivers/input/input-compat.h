#ifndef _INPUT_COMPAT_H
#define _INPUT_COMPAT_H

/*
 * 32bit compatibility wrappers for the input subsystem.
 *
 * Very heavily based on evdev.c - Copyright (c) 1999-2002 Vojtech Pavlik
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 */

#include <linux/compiler.h>
#include <linux/compat.h>
#include <linux/input.h>

#ifdef CONFIG_COMPAT

/* Note to the author of this code: did it ever occur to
   you why the ifdefs are needed? Think about it again. -AK */
#if defined(CONFIG_X86_64) || defined(CONFIG_TILE)
#  define INPUT_COMPAT_TEST is_compat_task()
#elif defined(CONFIG_S390)
#  define INPUT_COMPAT_TEST test_thread_flag(TIF_31BIT)
#elif defined(CONFIG_MIPS)
#  define INPUT_COMPAT_TEST test_thread_flag(TIF_32BIT_ADDR)
#else
#  define INPUT_COMPAT_TEST test_thread_flag(TIF_32BIT)
#endif

struct input_event_compat {
	struct compat_timeval time;
	__u16 type;
	__u16 code;
	__s32 value;
};

struct ff_periodic_effect_compat {
	__u16 waveform;
	__u16 period;
	__s16 magnitude;
	__s16 offset;
	__u16 phase;

	struct ff_envelope envelope;

	__u32 custom_len;
	compat_uptr_t custom_data;
};

struct ff_effect_compat {
	__u16 type;
	__s16 id;
	__u16 direction;
	struct ff_trigger trigger;
	struct ff_replay replay;

	union {
		struct ff_constant_effect constant;
		struct ff_ramp_effect ramp;
		struct ff_periodic_effect_compat periodic;
		struct ff_condition_effect condition[2]; /* One for each axis */
		struct ff_rumble_effect rumble;
	} u;
};

#endif /* CONFIG_COMPAT */

enum event_clock_type {
	EV_CLK_REAL = 0,
	EV_CLK_MONO,
	EV_CLK_BOOT,
	EV_CLK_MAX
};

enum event_if_type {
	EV_IF_LEGACY = 0,
	EV_IF_RAW,
	EV_IF_COMPOSITE,
	EV_IF_MAX
};

static inline size_t input_event_size(int if_type)
{
	switch (if_type) {
	case EV_IF_LEGACY:
#ifdef CONFIG_COMPAT
		if (INPUT_COMPAT_TEST && !COMPAT_USE_64BIT_TIME)
			return sizeof(struct input_event_compat);
#endif
		return sizeof(struct input_event);
	case EV_IF_RAW:
		return sizeof(struct input_value);
	case EV_IF_COMPOSITE:
		return sizeof(struct input_composite_event);
	default:
		return 0;
	}
}

int input_event_from_user(const char __user *buffer,
			  struct input_value *event, int if_type);

int input_event_to_user(char __user *buffer, const struct input_value *event,
			int clk_type, int if_type);

#define input_value_to_user(buffer, event, if_type)	\
	input_event_to_user(buffer, event, 0, if_type)

int input_ff_effect_from_user(const char __user *buffer, size_t size,
			      struct ff_effect *effect);

#endif /* _INPUT_COMPAT_H */
