/*
 * oxfw-scs1x.c - a part of driver for OXFW970/971 based devices
 *
 * Copyright (c) Clemens Ladisch <clemens@ladisch.de>
 * Copyright (c) 2015 Takashi Sakamoto <o-takashi@sakamocchi.jp>
 *
 * Licensed under the terms of the GNU General Public License, version 2.
 */

#include "oxfw.h"

static int scs1x_add(struct snd_oxfw *oxfw)
{
	struct snd_rawmidi *rmidi;
	int err;

	/* For backward compatibility to scs1x module, use unique name. */
	err = snd_rawmidi_new(oxfw->card, "SCS.1x", 0, 0, 0, &rmidi);
	if (err < 0)
		return err;

	snprintf(rmidi->name, sizeof(rmidi->name),
		 "%s MIDI", oxfw->card->shortname);

	return err;
}

struct snd_oxfw_spec snd_oxfw_spec_scs1x = {
	.add	= scs1x_add,
};
