/* SPDX-License-Identifier: GPL-2.0+ */
/* Copyright (c) 2018 Jernej Skrabec <jernej.skrabec@siol.net> */

#ifndef _SUN8I_TCON_TOP_H_
#define _SUN8I_TCON_TOP_H_

#include <linux/device.h>

struct sun8i_tcon_top;

enum tcon_type {
	tcon_type_lcd,
	tcon_type_tv,
};

void sun8i_tcon_top_set_hdmi_src(struct sun8i_tcon_top *tcon_top, int tcon);
void sun8i_tcon_top_de_config(struct sun8i_tcon_top *tcon_top,
			      int mixer, enum tcon_type tcon_type, int tcon);

#endif /* _SUN8I_TCON_TOP_H_ */
