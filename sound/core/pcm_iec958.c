/*
 *  PCM DRM helpers
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License version 2 as
 *   published by the Free Software Foundation.
 */
#include <linux/export.h>
#include <linux/types.h>
#include <sound/asoundef.h>
#include <sound/pcm.h>
#include <sound/pcm_iec958.h>

/*
 * snd_pcm_update_iec958_consumer - update consumer format IEC958 channel status
 * @runtime: pcm runtime structure with ->rate filled in
 * @cs: channel status buffer, at least four bytes
 * @len: length of channel status buffer
 *
 * Update sampling frequency and word length parameters of the consumer format
 * channel status data in @cs of maximum size @len.
 * Values correspond to the rate and format parameters of the PCM runtime
 * @runtime.
 *
 * Returns: length of buffer, or negative error code if something failed.
 */
int snd_pcm_update_iec958_consumer(struct snd_pcm_runtime *runtime, u8 *cs,
				   size_t len)
{
	if (len < 4)
		return -EINVAL;

	cs[3] &= ~IEC958_AES3_CON_FS;

	switch (runtime->rate) {
	case 22050:
		cs[3] |= IEC958_AES3_CON_FS_22050;
		break;
	case 32000:
		cs[3] |= IEC958_AES3_CON_FS_32000;
		break;
	case 44100:
		cs[3] |= IEC958_AES3_CON_FS_44100;
		break;
	case 48000:
		cs[3] |= IEC958_AES3_CON_FS_48000;
		break;
	case 88200:
		cs[3] |= IEC958_AES3_CON_FS_88200;
		break;
	case 96000:
		cs[3] |= IEC958_AES3_CON_FS_96000;
		break;
	case 176400:
		cs[3] |= IEC958_AES3_CON_FS_176400;
		break;
	case 192000:
		cs[3] |= IEC958_AES3_CON_FS_192000;
		break;
	default:
		return -EINVAL;
	}

	if (len > 4) {
		cs[4] &= ~IEC958_AES4_CON_WORDLEN;

		switch (snd_pcm_format_width(runtime->format)) {
		case 16:
			cs[4] |= IEC958_AES4_CON_WORDLEN_20_16;
			break;
		case 18:
			cs[4] |= IEC958_AES4_CON_WORDLEN_22_18;
			break;
		case 20:
			cs[4] |= IEC958_AES4_CON_WORDLEN_20_16 |
			     IEC958_AES4_CON_MAX_WORDLEN_24;
			break;
		case 24:
			cs[4] |= IEC958_AES4_CON_WORDLEN_24_20 |
			     IEC958_AES4_CON_MAX_WORDLEN_24;
			break;

		default:
			return -EINVAL;
		}
	}

	return len;
}
EXPORT_SYMBOL(snd_pcm_update_iec958_consumer);

/**
 * snd_pcm_create_iec958_consumer - create consumer format IEC958 channel status
 * @runtime: pcm runtime structure with ->rate filled in
 * @cs: channel status buffer, at least four bytes
 * @len: length of channel status buffer
 *
 * Create the consumer format channel status data in @cs of maximum size
 * @len corresponding to the parameters of the PCM runtime @runtime.
 *
 * Drivers may wish to tweak the contents of the buffer after creation.
 *
 * Returns: length of buffer, or negative error code if something failed.
 */
int snd_pcm_create_iec958_consumer(struct snd_pcm_runtime *runtime, u8 *cs,
				   size_t len)
{
	int ret;

	memset(cs, 0, len);

	ret = snd_pcm_update_iec958_consumer(runtime, cs, len);
	if (ret < 0)
		return ret;

	cs[0] = IEC958_AES0_CON_NOT_COPYRIGHT | IEC958_AES0_CON_EMPHASIS_NONE;
	cs[1] = IEC958_AES1_CON_GENERAL;
	cs[2] = IEC958_AES2_CON_SOURCE_UNSPEC | IEC958_AES2_CON_CHANNEL_UNSPEC;
	cs[3] |= IEC958_AES3_CON_CLOCK_1000PPM;

	return len;
}
EXPORT_SYMBOL(snd_pcm_create_iec958_consumer);
