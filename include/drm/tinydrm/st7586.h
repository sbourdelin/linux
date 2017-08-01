/*
 * ST7586 LCD controller
 *
 * Copyright (C) 2017 David Lechner <david@lechnology.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef __LINUX_ST7856_H
#define __LINUX_ST7856_H

#define ST7586_DISP_MODE_GRAY	0x38
#define ST7586_DISP_MODE_MONO	0x39
#define ST7586_ENABLE_DDRAM	0x3a
#define ST7586_SET_DISP_DUTY	0xb0
#define ST7586_SET_PART_DISP	0xb4
#define ST7586_SET_NLINE_INV	0xb5
#define ST7586_SET_VOP		0xc0
#define ST7586_SET_BIAS_SYSTEM	0xc3
#define ST7586_SET_BOOST_LEVEL	0xc4
#define ST7586_SET_VOP_OFFSET	0xc7
#define ST7586_ENABLE_ANALOG	0xd0
#define ST7586_AUTO_READ_CTRL	0xd7
#define ST7586_OTP_RW_CTRL	0xe0
#define ST7586_OTP_CTRL_OUT	0xe1
#define ST7586_OTP_READ		0xe3

#define ST7586_DISP_CTRL_MX	BIT(6)
#define ST7586_DISP_CTRL_MY	BIT(7)

#endif /* __LINUX_ST7856_H */
