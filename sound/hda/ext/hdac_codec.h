/*
 *  hdac_codec.h - HDA codec library
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

#ifndef __HDAC_CODEC_H__
#define __HDAC_CODEC_H__

#define HDA_MAX_CONNECTIONS 32
/* amp values */
#define AMP_IN_MUTE(idx)	(0x7080 | ((idx)<<8))
#define AMP_IN_UNMUTE(idx)	(0x7000 | ((idx)<<8))
#define AMP_OUT_MUTE		0xb080
#define AMP_OUT_UNMUTE		0xb000

struct hdac_codec_widget;
struct hdac_codec_connection_list {
	hda_nid_t nid;
	unsigned int type;
	struct hdac_codec_widget *input_w;
};

struct hdac_codec_widget {
	struct list_head head;
	hda_nid_t nid;
	unsigned int caps;
	unsigned int type;
	int num_inputs;
	struct hdac_codec_connection_list conn_list[HDA_MAX_CONNECTIONS];
	void *priv;	/* Codec specific widget data */
	void *params;	/* Widget specific parameters */
};

int snd_hdac_parse_widgets(struct hdac_device *hdac);
int snd_hdac_codec_init(struct hdac_device *hdac);
void snd_hdac_codec_cleanup(struct hdac_device *hdac);

#endif /* __HDAC_CODEC_H__ */
