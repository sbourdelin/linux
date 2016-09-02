/*
 *  hdac_ext_codec.h - HDA ext codec helpers
 *
 *  Copyright (C) 2016 Intel Corp
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

#ifndef __HDAC_EXT_CODEC_H__
#define __HDAC_EXT_CODEC_H__

int snd_hdac_ext_parse_widgets(struct hdac_ext_device *hdac);
void snd_hdac_ext_codec_cleanup(struct hdac_ext_device *hdac);

#endif /* __HDAC_EXT_CODEC_H__ */
