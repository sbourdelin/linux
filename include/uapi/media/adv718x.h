/*
 * Copyright (c) 2014-2015 Mentor Graphics Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version
 */

#ifndef __UAPI_MEDIA_ADV718X_H__
#define __UAPI_MEDIA_ADV718X_H__

enum adv718x_ctrl_id {
	V4L2_CID_ADV718x_OFF_CB = (V4L2_CID_USER_ADV718X_BASE + 0),
	V4L2_CID_ADV718x_OFF_CR,
	V4L2_CID_ADV718x_FREERUN_ENABLE,
	V4L2_CID_ADV718x_FORCE_FREERUN,
	V4L2_CID_ADV718x_FREERUN_Y,
	V4L2_CID_ADV718x_FREERUN_CB,
	V4L2_CID_ADV718x_FREERUN_CR,
	/* Chroma Transient Improvement Controls */
	V4L2_CID_ADV718x_CTI_ENABLE,
	V4L2_CID_ADV718x_CTI_AB_ENABLE,
	V4L2_CID_ADV718x_CTI_AB,
	V4L2_CID_ADV718x_CTI_THRESH,
	/* Digital Noise Reduction and Lumanance Peaking Gain Controls */
	V4L2_CID_ADV718x_DNR_ENABLE,
	V4L2_CID_ADV718x_DNR_THRESH1,
	V4L2_CID_ADV718x_LUMA_PEAK_GAIN,
	V4L2_CID_ADV718x_DNR_THRESH2,
	/* ADV7182 specific controls */
	V4L2_CID_ADV7182_FREERUN_PAT_SEL,
	V4L2_CID_ADV7182_ACE_ENABLE,
	V4L2_CID_ADV7182_ACE_LUMA_GAIN,
	V4L2_CID_ADV7182_ACE_RESPONSE_SPEED,
	V4L2_CID_ADV7182_ACE_CHROMA_GAIN,
	V4L2_CID_ADV7182_ACE_CHROMA_MAX,
	V4L2_CID_ADV7182_ACE_GAMMA_GAIN,
	V4L2_CID_ADV7182_DITHER_ENABLE,
};

#endif
