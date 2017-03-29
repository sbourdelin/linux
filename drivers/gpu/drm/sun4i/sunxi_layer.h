/*
 * Copyright (C) 2017 Icenowy Zheng <icenowy@aosc.xyz>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 */

#ifndef _SUNXI_LAYER_H_
#define _SUNXI_LAYER_H_

struct sunxi_layer_ops {
	struct drm_plane *(*get_plane)(void *layer);
};

#endif /* _SUNXI_LAYER_H_ */
