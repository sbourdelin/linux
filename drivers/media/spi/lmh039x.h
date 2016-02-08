/*
 * LMH0395 SPI driver.
 * Copyright (C) 2014  Jean-Michel Hautbois
 *
 * 3G HD/SD SDI Dual Output Low Power Extended Reach Adaptive Cable Equalizer
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
 */

#ifndef _LMH039X_
#define _LMH039X_

#include <media/v4l2-device.h>

#define	LMH0395_SPI_CMD_WRITE	0x00
#define	LMH0395_SPI_CMD_READ	0x80

/* Registers of LMH0395 */
#define LMH0395_GENERAL_CTRL		0x00
#define LMH0395_OUTPUT_DRIVER		0x01
#define LMH0395_LAUNCH_AMP_CTRL		0x02
#define LMH0395_MUTE_REF		0x03
#define LMH0395_DEVICE_ID		0x04
#define	LMH0395_RATE_INDICATOR		0x05
#define	LMH0395_CABLE_LENGTH_INDICATOR	0x06
#define	LMH0395_LAUNCH_AMP_INDICATION	0x07

/* This is a one input, dual output device */
#define LMH0395_SDI_INPUT	0
#define LMH0395_SDI_OUT0	1
#define LMH0395_SDI_OUT1	2

#define LMH0395_PADS_NUM	3

#define ID_LMH0384		0x03
#define ID_LMH0394		0x13
#define ID_LMH0395		0x23

/* Register LMH0395_MUTE_REF bits [7:6] */
enum lmh0395_output_type {
	LMH0395_OUTPUT_TYPE_NONE,
	LMH0395_OUTPUT_TYPE_SDO0,
	LMH0395_OUTPUT_TYPE_SDO1,
	LMH0395_OUTPUT_TYPE_BOTH
};

#endif
