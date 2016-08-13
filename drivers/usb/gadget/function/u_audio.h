/*
 * u_audio.h -- interface to USB gadget "ALSA sound card" utilities
 *
 * Copyright (C) 2016
 * Author: Ruslan Bilovol <ruslan.bilovol@gmail.com>
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

#ifndef __U_AUDIO_H
#define __U_AUDIO_H

#include <linux/usb/composite.h>

struct uac_params {
	/* playback */
	int p_chmask;	/* channel mask */
	int p_srate;	/* rate in Hz */
	int p_ssize;	/* sample size */

	/* capture */
	int c_chmask;	/* channel mask */
	int c_srate;	/* rate in Hz */
	int c_ssize;	/* sample size */
};

struct gaudio {
	struct usb_function func;
	struct usb_gadget *gadget;

	struct usb_ep *in_ep;
	struct usb_ep *out_ep;

	/* Max packet size for all in_ep possible speeds */
	unsigned int in_ep_maxpsize;
	/* Max packet size for all out_ep possible speeds */
	unsigned int out_ep_maxpsize;

	/* The ALSA Sound Card it represents on the USB-Client side */
	struct snd_uac_chip *uac;

	struct uac_params params;
};

static inline struct gaudio *func_to_gaudio(struct usb_function *f)
{
	return container_of(f, struct gaudio, func);
}

static inline uint num_channels(uint chanmask)
{
	uint num = 0;

	while (chanmask) {
		num += (chanmask & 1);
		chanmask >>= 1;
	}

	return num;
}

/*
 * gaudio_setup - initialize one virtual ALSA sound card
 * @gaudio: struct with filled params, in_ep_maxpsize, out_ep_maxpsize
 * @pcm_name: the id string for a PCM instance of this sound card
 * @card_name: name of this soundcard
 *
 * This sets up the single virtual ALSA sound card that may be exported by a
 * gadget driver using this framework.
 *
 * Context: may sleep
 *
 * Returns zero on success, or a negative error on failure.
 */
int gaudio_setup(struct gaudio *gaudio, const char *pcm_name,
					const char *card_name);
void gaudio_cleanup(struct gaudio *gaudio);

int gaudio_start_capture(struct gaudio *gaudio);
void gaudio_stop_capture(struct gaudio *gaudio);
int gaudio_start_playback(struct gaudio *gaudio);
void gaudio_stop_playback(struct gaudio *gaudio);

#endif /* __U_AUDIO_H */
