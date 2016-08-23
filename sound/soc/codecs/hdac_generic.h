/*
 *  hdac_generic.h - ASoc HDA generic codec driver
 *
 *  Copyright (C) 2016-2017 Intel Corp
 *  Author: Subhransu S. Prusty <subhransu.s.prusty@intel.com>
 *  ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; version 2 of the License.
 *
 *  This program is distributed in the hope that it will be useful, but
 *  WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  General Public License for more details.
 *
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 */

#ifndef __HDAC_GENERIC_H__
#define __HDAC_GENERIC_H__

#define HDAC_GENERIC_NAME_SIZE	32
#define AMP_OUT_MUTE		0xb080
#define AMP_OUT_UNMUTE		0xb000
#define PIN_OUT			(AC_PINCTL_OUT_EN)

struct hdac_generic_vendor_ops {
	int (*init)(struct hdac_ext_device *edev);
	int (*cleanup)(struct hdac_ext_device *edev);
};

int hdac_generic_machine_control_init(struct snd_soc_dapm_context *dapm,
					struct snd_soc_codec *codec);

#endif /* __HDAC_GENERIC_H__ */
