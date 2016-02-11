/*
 * v4l2-mc.h - Media Controller V4L2 types and prototypes
 *
 * Copyright (C) 2016 Mauro Carvalho Chehab <mchehab@osg.samsung.com>
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

#ifndef _V4L2_MC_H
#define _V4L2_MC_H

#include <media/v4l2-dev.h>

/**
 * enum tuner_pad_index - tuner pad index for MEDIA_ENT_F_TUNER
 *
 * @TUNER_PAD_RF_INPUT:	Radiofrequency (RF) sink pad, usually linked to a
 *			RF connector entity.
 * @TUNER_PAD_OUTPUT:	Tuner video output source pad. Contains the video
 *			chrominance and luminance or the hole bandwidth
 *			of the signal converted to an Intermediate Frequency
 *			(IF) or to baseband (on zero-IF tuners).
 * @TUNER_PAD_AUD_OUT:	Tuner audio output source pad. Tuners used to decode
 *			analog TV signals have an extra pad for audio output.
 *			Old tuners use an analog stage with a saw filter for
 *			the audio IF frequency. The output of the pad is, in
 *			this case, the audio IF, with should be decoded either
 *			by the bridge chipset (that's the case of cx2388x
 *			chipsets) or may require an external IF sound
 *			processor, like msp34xx. On modern silicon tuners,
 *			the audio IF decoder is usually incorporated at the
 *			tuner. On such case, the output of this pad is an
 *			audio sampled data.
 * @TUNER_NUM_PADS:	Number of pads of the tuner.
 */
enum tuner_pad_index {
	TUNER_PAD_RF_INPUT,
	TUNER_PAD_OUTPUT,
	TUNER_PAD_AUD_OUT,
	TUNER_NUM_PADS
};

/**
 * enum if_vid_dec_index - video IF-PLL pad index for
 *			   MEDIA_ENT_F_IF_VID_DECODER
 *
 * @IF_VID_DEC_PAD_IF_INPUT:	video Intermediate Frequency (IF) sink pad
 * @IF_VID_DEC_PAD_OUT:		IF-PLL video output source pad. Contains the
 *				video chrominance and luminance IF signals.
 * @IF_VID_DEC_PAD_NUM_PADS:	Number of pads of the video IF-PLL.
 */
enum if_vid_dec_pad_index {
	IF_VID_DEC_PAD_IF_INPUT,
	IF_VID_DEC_PAD_OUT,
	IF_VID_DEC_PAD_NUM_PADS
};

/**
 * enum if_aud_dec_index - audio/sound IF-PLL pad index for
 *			   MEDIA_ENT_F_IF_AUD_DECODER
 *
 * @IF_AUD_DEC_PAD_IF_INPUT:	audio Intermediate Frequency (IF) sink pad
 * @IF_AUD_DEC_PAD_OUT:		IF-PLL audio output source pad. Contains the
 *				audio sampled stream data, usually connected
 *				to the bridge bus via an Inter-IC Sound (I2S)
 *				bus.
 * @IF_AUD_DEC_PAD_NUM_PADS:	Number of pads of the audio IF-PLL.
 */
enum if_aud_dec_pad_index {
	IF_AUD_DEC_PAD_IF_INPUT,
	IF_AUD_DEC_PAD_OUT,
	IF_AUD_DEC_PAD_NUM_PADS
};

/**
 * enum demod_pad_index - analog TV pad index for MEDIA_ENT_F_ATV_DECODER
 *
 * @DEMOD_PAD_IF_INPUT:	IF input sink pad.
 * @DEMOD_PAD_VID_OUT:	Video output source pad.
 * @DEMOD_PAD_VBI_OUT:	Vertical Blank Interface (VBI) output source pad.
 * @DEMOD_NUM_PADS:	Maximum number of output pads.
 */
enum demod_pad_index {
	DEMOD_PAD_IF_INPUT,
	DEMOD_PAD_VID_OUT,
	DEMOD_PAD_VBI_OUT,
	DEMOD_NUM_PADS
};

/**
 * v4l_enable_media_source() -	Hold media source for exclusive use
 *				if free
 *
 * @vdev - poniter to struct video_device
 *
 * This interface calls enable_source handler to determine if
 * media source is free for use. The enable_source handler is
 * responsible for checking is the media source is free and
 * start a pipeline between the media source and the media
 * entity associated with the video device. This interface
 * should be called from v4l2-core and dvb-core interfaces
 * that change the source configuration.
 *
 * Return: returns zero on success or a negative error code.
 */
#ifdef CONFIG_MEDIA_CONTROLLER
int v4l_enable_media_source(struct video_device *vdev);
#else
static int v4l_enable_media_source(struct video_device *vdev) { return 0; }
#endif

/**
 * v4l_disable_media_source() -	Release media source
 *
 * @vdev - poniter to struct video_device
 *
 * This interface calls disable_source handler to release
 * the media source. The disable_source handler stops the
 * active media pipeline between the media source and the
 * media entity associated with the video device.
 *
 * Return: returns zero on success or a negative error code.
 */
#ifdef CONFIG_MEDIA_CONTROLLER
void v4l_disable_media_source(struct video_device *vdev);
#else
static void v4l_disable_media_source(struct video_device *vdev) { return; }
#endif

/*
 * v4l_vb2q_enable_media_tuner -  Hold media source for exclusive use
 *				  if free.
 * @q - pointer to struct vb2_queue
 *
 * Wrapper for v4l_enable_media_source(). This function should
 * be called from v4l2-core to enable the media source with
 * pointer to struct vb2_queue as the input argument. Some
 * v4l2-core interfaces don't have access to video device and
 * this interface finds the struct video_device for the q and
 * calls v4l_enable_media_source().
 */
#ifdef CONFIG_MEDIA_CONTROLLER
int v4l_vb2q_enable_media_source(struct vb2_queue *q);
#else
static int v4l_vb2q_enable_media_source(struct vb2_queue *q) { return 0; }
#endif

#endif /* _V4L2_MC_H */
