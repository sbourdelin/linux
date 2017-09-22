/*
 * Copyright (C) 2016 Noralf Trønnes
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef __LINUX_TINYDRM_BACKLIGHT_H
#define __LINUX_TINYDRM_BACKLIGHT_H

struct backlight_device;
struct backlight_device *tinydrm_of_find_backlight(struct device *dev);
int tinydrm_enable_backlight(struct backlight_device *backlight);
int tinydrm_disable_backlight(struct backlight_device *backlight);

#endif /* __LINUX_TINYDRM_BACKLIGHT_H */
