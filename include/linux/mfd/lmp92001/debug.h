/*
 * include/linux/mfd/lmp92001/debug.h - Debug interface for TI 92001
 *
 * Copyright 2016-2017 Celestica Ltd.
 *
 * Author: Abhisit Sangjan <s.abhisit@gmail.com>
 *
 * Inspired by wm831x driver.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#ifndef __MFD_LMP92001_DEBUG_H__
#define __MFD_LMP92001_DEBUG_H__

int lmp92001_debug_init(struct lmp92001 *lmp92001);
void lmp92001_debug_exit(struct lmp92001 *lmp92001);

#endif /* __MFD_LMP92001_DEBUG_H__ */
