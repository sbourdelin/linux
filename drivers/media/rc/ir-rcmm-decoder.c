/* ir-rcmm-decoder.c - A decoder for the RCMM IR protocol
 *
 * Copyright (C) 2016 by Patrick Lerda
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include "rc-core-priv.h"
#include <linux/module.h>
#include <linux/version.h>


#define RCMM_UNIT		166667	/* nanosecs */
#define RCMM_0_NBITS		64
#define RCMM_PREFIX_PULSE	416666  /* 166666.666666666*2.5 */
#define RCMM_PULSE_0            277777  /* 166666.666666666*(1+2/3) */
#define RCMM_PULSE_1            444444  /* 166666.666666666*(2+2/3) */
#define RCMM_PULSE_2            611111  /* 166666.666666666*(3+2/3) */
#define RCMM_PULSE_3            777778  /* 166666.666666666*(4+2/3) */
#define RCMM_MODE_MASK          0x0000

enum rcmm_state {
	STATE_INACTIVE,
	STATE_LOW,
	STATE_BUMP,
	STATE_VALUE,
	STATE_FINISHED,
};

static bool rcmm_mode(struct rcmm_dec *data)
{
        return !((0x000c0000 & data->bits) == 0x000c0000);
}

/**
 * ir_rcmm_decode() - Decode one RCMM pulse or space
 * @dev:	the struct rc_dev descriptor of the device
 * @ev:		the struct ir_raw_event descriptor of the pulse/space
 *
 * This function returns -EINVAL if the pulse violates the state machine
 */
static int ir_rcmm_decode(struct rc_dev *dev, struct ir_raw_event ev)
{
	struct rcmm_dec *data = &dev->raw->rcmm;
	u32 scancode;
	u8 toggle;

	if (!(dev->enabled_protocols & RC_PROTO_BIT_RCMM))
		return 0;

	if (!is_timing_event(ev)) {
		if (ev.reset)
			data->state = STATE_INACTIVE;
		return 0;
	}

	if (ev.duration > RCMM_PULSE_3 + RCMM_UNIT)
		goto out;

	switch (data->state) {

	case STATE_INACTIVE:
		if (!ev.pulse)
			break;

		/* Note: larger margin on first pulse since each RCMM_UNIT
		   is quite short and some hardware takes some time to
		   adjust to the signal */
		if (!eq_margin(ev.duration, RCMM_PREFIX_PULSE, RCMM_UNIT/2))
			break;

		data->state = STATE_LOW;
		data->count = 0;
		data->bits  = 0;
		return 0;

	case STATE_LOW:
		if (ev.pulse)
			break;

		/* Note: larger margin on first pulse since each RCMM_UNIT
		   is quite short and some hardware takes some time to
		   adjust to the signal */
		if (!eq_margin(ev.duration, RCMM_PULSE_0, RCMM_UNIT/2))
			break;

		data->state = STATE_BUMP;
		return 0;

	case STATE_BUMP:
		if (!ev.pulse)
			break;

		if (!eq_margin(ev.duration, RCMM_UNIT, RCMM_UNIT / 2))
			break;

		data->state = STATE_VALUE;
		return 0;

	case STATE_VALUE:
		if (ev.pulse)
			break;
	        {
			int value;

			if (eq_margin(ev.duration, RCMM_PULSE_0, RCMM_UNIT / 2)) {
				value = 0;
			} else if (eq_margin(ev.duration, RCMM_PULSE_1, RCMM_UNIT / 2))	{
				value = 1;
			} else if (eq_margin(ev.duration, RCMM_PULSE_2, RCMM_UNIT / 2))	{
				value = 2;
			} else if (eq_margin(ev.duration, RCMM_PULSE_3, RCMM_UNIT / 2))	{
				value = 3;
			} else
				break;

			data->bits <<= 2;
			data->bits |= value;
		}

		data->count+=2;

		if (data->count < 32) {
			data->state = STATE_BUMP;
		} else {
			data->state = STATE_FINISHED;
		}

		return 0;

	case STATE_FINISHED:
	        if (!ev.pulse) break;

		if (!eq_margin(ev.duration, RCMM_UNIT, RCMM_UNIT / 2))
			break;

		if (rcmm_mode(data)) {
			toggle = !!(0x8000 & data->bits);
			scancode = data->bits & ~0x8000;
		} else {
			toggle = 0;
			scancode = data->bits;
		}

		rc_keydown(dev, RC_PROTO_RCMM, scancode, toggle);
		data->state = STATE_INACTIVE;
		return 0;
	}

out:
	data->state = STATE_INACTIVE;
	return -EINVAL;
}

static struct ir_raw_handler rcmm_handler = {
	.protocols	= RC_PROTO_BIT_RCMM,
	.decode		= ir_rcmm_decode,
};

static int __init ir_rcmm_decode_init(void)
{
	ir_raw_handler_register(&rcmm_handler);

	printk(KERN_INFO "IR RCMM protocol handler initialized\n");
	return 0;
}

static void __exit ir_rcmm_decode_exit(void)
{
	ir_raw_handler_unregister(&rcmm_handler);
}

module_init(ir_rcmm_decode_init);
module_exit(ir_rcmm_decode_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Patrick LERDA");
MODULE_DESCRIPTION("RCMM IR protocol decoder");
