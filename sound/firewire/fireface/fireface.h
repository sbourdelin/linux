/*
 * fireface.h - a part of driver for RME Fireface series
 *
 * Copyright (c) 2015-2016 Takashi Sakamoto
 *
 * Licensed under the terms of the GNU General Public License, version 2.
 */

#ifndef SOUND_FIREFACE_H_INCLUDED
#define SOUND_FIREFACE_H_INCLUDED

#include <linux/device.h>
#include <linux/firewire.h>
#include <linux/firewire-constants.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/compat.h>

#include <sound/core.h>
#include <sound/initval.h>
#include <sound/info.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/firewire.h>
#include <sound/hwdep.h>
#include <sound/rawmidi.h>

#include "../lib.h"
#include "../amdtp-stream.h"
#include "../iso-resources.h"

struct snd_ff {
	struct snd_card *card;
	struct fw_unit *unit;
	struct mutex mutex;
};
#endif
