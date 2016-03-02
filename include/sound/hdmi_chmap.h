/*
 * For multichannel support
 */

#ifndef __SOUND_HDMI_CHMAP_H
#define __SOUND_HDMI_CHMAP_H

#include <sound/hdaudio.h>

struct hdmi_cea_channel_speaker_allocation {
	int ca_index;
	int speakers[8];

	/* derived values, just for convenience */
	int channels;
	int spk_mask;
};
struct hdmi_chmap;

struct hdmi_chmap_ops {
	/*
	 * Helpers for producing the channel map TLVs. These can be overridden
	 * for devices that have non-standard mapping requirements.
	 */
	int (*chmap_cea_alloc_validate_get_type)(struct hdmi_chmap *chmap,
		struct hdmi_cea_channel_speaker_allocation *cap, int channels);
	void (*cea_alloc_to_tlv_chmap)
		(struct hdmi_cea_channel_speaker_allocation *cap,
		unsigned int *chmap, int channels);

	/* check that the user-given chmap is supported */
	int (*chmap_validate)(int ca, int channels, unsigned char *chmap);

	void (*get_chmap)(struct hdac_device *hdac, int pcm_idx,
					unsigned char *chmap);
	void (*set_chmap)(struct hdac_device *hdac, int pcm_idx,
			unsigned char *chmap, int prepared);
	bool (*is_monitor_connected)(struct hdac_device *hdac, int pcm_idx);
};

struct hdmi_chmap {
	unsigned int channels_max; /* max over all cvts */
	struct hdmi_chmap_ops ops;
	struct hdac_device *hdac;
};

#endif /* __SOUND_HDMI_CHMAP_H */
