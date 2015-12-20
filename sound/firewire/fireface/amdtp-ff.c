/*
 * amdtp-ff.c - a part of driver for RME Fireface series
 *
 * Copyright (c) 2015-2016 Takashi Sakamoto
 *
 * Licensed under the terms of the GNU General Public License, version 2.
 */

#include <sound/pcm.h>
#include "fireface.h"

struct amdtp_ff {
	unsigned int pcm_channels;

	void (*transfer_samples)(struct amdtp_stream *s,
				 struct snd_pcm_substream *pcm,
				 __be32 *buffer, unsigned int frames);
};

int amdtp_ff_set_parameters(struct amdtp_stream *s, unsigned int rate,
			    unsigned int pcm_channels)
{
	struct amdtp_ff *p = s->protocol;
	unsigned int data_channels;

	if (amdtp_stream_running(s))
		return -EBUSY;

	p->pcm_channels = pcm_channels;
	data_channels = pcm_channels;

	return amdtp_stream_set_parameters(s, rate, data_channels);
}

static void write_pcm_s32(struct amdtp_stream *s,
			  struct snd_pcm_substream *pcm,
			  __be32 *buffer, unsigned int frames)
{
	struct amdtp_ff *p = s->protocol;
	struct snd_pcm_runtime *runtime = pcm->runtime;
	unsigned int channels, remaining_frames, i, c;
	const u32 *src;

	channels = p->pcm_channels;
	src = (void *)runtime->dma_area +
			frames_to_bytes(runtime, s->pcm_buffer_pointer);
	remaining_frames = runtime->buffer_size - s->pcm_buffer_pointer;

	for (i = 0; i < frames; ++i) {
		for (c = 0; c < channels; ++c) {
			/* TODO: fill upper 1 byte for the other aim. */
			buffer[c] = cpu_to_le32(*src);
			src++;
		}
		buffer += s->data_block_quadlets;
		if (--remaining_frames == 0)
			src = (void *)runtime->dma_area;
	}
}

static void write_pcm_s16(struct amdtp_stream *s,
			  struct snd_pcm_substream *pcm,
			  __be32 *buffer, unsigned int frames)
{
	struct amdtp_ff *p = s->protocol;
	struct snd_pcm_runtime *runtime = pcm->runtime;
	unsigned int channels, remaining_frames, i, c;
	const u16 *src;

	channels = p->pcm_channels;
	src = (void *)runtime->dma_area +
			frames_to_bytes(runtime, s->pcm_buffer_pointer);
	remaining_frames = runtime->buffer_size - s->pcm_buffer_pointer;

	for (i = 0; i < frames; ++i) {
		for (c = 0; c < channels; ++c) {
			/* TODO: fill upper 1 byte for the other aim. */
			buffer[c] = cpu_to_le32(*src << 16);
			src++;
		}
		buffer += s->data_block_quadlets;
		if (--remaining_frames == 0)
			src = (void *)runtime->dma_area;
	}
}

static void read_pcm_s32(struct amdtp_stream *s,
			 struct snd_pcm_substream *pcm,
			 __be32 *buffer, unsigned int frames)
{
	struct amdtp_ff *p = s->protocol;
	struct snd_pcm_runtime *runtime = pcm->runtime;
	unsigned int channels, remaining_frames, i, c;
	u32 *dst;

	channels = p->pcm_channels;
	dst  = (void *)runtime->dma_area +
			frames_to_bytes(runtime, s->pcm_buffer_pointer);
	remaining_frames = runtime->buffer_size - s->pcm_buffer_pointer;

	for (i = 0; i < frames; ++i) {
		for (c = 0; c < channels; ++c) {
			/* TODO: read upper 1 byte for the other aim. */
			*dst = cpu_to_le32(buffer[c]) & 0xffffff00;
			dst++;
		}
		buffer += s->data_block_quadlets;
		if (--remaining_frames == 0)
			dst = (void *)runtime->dma_area;
	}
}

static void write_pcm_silence(struct amdtp_stream *s,
			      __be32 *buffer, unsigned int frames)
{
	struct amdtp_ff *p = s->protocol;
	unsigned int i, c, channels = p->pcm_channels;

	for (i = 0; i < frames; ++i) {
		for (c = 0; c < channels; ++c)
			/* TODO: write upper 1 byte for the other aim. */
			buffer[c] = cpu_to_le32(0x00000000);
		buffer += s->data_block_quadlets;
	}
}

void amdtp_ff_set_pcm_format(struct amdtp_stream *s, snd_pcm_format_t format)
{
	struct amdtp_ff *p = s->protocol;

	if (WARN_ON(amdtp_stream_pcm_running(s)))
		return;

	switch (format) {
	default:
		WARN_ON(1);
		/* fail through */
	case SNDRV_PCM_FORMAT_S16:
		if (s->direction == AMDTP_OUT_STREAM) {
			p->transfer_samples = write_pcm_s16;
			break;
		}
		WARN_ON(1);
		/* fail through */
	case SNDRV_PCM_FORMAT_S32:
		if (s->direction == AMDTP_OUT_STREAM)
			p->transfer_samples = write_pcm_s32;
		else
			p->transfer_samples = read_pcm_s32;
		break;
	}
}

int amdtp_ff_add_pcm_hw_constraints(struct amdtp_stream *s,
				    struct snd_pcm_runtime *runtime)
{
	int err;

	/*
	 * Our implementation allows this protocol to deliver 24 bit sample in
	 * 32bit data channel.
	 */
	err = snd_pcm_hw_constraint_msbits(runtime, 0, 32, 24);
	if (err < 0)
		return err;

	return amdtp_stream_add_pcm_hw_constraints(s, runtime);
}

static unsigned int process_rx_data_blocks(struct amdtp_stream *s,
					   __be32 *buffer,
					   unsigned int data_blocks,
					   unsigned int *syt)
{
	struct amdtp_ff *p = s->protocol;
	struct snd_pcm_substream *pcm = ACCESS_ONCE(s->pcm);
	unsigned int pcm_frames;

	if (pcm) {
		p->transfer_samples(s, pcm, buffer, data_blocks);
		pcm_frames = data_blocks;
	} else {
		write_pcm_silence(s, buffer, data_blocks);
		pcm_frames = 0;
	}

	return pcm_frames;
}

static unsigned int process_tx_data_blocks(struct amdtp_stream *s,
					   __be32 *buffer,
					   unsigned int data_blocks,
					   unsigned int *syt)
{
	struct amdtp_ff *p = s->protocol;
	struct snd_pcm_substream *pcm = ACCESS_ONCE(s->pcm);
	unsigned int pcm_frames;

	if (pcm) {
		p->transfer_samples(s, pcm, buffer, data_blocks);
		pcm_frames = data_blocks;
	} else {
		pcm_frames = 0;
	}

	return pcm_frames;
}

int amdtp_ff_init(struct amdtp_stream *s, struct fw_unit *unit,
		  enum amdtp_stream_direction dir)
{
	amdtp_stream_process_data_blocks_t process_data_blocks;

	if (dir == AMDTP_IN_STREAM)
		process_data_blocks = process_tx_data_blocks;
	else
		process_data_blocks = process_rx_data_blocks;

	return amdtp_stream_init(s, unit, dir, CIP_NO_HEADERS, 0,
				 process_data_blocks, sizeof(struct amdtp_ff));
}
