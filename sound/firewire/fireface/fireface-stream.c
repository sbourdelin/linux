/*
 * fireface-stream.c - a part of driver for RME Fireface series
 *
 * Copyright (c) 2015-2016 Takashi Sakamoto
 *
 * Licensed under the terms of the GNU General Public License, version 2.
 */

#include <linux/delay.h>
#include "fireface.h"

#define CALLBACK_TIMEOUT_MS	200

static int get_rate_mode(unsigned int rate, unsigned int *mode)
{
	int i;

	for (i = 0; i < CIP_SFC_COUNT; i++) {
		if (amdtp_rate_table[i] == rate)
			break;
	}

	if (i == CIP_SFC_COUNT)
		return -EINVAL;

	*mode = ((int)i - 1) / 2;

	return 0;
}

int snd_ff_stream_get_clock(struct snd_ff *ff, unsigned int *rate,
			    enum snd_ff_clock_src *src)
{
	__le32 reg;
	u32 data;
	int err;

	err = snd_fw_transaction(ff->unit, TCODE_READ_QUADLET_REQUEST,
				 0x0000801C0004, &reg, sizeof(reg), 0);
	if (err < 0)
		return err;
	data = le32_to_cpu(reg);

	/* Calculate sampling rate. */
	switch ((data >> 1) & 0x03) {
	case 0x01:
		*rate = 32000;
		break;
	case 0x00:
		*rate = 44100;
		break;
	case 0x03:
		*rate = 48000;
		break;
	case 0x02:
	default:
		return -EIO;
	}

	if (data & 0x08)
		*rate *= 2;
	else if (data & 0x10)
		*rate *= 4;

	/* Calculate source of clock. */
	if (data & 0x01) {
		*src = SND_FF_CLOCK_SRC_INTERNAL;
	} else {
		/* TODO: 0x00, 0x01, 0x02, 0x06, 0x07? */
		switch ((data >> 10) & 0x07) {
		case 0x03:
			*src = SND_FF_CLOCK_SRC_SPDIF;
			break;
		case 0x04:
			*src = SND_FF_CLOCK_SRC_WORD;
			break;
		case 0x05:
			*src = SND_FF_CLOCK_SRC_LTC;
			break;
		case 0x00:
		default:
			*src = SND_FF_CLOCK_SRC_ADAT;
			break;
		}
	}

	return 0;
}

/*
 * In this device, the length of register for isochronous channels is just
 * three bits. Therefore, we can allocate between 0 and 7 channel.
 */
static int keep_resources(struct snd_ff *ff, unsigned int rate)
{
	int mode;
	int err;

	err = get_rate_mode(rate, &mode);
	if (err < 0)
		return err;

	/* Keep resources for in-stream. */
	err = amdtp_ff_set_parameters(&ff->tx_stream, rate,
				      ff->spec->pcm_capture_channels[mode]);
	if (err < 0)
		return err;
	ff->tx_resources.channels_mask = 0x00000000000000ffuLL;
	err = fw_iso_resources_allocate(&ff->tx_resources,
			amdtp_stream_get_max_payload(&ff->tx_stream),
			fw_parent_device(ff->unit)->max_speed);
	if (err < 0)
		return err;

	/* Keep resources for out-stream. */
	err = amdtp_ff_set_parameters(&ff->rx_stream, rate,
				      ff->spec->pcm_playback_channels[mode]);
	if (err < 0)
		return err;
	ff->rx_resources.channels_mask = 0x00000000000000ffuLL;
	err = fw_iso_resources_allocate(&ff->rx_resources,
			amdtp_stream_get_max_payload(&ff->rx_stream),
			fw_parent_device(ff->unit)->max_speed);
	if (err < 0)
		fw_iso_resources_free(&ff->tx_resources);

	return err;
}

static void release_resources(struct snd_ff *ff)
{
	fw_iso_resources_free(&ff->tx_resources);
	fw_iso_resources_free(&ff->rx_resources);
}

static int begin_session(struct snd_ff *ff, unsigned int rate)
{
	__le32 reg;
	int i, err;

	/* Check whether the given value is supported or not. */
	for (i = 0; i < CIP_SFC_COUNT; i++) {
		if (amdtp_rate_table[i] == rate)
			break;
	}
	if (i == CIP_SFC_COUNT)
		return -EINVAL;

	/* Set the number of data blocks transferred in a second. */
	reg = cpu_to_le32(rate);
	err = snd_fw_transaction(ff->unit, TCODE_WRITE_QUADLET_REQUEST,
				 0x000080100500, &reg, sizeof(reg), 0);
	if (err < 0)
		return err;

	msleep(100);

	/*
	 * Set isochronous channel and the number of quadlets of received
	 * packets.
	 */
	reg = cpu_to_le32(((ff->rx_stream.data_block_quadlets << 3) << 8) |
			  ff->rx_resources.channel);
	err = snd_fw_transaction(ff->unit, TCODE_WRITE_QUADLET_REQUEST,
				 0x000080100504, &reg, sizeof(reg), 0);
	if (err < 0)
		return err;

	/*
	 * Set isochronous channel and the number of quadlets of transmitted
	 * packet.
	 */
	/* TODO: investigate the purpose of this 0x80. */
	reg = cpu_to_le32((0x80 << 24) |
			  (ff->tx_resources.channel << 5) |
			  (ff->tx_stream.data_block_quadlets));
	err = snd_fw_transaction(ff->unit, TCODE_WRITE_QUADLET_REQUEST,
				 0x00008010050c, &reg, sizeof(reg), 0);
	if (err < 0)
		return err;

	/* Allow to transmit packets. */
	reg = cpu_to_le32(0x00000001);
	return snd_fw_transaction(ff->unit, TCODE_WRITE_QUADLET_REQUEST,
				 0x000080100508, &reg, sizeof(reg), 0);
}

static void finish_session(struct snd_ff *ff)
{
	__le32 reg;

	reg = cpu_to_le32(0x80000000);
	snd_fw_transaction(ff->unit, TCODE_WRITE_QUADLET_REQUEST,
			   0x000080100510, &reg, sizeof(reg), 0);
}

static int init_stream(struct snd_ff *ff, enum amdtp_stream_direction dir)
{
	int err;
	struct fw_iso_resources *resources;
	struct amdtp_stream *stream;

	if (dir == AMDTP_IN_STREAM) {
		resources = &ff->tx_resources;
		stream = &ff->tx_stream;
	} else {
		resources = &ff->rx_resources;
		stream = &ff->rx_stream;
	}

	err = fw_iso_resources_init(resources, ff->unit);
	if (err < 0)
		return err;

	err = amdtp_ff_init(stream, ff->unit, dir);
	if (err < 0)
		fw_iso_resources_destroy(resources);

	return err;
}

static void destroy_stream(struct snd_ff *ff, enum amdtp_stream_direction dir)
{
	if (dir == AMDTP_IN_STREAM) {
		amdtp_stream_destroy(&ff->tx_stream);
		fw_iso_resources_destroy(&ff->tx_resources);
	} else {
		amdtp_stream_destroy(&ff->rx_stream);
		fw_iso_resources_destroy(&ff->rx_resources);
	}
}

int snd_ff_stream_init_duplex(struct snd_ff *ff)
{
	int err;

	err = init_stream(ff, AMDTP_OUT_STREAM);
	if (err < 0)
		goto end;

	err = init_stream(ff, AMDTP_IN_STREAM);
	if (err < 0)
		destroy_stream(ff, AMDTP_OUT_STREAM);
end:
	return err;
}

/*
 * This function should be called before starting streams or after stopping
 * streams.
 */
void snd_ff_stream_destroy_duplex(struct snd_ff *ff)
{
	destroy_stream(ff, AMDTP_IN_STREAM);
	destroy_stream(ff, AMDTP_OUT_STREAM);
}

int snd_ff_stream_start_duplex(struct snd_ff *ff, unsigned int rate)
{
	unsigned int curr_rate;
	enum snd_ff_clock_src src;
	int err;

	if (ff->substreams_counter == 0)
		return 0;

	err = snd_ff_stream_get_clock(ff, &curr_rate, &src);
	if (err < 0)
		return err;
	if (curr_rate != rate ||
	    amdtp_streaming_error(&ff->tx_stream) ||
	    amdtp_streaming_error(&ff->rx_stream)) {
		finish_session(ff);

		amdtp_stream_stop(&ff->tx_stream);
		amdtp_stream_stop(&ff->rx_stream);

		release_resources(ff);
	}

	/*
	 * Regardless of current source of clock signal, drivers transfer some
	 * packets. Then, the device transfers packets.
	 */
	if (!amdtp_stream_running(&ff->rx_stream)) {
		err = keep_resources(ff, rate);
		if (err < 0)
			goto error;

		err = begin_session(ff, rate);
		if (err < 0)
			goto error;

		err = amdtp_stream_start(&ff->rx_stream,
					 ff->rx_resources.channel,
					 fw_parent_device(ff->unit)->max_speed);
		if (err < 0)
			goto error;

		if (!amdtp_stream_wait_callback(&ff->rx_stream,
						CALLBACK_TIMEOUT_MS)) {
			err = -ETIMEDOUT;
			goto error;
		}
	}

	/*
	 * The incoming packets has no timestamp, thus no afraid of detecting
	 * packet discontinuity.
	 */
	if (!amdtp_stream_running(&ff->tx_stream)) {
		err = amdtp_stream_start(&ff->tx_stream,
					 ff->tx_resources.channel,
					 fw_parent_device(ff->unit)->max_speed);
		if (err < 0)
			goto error;

		if (!amdtp_stream_wait_callback(&ff->tx_stream,
						CALLBACK_TIMEOUT_MS)) {
			err = -ETIMEDOUT;
			goto error;
		}
	}

	return 0;
error:
	amdtp_stream_stop(&ff->tx_stream);
	amdtp_stream_stop(&ff->rx_stream);

	finish_session(ff);
	release_resources(ff);

	return err;
}

void snd_ff_stream_stop_duplex(struct snd_ff *ff)
{
	if (ff->substreams_counter > 0)
		return;

	amdtp_stream_stop(&ff->tx_stream);
	amdtp_stream_stop(&ff->rx_stream);
	finish_session(ff);
	release_resources(ff);
}

void snd_ff_stream_update_duplex(struct snd_ff *ff)
{
	/* The device discontinue to transfer packets.  */
	amdtp_stream_pcm_abort(&ff->tx_stream);
	amdtp_stream_stop(&ff->tx_stream);

	amdtp_stream_pcm_abort(&ff->rx_stream);
	amdtp_stream_stop(&ff->rx_stream);

	fw_iso_resources_update(&ff->tx_resources);
	fw_iso_resources_update(&ff->rx_resources);
}
