/*
 * For multichannel support
 */

#ifndef __SOUND_HDMI_CHMAP_H
#define __SOUND_HDMI_CHMAP_H

#include <sound/pcm.h>
#include <sound/hdaudio.h>

#define SND_PRINT_CHANNEL_ALLOCATION_ADVISED_BUFSIZE 80

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
	void (*cea_alloc_to_tlv_chmap)(struct hdmi_chmap *hchmap,
		struct hdmi_cea_channel_speaker_allocation *cap,
		unsigned int *chmap, int channels);

	/* check that the user-given chmap is supported */
	int (*chmap_validate)(struct hdmi_chmap *hchmap, int ca,
			int channels, unsigned char *chmap);

	void (*get_chmap)(struct hdac_device *hdac, int pcm_idx,
					unsigned char *chmap);
	void (*set_chmap)(struct hdac_device *hdac, int pcm_idx,
			unsigned char *chmap, int prepared);
	bool (*is_monitor_connected)(struct hdac_device *hdac, int pcm_idx);

	/* get and set channel assigned to each HDMI ASP (audio sample packet) slot */
	int (*pin_get_slot_channel)(struct hdac_device *codec,
			hda_nid_t pin_nid, int asp_slot);
	int (*pin_set_slot_channel)(struct hdac_device *codec,
			hda_nid_t pin_nid, int asp_slot, int channel);
	void (*set_channel_count)(struct hdac_device *codec,
				hda_nid_t cvt_nid, int chs);
	int (*get_active_channels)(int ca);
	void (*setup_channel_mapping)(struct hdmi_chmap *chmap,
			hda_nid_t pin_nid, bool non_pcm, int ca,
			int channels, unsigned char *map,
			bool chmap_set);
	int (*channel_allocation)(struct hdac_device *hdac, int spk_alloc,
				int channels, bool chmap_set,
				bool non_pcm, unsigned char *map);
	struct hdmi_cea_channel_speaker_allocation *(*get_cap_from_ca)(int ca);
	int (*alsa_chmap_to_spk_mask)(unsigned char c);
	int (*spk_to_alsa_chmap)(int spk);
};

struct hdmi_chmap {
	unsigned int channels_max; /* max over all cvts */
	struct hdmi_chmap_ops ops;
	struct hdac_device *hdac;
};

void snd_hdmi_register_chmap_ops(struct hdac_device *hdac,
				struct hdmi_chmap *chmap);
void snd_hdmi_print_channel_allocation(int spk_alloc, char *buf, int buflen);
int snd_hdmi_add_chmap_ctls(struct snd_pcm *pcm, int pcm_idx,
				struct hdmi_chmap *chmap);
#endif /* __SOUND_HDMI_CHMAP_H */
