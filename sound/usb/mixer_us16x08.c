/*
 *   Tascam US-16x08 ALSA driver
 *
 *   Copyright (c) 2016 by Detlef Urban (onkel@paraair.de)
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 */

#include <linux/slab.h>
#include <linux/usb.h>
#include <linux/usb/audio-v2.h>
#include "linux/kthread.h"

#include <sound/core.h>
#include <sound/control.h>
#include <asm-generic/errno-base.h>

#include "usbaudio.h"
#include "mixer.h"
#include "helper.h"

#include "mixer_us16x08.h"

/* USB control message templates */
static char route_msg[] = {
	0x61,
	0x02,
	0x03, /* input from master (0x02) or input from computer bus (0x03) */
	0x62,
	0x02,
	0x01, /* input index (0x01/0x02 eq. left/right) or bus (0x01-0x08) */
	0x41,
	0x01,
	0x61,
	0x02,
	0x01,
	0x62,
	0x02,
	0x01, /* output index (0x01-0x08) */
	0x42,
	0x01,
	0x43,
	0x01,
	0x00,
	0x00
};

static char mix_init_msg1[] = {
	0x71, 0x01, 0x00, 0x00
};

static char mix_init_msg2[] = {
	0x62, 0x02, 0x00, 0x61, 0x02, 0x04, 0xb1, 0x01, 0x00, 0x00
};

static char mix_msg_in[] = {
	/* default message head, equal to all mixers */
	0x61, 0x02, 0x04, 0x62, 0x02, 0x01,
	0x81, /* 0x06: Controller ID */
	0x02, /* 0x07:  */
	0x00, /* 0x08: Value of common mixer */
	0x00,
	0x00
};

static char mix_msg_out[] = {
	/* default message head, equal to all mixers */
	0x61, 0x02, 0x02, 0x62, 0x02, 0x01,
	0x81, /* 0x06: Controller ID */
	0x02, /*                    0x07:  */
	0x00, /*                    0x08: Value of common mixer */
	0x00,
	0x00
};

static char bypass_msg_out[] = {
	0x45,
	0x02,
	0x01, /* on/off flag */
	0x00,
	0x00
};

static char bus_msg_out[] = {
	0x44,
	0x02,
	0x01, /* on/off flag */
	0x00,
	0x00
};

static char comp_msg[] = {
	/* default message head, equal to all mixers */
	0x61, 0x02, 0x04, 0x62, 0x02, 0x01,
	0x91,
	0x02,
	0xf0, /* 0x08: Threshold db (8) (e0 ... 00) (+-0dB -- -32dB) x-32 */
	0x92,
	0x02,
	0x0a, /* 0x0b: Ratio (0a,0b,0d,0f,11,14,19,1e,23,28,32,3c,50,a0,ff)  */
	0x93,
	0x02,
	0x02, /* 0x0e: Attack (0x02 ... 0xc0) (2ms ... 200ms) */
	0x94,
	0x02,
	0x01, /* 0x11: Release (0x01 ... 0x64) (10ms ... 1000ms) x*10  */
	0x95,
	0x02,
	0x03, /* 0x14: gain (0 ... 20) (0dB .. 20dB) */
	0x96,
	0x02,
	0x01,
	0x97,
	0x02,
	0x01, /* 0x1a: main Comp switch (0 ... 1) (off ... on)) */
	0x00,
	0x00
};

static char eqs_msq[] = {
	/* default message head, equal to all mixers */
	0x61, 0x02, 0x04, 0x62, 0x02, 0x01,
	0x51, /*                0x06: Controller ID  */
	0x02,
	0x04, /* 0x08: EQ set num (0x01..0x04) (LOW, LOWMID, HIGHMID, HIGH)) */
	0x52,
	0x02,
	0x0c, /* 0x0b: value dB (0 ... 12) (-12db .. +12db)  x-6 */
	0x53,
	0x02,
	0x0f, /* 0x0e: value freq (32-47) (1.7kHz..18kHz) */
	0x54,
	0x02,
	0x02, /* 0x11: band width (0-6) (Q16-Q0.25)  2^x/4 (EQ xxMID only) */
	0x55,
	0x02,
	0x01, /* 0x14: main EQ switch (0 ... 1) (off ... on)) */
	0x00,
	0x00
};

/* compressor ratio map */
static char ratio_map[] = {
	0x0a, 0x0b, 0x0d, 0x0f, 0x11, 0x14, 0x19, 0x1e,
	0x23, 0x28, 0x32, 0x3c, 0x50, 0xa0, 0xff
};

static int snd_us16x08_recv_urb(struct snd_usb_audio *chip,
	unsigned char *buf, int size)
{

	mutex_lock(&chip->mutex);
	snd_usb_ctl_msg(chip->dev,
		usb_rcvctrlpipe(chip->dev, 0),
		SND_US16X08_URB_METER_REQUEST,
		SND_US16X08_URB_METER_REQUESTTYPE, 0, 0, buf, size);
	mutex_unlock(&chip->mutex);
	return 0;
}

/* wrapper function to send prepared URB buffer to usb device. Return -1
 * if something went wrong
 */
static int snd_us16x08_send_urb(struct snd_usb_audio *chip, char *buf, int size)
{
	int count = -1;

	if (chip) {
		count = snd_usb_ctl_msg(chip->dev,
			usb_sndctrlpipe(chip->dev, 0),
			SND_US16X08_URB_REQUEST,
			SND_US16X08_URB_REQUESTTYPE,
			0, 0, buf, size);
	}

	return (count == size) ? 0 : count;
}

static int snd_us16x08_route_info(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_info *uinfo)
{
	uinfo->count = 1;
	uinfo->type = SNDRV_CTL_ELEM_TYPE_ENUMERATED;
	uinfo->value.integer.max = SND_US16X08_KCMAX(kcontrol);
	uinfo->value.integer.min = SND_US16X08_KCMIN(kcontrol);
	uinfo->value.integer.step = SND_US16X08_KCSTEP(kcontrol);
	return 0;
}

static int snd_us16x08_route_get(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct usb_mixer_elem_info *elem = kcontrol->private_data;
	struct snd_us16x08_common *store =
		(struct snd_us16x08_common *) elem->private_data;
	int index = ucontrol->id.index;

	/* route has no bias */
	ucontrol->value.integer.value[0] = store->value[index];

	return 0;
}

static int snd_us16x08_route_put(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct usb_mixer_elem_info *elem = kcontrol->private_data;
	struct snd_usb_audio *chip = elem->head.mixer->chip;
	struct snd_us16x08_common *store =
		(struct snd_us16x08_common *) elem->private_data;
	int index = ucontrol->id.index;
	char buf[sizeof(route_msg)];
	int val, val_org, err = 0;

	/* prepare the message buffer from template */
	memcpy(buf, route_msg, sizeof(route_msg));

	/*  get the new value (no bias for routes) */
	val = ucontrol->value.integer.value[0];
	if (val < 2) {
		/* input comes from a master channel */
		val_org = val;
		buf[2] = 0x02;
	} else {
		/* input comes from a computer channel */
		buf[2] = 0x03;
		val_org = val - 2;
	}

	/* place new route selection in URB message */
	buf[5] = (unsigned char) (val_org & 0x0f) + 1;
	/* place route selector in URB message */
	buf[13] = index + 1;

	err = snd_us16x08_send_urb(chip, buf, sizeof(route_msg));

	if (err == 0) {
		store->value[index] = val;
		elem->cached &= 1 << index;
		elem->cache_val[index] = val;
	}

	return err == 0 ? 1 : 0;
}

static int snd_us16x08_master_info(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_info *uinfo)
{
	uinfo->count = 1;
	uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	uinfo->value.integer.max = SND_US16X08_KCMAX(kcontrol);
	uinfo->value.integer.min = SND_US16X08_KCMIN(kcontrol);
	uinfo->value.integer.step = SND_US16X08_KCSTEP(kcontrol);
	return 0;
}

static int snd_us16x08_master_get(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct usb_mixer_elem_info *elem = kcontrol->private_data;
	struct snd_us16x08_common *store =
		(struct snd_us16x08_common *) elem->private_data;
	int index = ucontrol->id.index;

	ucontrol->value.integer.value[0] =
		store->value[index];

	return 0;
}

static int snd_us16x08_master_put(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct usb_mixer_elem_info *elem = kcontrol->private_data;
	struct snd_usb_audio *chip = elem->head.mixer->chip;
	struct snd_us16x08_common *store =
		(struct snd_us16x08_common *) elem->private_data;
	char buf[sizeof(mix_msg_out)];
	int val, err = 0;
	int index = ucontrol->id.index;

	/* prepare the message buffer from template */
	memcpy(buf, mix_msg_out, sizeof(mix_msg_out));

	/* new control value incl. bias*/
	val = ucontrol->value.integer.value[0];

	buf[8] = val - SND_US16X08_KCBIAS(kcontrol);
	buf[6] = elem->head.id;

	/* place channel selector in URB message */
	buf[5] = index + 1;
	err = snd_us16x08_send_urb(chip, buf, sizeof(mix_msg_out));

	if (err == 0) {
		store->value[index] = val;
		elem->cached &= 1 << index;
		elem->cache_val[index] = val;
	}

	return err == 0 ? 1 : 0;
}

static int snd_us16x08_bus_put(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct usb_mixer_elem_info *elem = kcontrol->private_data;
	struct snd_usb_audio *chip = elem->head.mixer->chip;
	struct snd_us16x08_bus_store *store =
		(struct snd_us16x08_bus_store *) elem->private_data;

	char buf[sizeof(mix_msg_out)];
	int val, err = 0;

	val = ucontrol->value.integer.value[0];

	/* prepare the message buffer from template */
	switch (elem->head.id) {
	case SND_US16X08_ID_BYPASS:
		memcpy(buf, bypass_msg_out, sizeof(bypass_msg_out));
		buf[2] = val;
		err = snd_us16x08_send_urb(chip, buf, sizeof(bypass_msg_out));
		store->bypass[0] = val;
		break;
	case SND_US16X08_ID_BUSS_OUT:
		memcpy(buf, bus_msg_out, sizeof(bus_msg_out));
		buf[2] = val;
		err = snd_us16x08_send_urb(chip, buf, sizeof(bus_msg_out));
		store->bus_out[0] = val;
		break;
	case SND_US16X08_ID_MUTE:
		memcpy(buf, mix_msg_out, sizeof(mix_msg_out));
		buf[8] = val;
		buf[6] = elem->head.id;
		buf[5] = 1;
		err = snd_us16x08_send_urb(chip, buf, sizeof(mix_msg_out));
		store->master_mute[0] = val;
		break;
	}

	if (err == 0) {
		elem->cached &= 1;
		elem->cache_val[0] = val;
	}

	return err == 0 ? 1 : 0;
}

static int snd_us16x08_bus_get(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct usb_mixer_elem_info *elem = kcontrol->private_data;
	struct snd_us16x08_bus_store *store =
		(struct snd_us16x08_bus_store *) elem->private_data;

	switch (elem->head.id) {
	case SND_US16X08_ID_BUSS_OUT:
		ucontrol->value.integer.value[0] = store->bus_out[0];
		break;
	case SND_US16X08_ID_BYPASS:
		ucontrol->value.integer.value[0] = store->bypass[0];
		break;
	case SND_US16X08_ID_MUTE:
		ucontrol->value.integer.value[0] = store->master_mute[0];
		break;
	}

	return 0;
}

/* gets a current mixer value from common store */
static int snd_us16x08_channel_get(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct usb_mixer_elem_info *elem = kcontrol->private_data;
	struct snd_us16x08_channel_store *store =
		(struct snd_us16x08_channel_store *) elem->private_data;
	int index = ucontrol->id.index;

	switch (elem->head.id) {
	case SND_US16X08_ID_MUTE:
		ucontrol->value.integer.value[0] = store->mute[index];
		break;
	case SND_US16X08_ID_PAN:
		ucontrol->value.integer.value[0] = store->pan[index];
		break;
	case SND_US16X08_ID_FADER:
		ucontrol->value.integer.value[0] = store->gain[index];
		break;
	case SND_US16X08_ID_PHASE:
		ucontrol->value.integer.value[0] = store->phase[index];
		break;
	}

	return 0;
}

static int snd_us16x08_channel_put(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct usb_mixer_elem_info *elem = kcontrol->private_data;
	struct snd_usb_audio *chip = elem->head.mixer->chip;
	struct snd_us16x08_channel_store *store =
		(struct snd_us16x08_channel_store *) elem->private_data;
	char buf[sizeof(mix_msg_in)];
	int val, err;
	int index = ucontrol->id.index;

	/* prepare URB message from template */
	memcpy(buf, mix_msg_in, sizeof(mix_msg_in));

	val = ucontrol->value.integer.value[0];

	/* add the bias to the new value */
	buf[8] = val - SND_US16X08_KCBIAS(kcontrol);
	buf[6] = elem->head.id;
	buf[5] = index + 1;

	err = snd_us16x08_send_urb(chip, buf, sizeof(mix_msg_in));

	if (err == 0) {
		switch (elem->head.id) {
		case SND_US16X08_ID_MUTE:
			store->mute[index] = val;
			break;
		case SND_US16X08_ID_PAN:
			store->pan[index] = val;
			break;
		case SND_US16X08_ID_FADER:
			store->gain[index] = val;
			break;
		case SND_US16X08_ID_PHASE:
			store->phase[index] = val;
			break;
		}
		elem->cached &= 1 << index;
		elem->cache_val[index] = val;
	}

	return err == 0 ? 1 : 0;
}

static int snd_us16x08_mix_info(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_info *uinfo)
{
	uinfo->count = 1;
	uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	uinfo->value.integer.max = SND_US16X08_KCMAX(kcontrol);
	uinfo->value.integer.min = SND_US16X08_KCMIN(kcontrol);
	uinfo->value.integer.step = SND_US16X08_KCSTEP(kcontrol);
	return 0;
}

static int snd_us16x08_comp_get(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	int val = 0;
	struct usb_mixer_elem_info *elem = kcontrol->private_data;
	struct snd_us16x08_comp_store *store =
		((struct snd_us16x08_comp_store *) elem->private_data);
	int index = ucontrol->id.index;

	switch (elem->head.id) {
	case SND_US16X08_ID_COMP_THRESHOLD:
		val = store->valThreshold[index];
		break;
	case SND_US16X08_ID_COMP_RATIO:
		val = store->valRatio[index];
		break;
	case SND_US16X08_ID_COMP_ATTACK:
		val = store->valAttack[index];
		break;
	case SND_US16X08_ID_COMP_RELEASE:
		val = store->valRelease[index];
		break;
	case SND_US16X08_ID_COMP_GAIN:
		val = store->valGain[index];
		break;
	case SND_US16X08_ID_COMP_SWITCH:
		val = store->valSwitch[index];
		break;
	}
	ucontrol->value.integer.value[0] = val;

	return 0;
}

static int snd_us16x08_comp_put(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct usb_mixer_elem_info *elem = kcontrol->private_data;
	struct snd_usb_audio *chip = elem->head.mixer->chip;
	struct snd_us16x08_comp_store *store =
		((struct snd_us16x08_comp_store *) elem->private_data);
	int index = ucontrol->id.index;

	char buf[sizeof(comp_msg)];
	int val, err = 0;

	/* prepare compressor URB message from template  */
	memcpy(buf, comp_msg, sizeof(comp_msg));

	/* new control value incl. bias*/
	val = ucontrol->value.integer.value[0];
	switch (elem->head.id) {
	case SND_US16X08_ID_COMP_THRESHOLD:
		store->valThreshold[index] = val;
		break;
	case SND_US16X08_ID_COMP_RATIO:
		store->valRatio[index] = val;
		break;
	case SND_US16X08_ID_COMP_ATTACK:
		store->valAttack[index] = val;
		break;
	case SND_US16X08_ID_COMP_RELEASE:
		store->valRelease[index] = val;
		break;
	case SND_US16X08_ID_COMP_GAIN:
		store->valGain[index] = val;
		break;
	case SND_US16X08_ID_COMP_SWITCH:
		store->valSwitch[index] = val;
		break;
	}

	/* place comp values in message buffer watch bias! */
	buf[8] = store->valThreshold[index] - SND_US16X08_COMP_THRESHOLD_BIAS;
	buf[11] = ratio_map[store->valRatio[index]];
	buf[14] = store->valAttack[index] + SND_US16X08_COMP_ATTACK_BIAS;
	buf[17] = store->valRelease[index] + SND_US16X08_COMP_RELEASE_BIAS;
	buf[20] = store->valGain[index];
	buf[26] = store->valSwitch[index];

	/* place channel selector in message buffer */
	buf[5] = index + 1;

	err = snd_us16x08_send_urb(chip, buf, sizeof(comp_msg));

	if (err == 0) {
		elem->cached &= 1 << index;
		elem->cache_val[index] = val;
	}

	return 1;
}

static int snd_us16x08_eqswitch_get(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	int val = 0;
	struct usb_mixer_elem_info *elem = kcontrol->private_data;
	struct snd_us16x08_eq_all_store *store =
		((struct snd_us16x08_eq_all_store *) elem->private_data);
	int index = ucontrol->id.index;

	/* get low switch from cache is enough, cause all bands are together */
	val = store->low_store->valSwitch[index];
	ucontrol->value.integer.value[0] = val;

	return 0;
}

static int snd_us16x08_eqswitch_put(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct usb_mixer_elem_info *elem = kcontrol->private_data;
	struct snd_usb_audio *chip = elem->head.mixer->chip;
	struct snd_us16x08_eq_all_store *store =
		((struct snd_us16x08_eq_all_store *) elem->private_data);
	int index = ucontrol->id.index;

	char buf[sizeof(eqs_msq)];
	int val, err = 0;

	/* new control value incl. bias*/
	val = ucontrol->value.integer.value[0] + SND_US16X08_KCBIAS(kcontrol);

	/* prepare URB message from EQ template */
	memcpy(buf, eqs_msq, sizeof(eqs_msq));

	store->low_store->valSwitch[index] = val;
	store->midlow_store->valSwitch[index] = val;
	store->midhigh_store->valSwitch[index] = val;
	store->high_store->valSwitch[index] = val;

	/* place channel index in URB message */
	buf[5] = index + 1;

	/* all four EQ bands have to be enabled/disabled in once */
	buf[20] = store->low_store->valSwitch[index];
	buf[17] = store->low_store->valWidth[index];
	buf[14] = store->low_store->valFreq[index];
	buf[11] = store->low_store->valdB[index];
	buf[8] = 0x01;
	err = snd_us16x08_send_urb(chip, buf, sizeof(eqs_msq));

	/* give time to the device to handle the request */
	mdelay(15);
	buf[20] = store->midlow_store->valSwitch[index];
	buf[17] = store->midlow_store->valWidth[index];
	buf[14] = store->midlow_store->valFreq[index];
	buf[11] = store->midlow_store->valdB[index];
	buf[8] = 0x02;
	err = snd_us16x08_send_urb(chip, buf, sizeof(eqs_msq));

	mdelay(15);
	buf[20] = store->midhigh_store->valSwitch[index];
	buf[17] = store->midhigh_store->valWidth[index];
	buf[14] = store->midhigh_store->valFreq[index];
	buf[11] = store->midhigh_store->valdB[index];
	buf[8] = 0x03;
	err = snd_us16x08_send_urb(chip, buf, sizeof(eqs_msq));

	mdelay(15);
	buf[20] = store->high_store->valSwitch[index];
	buf[17] = store->high_store->valWidth[index];
	buf[14] = store->high_store->valFreq[index] +
		SND_US16X08_EQ_HIGHFREQ_BIAS;
	buf[11] = store->high_store->valdB[index];
	buf[8] = 0x04;
	err = snd_us16x08_send_urb(chip, buf, sizeof(eqs_msq));

	if (err == 0) {
		store->low_store->valSwitch[index] = val;
		elem->cached &= 1 << index;
		elem->cache_val[index] = val;
	}

	return 1;
}

static int snd_us16x08_eq_get(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	int val = 0;
	struct usb_mixer_elem_info *elem = kcontrol->private_data;
	struct snd_us16x08_eq_store *store =
		((struct snd_us16x08_eq_store *) elem->private_data);
	int index = ucontrol->id.index;

	switch (elem->head.id & 0xf0) {
	case 0x00:
		val = store->valdB[index];
		break;
	case 0x10:
		val = store->valFreq[index];
		break;
	case 0x20:
		val = store->valWidth[index];
		break;
	}
	ucontrol->value.integer.value[0] = val;

	return 0;
}

static int snd_us16x08_eq_put(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct usb_mixer_elem_info *elem = kcontrol->private_data;
	struct snd_usb_audio *chip = elem->head.mixer->chip;
	struct snd_us16x08_eq_store *store =
		((struct snd_us16x08_eq_store *) elem->private_data);
	int index = ucontrol->id.index;

	char buf[sizeof(eqs_msq)];
	int val, err = 0;

	/* copy URB buffer from EQ template */
	memcpy(buf, eqs_msq, sizeof(eqs_msq));

	/* add control bias to new value */
	val = ucontrol->value.integer.value[0];

	switch (elem->head.id & 0xf0) {
	case 0x00: /* level dB */
		store->valdB[index] = val;
		break;
	case 0x10:
		store->valFreq[index] = val;
		break;
	case 0x20:
		store->valWidth[index] = val;
		break;
	}

	buf[20] = store->valSwitch[index];
	buf[17] = store->valWidth[index];
	/* add eq high frequence bias if high band changed*/
	buf[14] = store->valFreq[index] +
		((elem->head.id & 0x0f) == 0x04 ?
		SND_US16X08_EQ_HIGHFREQ_BIAS : SND_US16X08_NO_BIAS);
	buf[11] = store->valdB[index];

	/* place channel index in URB buffer */
	buf[5] = index + 1;

	/* place EQ band in URB buffer */
	buf[8] = (elem->head.id & 0x0F);

	err = snd_us16x08_send_urb(chip, buf, sizeof(eqs_msq));

	if (err == 0) {
		/* store new value in EQ band cache */
		elem->cached &= 1 << index;
		elem->cache_val[index] = val;
	}
	return 1;
}

static int snd_us16x08_meter_info(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_info *uinfo)
{
	uinfo->count = 1;
	uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	uinfo->value.integer.max = 0x7FFF;
	uinfo->value.integer.min = 0;

	return 0;
}

/* calculate compressor index for reduction level request */
static int snd_get_meter_comp_index(struct snd_us16x08_meter_store *store)
{
	int ret;

	/* any channel active */
	if (store->comp_active_index) {
		/* check for stereo link */
		if (store->comp_active_index & 0x20) {
			/* reset comp_index to left channel*/
			if (store->comp_index -
				store->comp_active_index > 1)
				store->comp_index =
				store->comp_active_index;

			ret = store->comp_index++ & 0x1F;
		} else {
			/* no stereo link */
			ret = store->comp_active_index;
		}
	} else {
		/* skip channels with no compressor active */
		while (!store->comp_store->valSwitch[store->comp_index - 1] &&
			store->comp_index < 17) {
			store->comp_index++;
		}
		ret = store->comp_index++;
		if (store->comp_index > 16)
			store->comp_index = 1;
	}
	return ret;
}

/* retrieve the meter level values from URB message */
static void get_meter_levels_from_urb(int s,
	struct snd_us16x08_meter_store *store,
	u8 *meter_urb)
{
	int val = MUC2(meter_urb, s) + (MUC3(meter_urb, s) << 8);

	if (MUA0(meter_urb, s) == 0x61 && MUA1(meter_urb, s) == 0x02 &&
		MUA2(meter_urb, s) == 0x04 && MUB0(meter_urb, s) == 0x62) {
		if (MUC0(meter_urb, s) == 0x72)
			store->meter_level[MUB2(meter_urb, s) - 1] = val;
		if (MUC0(meter_urb, s) == 0xb2)
			store->comp_level[MUB2(meter_urb, s) - 1] = val;
	}
	if (MUA0(meter_urb, s) == 0x61 && MUA1(meter_urb, s) == 0x02 &&
		MUA2(meter_urb, s) == 0x02 && MUB0(meter_urb, s) == 0x62)
		store->master_level[MUB2(meter_urb, s) - 1] = val;
}

/* Function to retrieve current meter values from the device.
 *
 * The device needs to be polled for meter values with an initial
 * requests. It will return with a sequence of different meter value
 * packages. The first request (case 0:) initiate this meter response sequence.
 * After the third response, an additional request can be placed,
 * to retrieve compressor reduction level value for given channel. This round
 * trip channel selector will skip all inactive compressors.
 * A mixer can interrupt this round-trip by selecting one ore two (stereo-link)
 * specific channels.
 */
static int snd_us16x08_meter_get(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	int i, set;
	struct usb_mixer_elem_info *elem = kcontrol->private_data;
	struct snd_usb_audio *chip = elem->head.mixer->chip;
	struct snd_us16x08_meter_store *store = elem->private_data;
	u8 meter_urb[64];


	if (elem) {
		store = (struct snd_us16x08_meter_store *) elem->private_data;
		chip = elem->head.mixer->chip;
	} else
		return 0;

	switch (kcontrol->private_value) {
	case 0:
		snd_us16x08_send_urb(chip, mix_init_msg1, 4);
		snd_us16x08_recv_urb(chip, meter_urb,
			sizeof(meter_urb));
		kcontrol->private_value++;
		break;
	case 1:
		snd_us16x08_recv_urb(chip, meter_urb,
			sizeof(meter_urb));
		kcontrol->private_value++;
		break;
	case 2:
		snd_us16x08_recv_urb(chip, meter_urb,
			sizeof(meter_urb));
		kcontrol->private_value++;
		break;
	case 3:
		mix_init_msg2[2] = snd_get_meter_comp_index(store);
		snd_us16x08_send_urb(chip, mix_init_msg2, 10);
		snd_us16x08_recv_urb(chip, meter_urb,
			sizeof(meter_urb));
		kcontrol->private_value = 0;
		break;
	}

	for (set = 0; set < 6; set++)
		get_meter_levels_from_urb(set, store, meter_urb);

	for (i = 0; i < SND_US16X08_MAX_CHANNELS; i++) {
		ucontrol->value.integer.value[i] =
			store ? store->meter_level[i] : 0;
	}

	ucontrol->value.integer.value[i++] = store ? store->master_level[0] : 0;
	ucontrol->value.integer.value[i++] = store ? store->master_level[1] : 0;

	for (i = 2; i < SND_US16X08_MAX_CHANNELS + 2; i++)
		ucontrol->value.integer.value[i + SND_US16X08_MAX_CHANNELS] =
		store ? store->comp_level[i - 2] : 0;

	return 1;
}

static int snd_us16x08_meter_put(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct usb_mixer_elem_info *elem = kcontrol->private_data;
	struct snd_us16x08_meter_store *store = elem->private_data;
	int val;

	val = ucontrol->value.integer.value[0];
	store->comp_active_index = val;
	store->comp_index = val;

	return 1;
}

static struct snd_kcontrol_new snd_us16x08_ch_boolean_ctl = {
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.access = SNDRV_CTL_ELEM_ACCESS_READWRITE,
	.count = 16,
	.info = snd_us16x08_switch_info,
	.get = snd_us16x08_channel_get,
	.put = snd_us16x08_channel_put,
	.private_value = SND_US16X08_KCSET(SND_US16X08_NO_BIAS, 1, 0, 1)
};

static struct snd_kcontrol_new snd_us16x08_ch_int_ctl = {
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.access = SNDRV_CTL_ELEM_ACCESS_READWRITE,
	.count = 16,
	.info = snd_us16x08_mix_info,
	.get = snd_us16x08_channel_get,
	.put = snd_us16x08_channel_put,
	.private_value = SND_US16X08_KCSET(SND_US16X08_FADER_BIAS, 1, 0, 133)
};

static struct snd_kcontrol_new snd_us16x08_master_ctl = {
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.access = SNDRV_CTL_ELEM_ACCESS_READWRITE,
	.count = 1,
	.info = snd_us16x08_master_info,
	.get = snd_us16x08_master_get,
	.put = snd_us16x08_master_put,
	.private_value = SND_US16X08_KCSET(SND_US16X08_FADER_BIAS, 1, 0, 133)
};

static struct snd_kcontrol_new snd_us16x08_route_ctl = {
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.access = SNDRV_CTL_ELEM_ACCESS_READWRITE,
	.count = 8,
	.info = snd_us16x08_route_info,
	.get = snd_us16x08_route_get,
	.put = snd_us16x08_route_put,
	.private_value = SND_US16X08_KCSET(SND_US16X08_NO_BIAS, 1, 0, 9)
};

static struct snd_kcontrol_new snd_us16x08_bus_ctl = {
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.access = SNDRV_CTL_ELEM_ACCESS_READWRITE,
	.count = 1,
	.info = snd_us16x08_switch_info,
	.get = snd_us16x08_bus_get,
	.put = snd_us16x08_bus_put,
	.private_value = SND_US16X08_KCSET(SND_US16X08_NO_BIAS, 1, 0, 1)
};

static struct snd_kcontrol_new snd_us16x08_compswitch_ctl = {
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.access = SNDRV_CTL_ELEM_ACCESS_READWRITE,
	.count = 16,
	.info = snd_us16x08_switch_info,
	.get = snd_us16x08_comp_get,
	.put = snd_us16x08_comp_put,
	.private_value = SND_US16X08_KCSET(SND_US16X08_NO_BIAS, 1, 0, 1)
};

static struct snd_kcontrol_new snd_us16x08_comp_threshold_ctl = {
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.access = SNDRV_CTL_ELEM_ACCESS_READWRITE,
	.count = 16,
	.info = snd_us16x08_mix_info,
	.get = snd_us16x08_comp_get,
	.put = snd_us16x08_comp_put,
	.private_value = SND_US16X08_KCSET(SND_US16X08_COMP_THRESHOLD_BIAS, 1,
	0, 0x20)
};

static struct snd_kcontrol_new snd_us16x08_comp_ratio_ctl = {
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.access = SNDRV_CTL_ELEM_ACCESS_READWRITE,
	.count = 16,
	.info = snd_us16x08_mix_info,
	.get = snd_us16x08_comp_get,
	.put = snd_us16x08_comp_put,
	.private_value = SND_US16X08_KCSET(SND_US16X08_NO_BIAS, 1, 0,
	sizeof(ratio_map) - 1), /*max*/
};

static struct snd_kcontrol_new snd_us16x08_comp_gain_ctl = {
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.access = SNDRV_CTL_ELEM_ACCESS_READWRITE,
	.count = 16,
	.info = snd_us16x08_mix_info,
	.get = snd_us16x08_comp_get,
	.put = snd_us16x08_comp_put,
	.private_value = SND_US16X08_KCSET(SND_US16X08_NO_BIAS, 1, 0, 0x14)
};

static struct snd_kcontrol_new snd_us16x08_comp_attack_ctl = {
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.access = SNDRV_CTL_ELEM_ACCESS_READWRITE,
	.count = 16,
	.info = snd_us16x08_mix_info,
	.get = snd_us16x08_comp_get,
	.put = snd_us16x08_comp_put,
	.private_value =
	SND_US16X08_KCSET(SND_US16X08_COMP_ATTACK_BIAS, 1, 0, 0xc6),
};

static struct snd_kcontrol_new snd_us16x08_comp_release_ctl = {
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.access = SNDRV_CTL_ELEM_ACCESS_READWRITE,
	.count = 16,
	.info = snd_us16x08_mix_info,
	.get = snd_us16x08_comp_get,
	.put = snd_us16x08_comp_put,
	.private_value =
	SND_US16X08_KCSET(SND_US16X08_COMP_RELEASE_BIAS, 1, 0, 0x63),
};

static struct snd_kcontrol_new snd_us16x08_eq_gain_ctl = {
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.access = SNDRV_CTL_ELEM_ACCESS_READWRITE,
	.count = 16,
	.info = snd_us16x08_mix_info,
	.get = snd_us16x08_eq_get,
	.put = snd_us16x08_eq_put,
	.private_value = SND_US16X08_KCSET(SND_US16X08_NO_BIAS, 1, 0, 24),
};

static struct snd_kcontrol_new snd_us16x08_eq_low_freq_ctl = {
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.access = SNDRV_CTL_ELEM_ACCESS_READWRITE,
	.count = 16,
	.info = snd_us16x08_mix_info,
	.get = snd_us16x08_eq_get,
	.put = snd_us16x08_eq_put,
	.private_value = SND_US16X08_KCSET(SND_US16X08_NO_BIAS, 1, 0, 0x1F),
};

static struct snd_kcontrol_new snd_us16x08_eq_mid_freq_ctl = {
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.access = SNDRV_CTL_ELEM_ACCESS_READWRITE,
	.count = 16,
	.info = snd_us16x08_mix_info,
	.get = snd_us16x08_eq_get,
	.put = snd_us16x08_eq_put,
	.private_value = SND_US16X08_KCSET(SND_US16X08_NO_BIAS, 1, 0, 0x3F)
};

static struct snd_kcontrol_new snd_us16x08_eq_mid_width_ctl = {
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.access = SNDRV_CTL_ELEM_ACCESS_READWRITE,
	.count = 16,
	.info = snd_us16x08_mix_info,
	.get = snd_us16x08_eq_get,
	.put = snd_us16x08_eq_put,
	.private_value = SND_US16X08_KCSET(SND_US16X08_NO_BIAS, 1, 0, 0x06)
};

static struct snd_kcontrol_new snd_us16x08_eq_high_freq_ctl = {
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.access = SNDRV_CTL_ELEM_ACCESS_READWRITE,
	.count = 16,
	.info = snd_us16x08_mix_info,
	.get = snd_us16x08_eq_get,
	.put = snd_us16x08_eq_put,
	.private_value =
	SND_US16X08_KCSET(SND_US16X08_EQ_HIGHFREQ_BIAS, 1, 0, 0x1F)
};

static struct snd_kcontrol_new snd_us16x08_eq_switch_ctl = {
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.access = SNDRV_CTL_ELEM_ACCESS_READWRITE,
	.count = 16,
	.info = snd_us16x08_switch_info,
	.get = snd_us16x08_eqswitch_get,
	.put = snd_us16x08_eqswitch_put,
	.private_value = SND_US16X08_KCSET(SND_US16X08_NO_BIAS, 1, 0, 1)
};

static struct snd_kcontrol_new snd_us16x08_meter_ctl = {
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.access = SNDRV_CTL_ELEM_ACCESS_READWRITE,
	.count = 1,
	.info = snd_us16x08_meter_info,
	.get = snd_us16x08_meter_get,
	.put = snd_us16x08_meter_put
};

/* control store preparation */

static struct snd_us16x08_common *snd_us16x08_create_mix_store(int default_val)
{
	int i;
	struct snd_us16x08_common *tmp =
		kmalloc(sizeof(struct snd_us16x08_common), GFP_KERNEL);

	if (tmp == NULL)
		return NULL;

	for (i = 0; i < SND_US16X08_MAX_CHANNELS; i++)
		tmp->value[i] = default_val;
	return tmp;
}

static struct snd_us16x08_channel_store *snd_us16x08_create_channel_store(void)
{
	int i;
	struct snd_us16x08_channel_store *tmp =
		kmalloc(sizeof(struct snd_us16x08_channel_store), GFP_KERNEL);

	if (tmp == NULL)
		return NULL;

	for (i = 0; i < SND_US16X08_MAX_CHANNELS; i++) {
		tmp->gain[i] = 127; /* 0dB */
		tmp->mute[i] = 0;
		tmp->pan[i] = 127; /* center */
		tmp->phase[i] = 0;
	}
	return tmp;
}

static struct snd_us16x08_comp_store *snd_us16x08_create_comp_store(void)
{
	int i = 0;
	struct snd_us16x08_comp_store *tmp =
		kmalloc(sizeof(struct snd_us16x08_comp_store), GFP_KERNEL);

	if (tmp == NULL)
		return NULL;

	for (i = 0; i < SND_US16X08_MAX_CHANNELS; i++) {
		tmp->valThreshold[i] = 0x20; /*     0dB */
		tmp->valRatio[i] = 0x00; /*         1:1 */
		tmp->valGain[i] = 0x00; /*          0dB */
		tmp->valSwitch[i] = 0x00; /*        off */
		tmp->valAttack[i] = 0x00; /*        2ms */
		tmp->valRelease[i] = 0x00; /*       10ms */
	}
	return tmp;
}

static struct snd_us16x08_common *snd_us16x08_create_route_store(void)
{
	int i = 0;
	struct snd_us16x08_common *tmp =
		kmalloc(sizeof(struct snd_us16x08_common), GFP_KERNEL);

	if (tmp == NULL)
		return NULL;

	for (i = 0; i < 8; i++)
		tmp->value[i] = i < 2 ? i : i + 2;
	return tmp;
}

/* setup compressor store and assign default value */
static struct snd_us16x08_bus_store *snd_us16x08_create_bus_store(
	int default_val)
{
	struct snd_us16x08_bus_store *tmp =
		kmalloc(sizeof(struct snd_us16x08_bus_store), GFP_KERNEL);

	if (tmp == NULL)
		return NULL;

	tmp->bypass[0] = default_val;
	tmp->bus_out[0] = default_val;
	tmp->master_mute[0] = default_val;
	return tmp;
}

/* setup EQ store and assign default values */
static struct snd_us16x08_eq_store *snd_us16x08_create_eq_store(int band_index)
{
	int i = 0;
	struct snd_us16x08_eq_store *tmp =
		kmalloc(sizeof(struct snd_us16x08_eq_store), GFP_KERNEL);

	if (tmp == NULL)
		return NULL;

	for (i = 0; i < SND_US16X08_MAX_CHANNELS; i++) {
		switch (band_index) {
		case 0x01: /* EQ Low */
			tmp->valdB[i] = 0x0c;
			tmp->valFreq[i] = 0x05;
			tmp->valWidth[i] = 0xff;
			tmp->valSwitch[i] = 0x00; /* off */
			break;
		case 0x02: /* EQ Mid low */
			tmp->valdB[i] = 0x0c;
			tmp->valFreq[i] = 0x0e;
			tmp->valWidth[i] = 0x02;
			tmp->valSwitch[i] = 0x00; /* off */
			break;
		case 0x03: /* EQ Mid High */
			tmp->valdB[i] = 0x0c;
			tmp->valFreq[i] = 0x1b;
			tmp->valWidth[i] = 0x02;
			tmp->valSwitch[i] = 0x00; /* off */
			break;
		case 0x04: /* EQ High */
			tmp->valdB[i] = 0x0c;
			tmp->valFreq[i] = 0x2f - SND_US16X08_EQ_HIGHFREQ_BIAS;
			tmp->valWidth[i] = 0xff;
			tmp->valSwitch[i] = 0x00; /* off */
			break;
		}
	}
	return tmp;
}

struct snd_us16x08_meter_store *snd_us16x08_create_meter_store(void)
{
	struct snd_us16x08_meter_store *tmp =
		kzalloc(sizeof(struct snd_us16x08_meter_store), GFP_KERNEL);

	if (!tmp)
		return NULL;
	tmp->comp_index = 1;
	tmp->comp_active_index = 0;
	return tmp;

}

/* suspend/resume */

static void snd_us16x08_resume_route(struct usb_mixer_elem_info *elem)
{
	int i;
	struct snd_us16x08_common *store =
		(struct snd_us16x08_common *) elem->private_data;

	for (i = 0; i < elem->channels; i++)
		if (elem->cached & (1 << i))
			store->value[i] = elem->cache_val[i];
}

static void snd_us16x08_resume_master(struct usb_mixer_elem_info *elem)
{
	struct snd_us16x08_common *store =
		(struct snd_us16x08_common *) elem->private_data;

	if (elem->channels == 1 && elem->cached & 1)
		store->value[0] = elem->cache_val[0];
}

static void snd_us16x08_resume_bus(struct usb_mixer_elem_info *elem)
{
	struct snd_us16x08_bus_store *store =
		(struct snd_us16x08_bus_store *) elem->private_data;

	if (elem->channels == 1 && elem->cached & 1)
		switch (elem->head.id) {
		case SND_US16X08_ID_BYPASS:
			store->bypass[0] = elem->cache_val[0];
			break;
		case SND_US16X08_ID_BUSS_OUT:
			store->bus_out[0] = elem->cache_val[0];
			break;
		case SND_US16X08_ID_MUTE:
			store->master_mute[0] = elem->cache_val[0];
			break;
		}
}

static void snd_us16x08_resume_channel(struct usb_mixer_elem_info *elem)
{
	int i;
	struct snd_us16x08_channel_store *store =
		(struct snd_us16x08_channel_store *) elem->private_data;

	for (i = 0; i < elem->channels; i++)
		if (elem->cached & (1 << i))
			switch (elem->head.id) {
			case SND_US16X08_ID_PAN:
				store->pan[i] = elem->cache_val[i];
				break;
			case SND_US16X08_ID_FADER:
				if (elem->channels > 1)
					store->gain[i] = elem->cache_val[i];
				break;
			case SND_US16X08_ID_PHASE:
				store->phase[i] = elem->cache_val[i];
				break;
			case SND_US16X08_ID_MUTE:
				if (elem->channels > 1)
					store->mute[i] = elem->cache_val[i];
				break;
			}
}

static void snd_us16x08_resume_eq(struct usb_mixer_elem_info *elem)
{
	int i;
	int val;
	struct snd_us16x08_eq_all_store *store =
		(struct snd_us16x08_eq_all_store *) elem->private_data;

	for (i = 0; i < elem->channels; i++) {
		if (elem->cached & (1 << i)) {
			val = elem->cache_val[i];
			switch (elem->head.id) {
			case SND_US16X08_ID_EQLOWLEVEL:
				store->low_store->valdB[i] = val;
				break;
			case SND_US16X08_ID_EQLOWMIDLEVEL:
				store->midlow_store->valdB[i] = val;
				break;
			case SND_US16X08_ID_EQHIGHMIDLEVEL:
				store->midhigh_store->valdB[i] = val;
				break;
			case SND_US16X08_ID_EQHIGHLEVEL:
				store->high_store->valdB[i] = val;
				break;
			case SND_US16X08_ID_EQLOWFREQ:
				store->low_store->valFreq[i] = val;
				break;
			case SND_US16X08_ID_EQLOWMIDFREQ:
				store->midlow_store->valFreq[i] = val;
				break;
			case SND_US16X08_ID_EQHIGHMIDFREQ:
				store->midhigh_store->valFreq[i] = val;
				break;
			case SND_US16X08_ID_EQHIGHFREQ:
				store->high_store->valFreq[i] = val;
				break;
			case SND_US16X08_ID_EQLOWMIDWIDTH:
				store->midlow_store->valWidth[i] = val;
				break;
			case SND_US16X08_ID_EQHIGHMIDWIDTH:
				store->midhigh_store->valWidth[i] = val;
				break;
			case SND_US16X08_ID_EQENABLE:
				store->low_store->valSwitch[i] = val;
				store->midlow_store->valSwitch[i] = val;
				store->midhigh_store->valSwitch[i] = val;
				store->high_store->valSwitch[i] = val;
				break;
			}
		}
	}
}

static void snd_us16x08_resume_comp(struct usb_mixer_elem_info *elem)
{
	int i;
	int val;
	struct snd_us16x08_comp_store *store =
		(struct snd_us16x08_comp_store *) elem->private_data;

	for (i = 0; i < elem->channels; i++) {
		if (elem->cached & (1 << i)) {
			val = elem->cache_val[i];
			switch (elem->head.id) {
			case SND_US16X08_ID_COMP_THRESHOLD:
				store->valThreshold[i] = val;
				break;
			case SND_US16X08_ID_COMP_RATIO:
				store->valRatio[i] = val;
				break;
			case SND_US16X08_ID_COMP_ATTACK:
				store->valAttack[i] = val;
				break;
			case SND_US16X08_ID_COMP_RELEASE:
				store->valRelease[i] = val;
				break;
			case SND_US16X08_ID_COMP_GAIN:
				store->valGain[i] = val;
				break;
			case SND_US16X08_ID_COMP_SWITCH:
				store->valSwitch[i] = val;
				break;
			}
		}
	}
}

static int snd_us16x08_resume(struct usb_mixer_elem_list *list)
{
	struct usb_mixer_elem_info *elem =
		container_of(list, struct usb_mixer_elem_info, head);

	/* restore common mixer values */
	if (elem->head.id == SND_US16X08_ID_PAN ||
		elem->head.id == SND_US16X08_ID_FADER ||
		elem->head.id == SND_US16X08_ID_PHASE ||
		elem->head.id == SND_US16X08_ID_MUTE)
		snd_us16x08_resume_channel(elem);

	/* restore eq values */
	if (elem->head.id >= SND_US16X08_ID_EQLOWLEVEL &&
		elem->head.id <= SND_US16X08_ID_EQENABLE)
		snd_us16x08_resume_eq(elem);

	/* restore compressor values */
	if (elem->head.id >= SND_US16X08_ID_COMP_THRESHOLD &&
		elem->head.id <= SND_US16X08_ID_COMP_SWITCH)
		snd_us16x08_resume_comp(elem);

	/* restore route settings */
	if (elem->head.id == SND_US16X08_ID_ROUTE)
		snd_us16x08_resume_route(elem);

	/* restore master value */
	if (elem->head.id == SND_US16X08_ID_FADER)
		snd_us16x08_resume_master(elem);

	/* restore bus setting */
	if (elem->head.id == SND_US16X08_ID_BYPASS ||
		elem->head.id == SND_US16X08_ID_BUSS_OUT ||
		elem->head.id == SND_US16X08_ID_MUTE)
		snd_us16x08_resume_bus(elem);

	return 0;
}

static int add_new_ctl(struct usb_mixer_interface *mixer,
	const struct snd_kcontrol_new *ncontrol,
	int index, int val_type, int channels,
	const char *name, const void *opt,
	void (*freeer)(struct snd_kcontrol *kctl))
{
	struct snd_kcontrol *kctl;
	struct usb_mixer_elem_info *elem;
	int err;

	usb_audio_dbg(mixer->chip, "us16x08 add mixer %s\n", name);

	if (opt == NULL)
		return -EINVAL;

	elem = kzalloc(sizeof(*elem), GFP_KERNEL);
	if (!elem)
		return -ENOMEM;

	elem->head.mixer = mixer;
	elem->head.resume = snd_us16x08_resume;
	elem->control = 0;
	elem->idx_off = 0;
	elem->head.id = index;
	elem->val_type = val_type;
	elem->channels = channels;
	elem->private_data = (void *) opt;

	kctl = snd_ctl_new1(ncontrol, elem);
	if (!kctl) {
		kfree(elem);
		return -ENOMEM;
	}

	kctl->private_free = freeer;

	strlcpy(kctl->id.name, name, sizeof(kctl->id.name));

	err = snd_usb_mixer_add_control(&elem->head, kctl);
	if (err < 0)
		return err;

	return 0;
}

/* table of EQ and compressor controls */

static struct snd_us16x08_control_params control_params;

static struct snd_us16x08_control_params eq_controls[] = {
	{ /* EQ switch */
		.kcontrol_new = &snd_us16x08_eq_switch_ctl,
		.control_id = SND_US16X08_ID_EQENABLE,
		.type = USB_MIXER_BOOLEAN,
		.num_channels = 16,
		.name = "EQ enable",
		.freeer = snd_usb_mixer_elem_free
	},
	{ /* EQ low gain */
		.kcontrol_new = &snd_us16x08_eq_gain_ctl,
		.control_id = SND_US16X08_ID_EQLOWLEVEL,
		.type = USB_MIXER_U8,
		.num_channels = 16,
		.name = "Low gain",
		.freeer = snd_usb_mixer_elem_free
	},
	{ /* EQ low freq */
		.kcontrol_new = &snd_us16x08_eq_low_freq_ctl,
		.control_id = SND_US16X08_ID_EQLOWFREQ,
		.type = USB_MIXER_U8,
		.num_channels = 16,
		.name = "Low freq",
		.freeer = NULL
	},
	{ /* EQ mid low gain */
		.kcontrol_new = &snd_us16x08_eq_gain_ctl,
		.control_id = SND_US16X08_ID_EQLOWMIDLEVEL,
		.type = USB_MIXER_U8,
		.num_channels = 16,
		.name = "Mid low gain",
		.freeer = snd_usb_mixer_elem_free
	},
	{ /* EQ mid low freq */
		.kcontrol_new = &snd_us16x08_eq_mid_freq_ctl,
		.control_id = SND_US16X08_ID_EQLOWMIDFREQ,
		.type = USB_MIXER_U8,
		.num_channels = 16,
		.name = "Mid low freq",
		.freeer = NULL
	},
	{ /* EQ mid low Q */
		.kcontrol_new = &snd_us16x08_eq_mid_width_ctl,
		.control_id = SND_US16X08_ID_EQLOWMIDWIDTH,
		.type = USB_MIXER_U8,
		.num_channels = 16,
		.name = "Mid low Q",
		.freeer = NULL
	},
	{ /* EQ mid high gain */
		.kcontrol_new = &snd_us16x08_eq_gain_ctl,
		.control_id = SND_US16X08_ID_EQHIGHMIDLEVEL,
		.type = USB_MIXER_U8,
		.num_channels = 16,
		.name = "Mid high gain",
		.freeer = snd_usb_mixer_elem_free
	},
	{ /* EQ mid high freq */
		.kcontrol_new = &snd_us16x08_eq_mid_freq_ctl,
		.control_id = SND_US16X08_ID_EQHIGHMIDFREQ,
		.type = USB_MIXER_U8,
		.num_channels = 16,
		.name = "Mid high freq",
		.freeer = NULL
	},
	{ /* EQ mid high Q */
		.kcontrol_new = &snd_us16x08_eq_mid_width_ctl,
		.control_id = SND_US16X08_ID_EQHIGHMIDWIDTH,
		.type = USB_MIXER_U8,
		.num_channels = 16,
		.name = "Mid high Q",
		.freeer = NULL
	},
	{ /* EQ high gain */
		.kcontrol_new = &snd_us16x08_eq_gain_ctl,
		.control_id = SND_US16X08_ID_EQHIGHLEVEL,
		.type = USB_MIXER_U8,
		.num_channels = 16,
		.name = "High gain",
		.freeer = snd_usb_mixer_elem_free
	},
	{ /* EQ low freq */
		.kcontrol_new = &snd_us16x08_eq_high_freq_ctl,
		.control_id = SND_US16X08_ID_EQHIGHFREQ,
		.type = USB_MIXER_U8,
		.num_channels = 16,
		.name = "High freq",
		.freeer = NULL
	},
};

static struct snd_us16x08_control_params comp_controls[] = {
	{ /* Comp enable */
		.kcontrol_new = &snd_us16x08_compswitch_ctl,
		.control_id = SND_US16X08_ID_COMP_SWITCH,
		.type = USB_MIXER_BOOLEAN,
		.num_channels = 16,
		.name = "Comp enable",
		.freeer = snd_usb_mixer_elem_free
	},
	{ /* Comp threshold */
		.kcontrol_new = &snd_us16x08_comp_threshold_ctl,
		.control_id = SND_US16X08_ID_COMP_THRESHOLD,
		.type = USB_MIXER_U8,
		.num_channels = 16,
		.name = "Threshold",
		.freeer = snd_usb_mixer_elem_free
	},
	{ /* Comp ratio */
		.kcontrol_new = &snd_us16x08_comp_ratio_ctl,
		.control_id = SND_US16X08_ID_COMP_RATIO,
		.type = USB_MIXER_U8,
		.num_channels = 16,
		.name = "Ratio",
		.freeer = snd_usb_mixer_elem_free
	},
	{ /* Comp attack */
		.kcontrol_new = &snd_us16x08_comp_attack_ctl,
		.control_id = SND_US16X08_ID_COMP_ATTACK,
		.type = USB_MIXER_U8,
		.num_channels = 16,
		.name = "Attack",
		.freeer = snd_usb_mixer_elem_free
	},
	{ /* Comp release */
		.kcontrol_new = &snd_us16x08_comp_release_ctl,
		.control_id = SND_US16X08_ID_COMP_RELEASE,
		.type = USB_MIXER_U8,
		.num_channels = 16,
		.name = "Release",
		.freeer = snd_usb_mixer_elem_free
	},
	{ /* Comp gain */
		.kcontrol_new = &snd_us16x08_comp_gain_ctl,
		.control_id = SND_US16X08_ID_COMP_GAIN,
		.type = USB_MIXER_U8,
		.num_channels = 16,
		.name = "Gain",
		.freeer = snd_usb_mixer_elem_free
	},
};

static int snd_us16x08_controls_create_eq(struct usb_mixer_interface *mixer)
{
	int i;
	int err;
	void *store = NULL;
	struct snd_us16x08_eq_store *eq_low_store =
		snd_us16x08_create_eq_store(0x01);
	struct snd_us16x08_eq_store *eq_midlow_store =
		snd_us16x08_create_eq_store(0x02);
	struct snd_us16x08_eq_store *eq_midhigh_store =
		snd_us16x08_create_eq_store(0x03);
	struct snd_us16x08_eq_store *eq_high_store =
		snd_us16x08_create_eq_store(0x04);
	struct snd_us16x08_eq_all_store *eq_all_store =
		kmalloc(sizeof(struct snd_us16x08_eq_all_store), GFP_KERNEL);

	/* check for allocation error */
	if (eq_low_store == NULL || eq_midlow_store == NULL ||
		eq_midhigh_store == NULL || eq_high_store == NULL ||
		eq_all_store == NULL)
		return -ENOMEM;

	/* combine EQ per band stores */
	eq_all_store->low_store = eq_low_store;
	eq_all_store->midlow_store = eq_midlow_store;
	eq_all_store->midhigh_store = eq_midhigh_store;
	eq_all_store->high_store = eq_high_store;

	for (i = 0; i < sizeof(eq_controls) /
		sizeof(control_params); i++) {

		switch (eq_controls[i].control_id & 0xf) {
		case 0x00:
			store = eq_all_store;
			break;
		case 0x01:
			store = eq_low_store;
			break;
		case 0x02:
			store = eq_midlow_store;
			break;
		case 0x03:
			store = eq_midhigh_store;
			break;
		case 0x04:
			store = eq_high_store;
			break;
		}
		err = add_new_ctl(mixer,
			eq_controls[i].kcontrol_new,
			eq_controls[i].control_id,
			eq_controls[i].type,
			eq_controls[i].num_channels,
			eq_controls[i].name,
			store,
			eq_controls[i].freeer);
		if (err < 0)
			return err;
	}

	return 0;
}

int snd_us16x08_controls_create(struct usb_mixer_interface *mixer)
{
	int i;
	int err;
	struct snd_us16x08_common *route_store;
	struct snd_us16x08_comp_store *comp_store;
	struct snd_us16x08_meter_store *meter_store;
	struct snd_us16x08_common *master_store;
	struct snd_us16x08_bus_store *bus_store;
	struct snd_us16x08_channel_store *channel_store;

	/* just check for non-MIDI interface */
	if (mixer->hostif->desc.bInterfaceNumber == 3) {

		/* create compressor mixer elements */
		comp_store = snd_us16x08_create_comp_store();
		if (comp_store == NULL)
			return -ENOMEM;

		/* create bus routing store */
		route_store = snd_us16x08_create_route_store();
		if (route_store == NULL)
			return -ENOMEM;

		/* create meters store */
		meter_store = snd_us16x08_create_meter_store();
		if (meter_store == NULL)
			return -ENOMEM;

		/* create master store */
		master_store = snd_us16x08_create_mix_store(127);
		if (master_store == NULL)
			return -ENOMEM;

		/* create bus store */
		bus_store = snd_us16x08_create_bus_store(0);
		if (bus_store == NULL)
			return -ENOMEM;

		/* create channel store */
		channel_store = snd_us16x08_create_channel_store();
		if (channel_store == NULL)
			return -ENOMEM;

		err = add_new_ctl(mixer, &snd_us16x08_route_ctl,
			SND_US16X08_ID_ROUTE, USB_MIXER_U8, 8, "Route",
			(void *) route_store, snd_usb_mixer_elem_free);
		if (err < 0)
			return err;

		err = add_new_ctl(mixer, &snd_us16x08_master_ctl,
			SND_US16X08_ID_FADER, USB_MIXER_U8, 1, "Master",
			(void *) master_store,
			snd_usb_mixer_elem_free);
		if (err < 0)
			return err;

		err = add_new_ctl(mixer, &snd_us16x08_bus_ctl,
			SND_US16X08_ID_BYPASS, USB_MIXER_U8, 1, "Bypass",
			(void *) bus_store, snd_usb_mixer_elem_free);
		if (err < 0)
			return err;

		err = add_new_ctl(mixer, &snd_us16x08_bus_ctl,
			SND_US16X08_ID_BUSS_OUT, USB_MIXER_U8, 1, "Buss out",
			(void *) bus_store, NULL);
		if (err < 0)
			return err;

		err = add_new_ctl(mixer, &snd_us16x08_bus_ctl,
			SND_US16X08_ID_MUTE, USB_MIXER_U8, 1, "Master mute",
			(void *) bus_store, NULL);
		if (err < 0)
			return err;

		err = add_new_ctl(mixer, &snd_us16x08_ch_boolean_ctl,
			SND_US16X08_ID_PHASE, USB_MIXER_U8, 16, "Phase",
			(void *) channel_store,
			snd_usb_mixer_elem_free);
		if (err < 0)
			return err;

		err = add_new_ctl(mixer, &snd_us16x08_ch_int_ctl,
			SND_US16X08_ID_FADER, USB_MIXER_S16, 16, "Fader",
			(void *) channel_store, NULL);
		if (err < 0)
			return err;

		err = add_new_ctl(mixer, &snd_us16x08_ch_boolean_ctl,
			SND_US16X08_ID_MUTE, USB_MIXER_BOOLEAN, 16, "Mute",
			(void *) channel_store, NULL);

		if (err < 0)
			return err;

		err = add_new_ctl(mixer, &snd_us16x08_ch_int_ctl,
			SND_US16X08_ID_PAN, USB_MIXER_U16, 16, "Pan",
			(void *) channel_store, NULL);
		if (err < 0)
			return err;

		/* add EQ controls */
		err = snd_us16x08_controls_create_eq(mixer);
		if (err < 0)
			return err;

		/* add compressor controls */
		for (i = 0;
			i < sizeof(comp_controls)
			/ sizeof(control_params);
			i++) {

			err = add_new_ctl(mixer,
				comp_controls[i].kcontrol_new,
				comp_controls[i].control_id,
				comp_controls[i].type,
				comp_controls[i].num_channels,
				comp_controls[i].name,
				comp_store,
				comp_controls[i].freeer);
			if (err < 0)
				return err;
		}

		/* meter function 'get' must access to compressor store
		 * so place a reference here
		 */
		meter_store->comp_store = comp_store;
		err = add_new_ctl(mixer, &snd_us16x08_meter_ctl,
			SND_US16X08_ID_METER, USB_MIXER_U16, 0, "Meter",
			(void *) meter_store, snd_usb_mixer_elem_free);
		if (err < 0)
			return err;
	}

	return 0;
}
