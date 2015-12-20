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

#define SND_FF_MAXIMIM_MIDI_QUADS	9
#define SND_FF_IN_MIDI_PORTS		2
#define SND_FF_OUT_MIDI_PORTS		2

struct snd_ff_spec {
	const char *const name;
};

struct snd_ff {
	struct snd_card *card;
	struct fw_unit *unit;
	struct mutex mutex;
	spinlock_t lock;

	bool probed;
	struct delayed_work dwork;

	const struct snd_ff_spec *spec;

	/* To handle MIDI tx. */
	struct snd_rawmidi_substream *tx_midi_substreams[SND_FF_IN_MIDI_PORTS];
	struct fw_address_handler async_handler;

	/* TO handle MIDI rx. */
	struct snd_rawmidi_substream *rx_midi_substreams[SND_FF_OUT_MIDI_PORTS];
	u8 running_status[SND_FF_OUT_MIDI_PORTS];
	__le32 msg_buf[SND_FF_OUT_MIDI_PORTS][SND_FF_MAXIMIM_MIDI_QUADS];
	struct work_struct rx_midi_work[SND_FF_OUT_MIDI_PORTS];
	struct fw_transaction transactions[SND_FF_OUT_MIDI_PORTS];
	ktime_t next_ktime[SND_FF_OUT_MIDI_PORTS];
	bool rx_midi_error[SND_FF_OUT_MIDI_PORTS];
	unsigned int rx_bytes[SND_FF_OUT_MIDI_PORTS];
};

#define SND_FF_ADDR_CONTROLLER_ADDR_HI	0x0000801003f4
#define SND_FF_ADDR_GENERAL_PARAMS	0x00008010051c
#define SND_FF_ADDR_MIDI_RX_PORT_0	0x000080180000
#define SND_FF_ADDR_MIDI_RX_PORT_1	0x000080190000

#define SND_FF_ADDR_MIDI_TX		0x000100000000

int snd_ff_transaction_register(struct snd_ff *ff);
int snd_ff_transaction_reregister(struct snd_ff *ff);
void snd_ff_transaction_unregister(struct snd_ff *ff);

int snd_ff_create_midi_devices(struct snd_ff *ff);

#endif
