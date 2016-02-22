/*
 * For multichannel support
 */

#ifndef __SOUND_HDMI_CHMAP_H
#define __SOUND_HDMI_CHMAP_H

#include <sound/pcm.h>
#include <sound/hdaudio.h>

struct cea_channel_speaker_allocation;

#define SND_PRINT_CHANNEL_ALLOCATION_ADVISED_BUFSIZE 80

struct cea_channel_speaker_allocation {
	int ca_index;
	int speakers[8];

	/* derived values, just for convenience */
	int channels;
	int spk_mask;
};

struct hdmi_chmap_ops {
	/*
	 * Helpers for producing the channel map TLVs. These can be overridden
	 * for devices that have non-standard mapping requirements.
	 */
	int (*chmap_cea_alloc_validate_get_type)
		(struct cea_channel_speaker_allocation *cap, int channels);
	void (*cea_alloc_to_tlv_chmap)
		(struct cea_channel_speaker_allocation *cap,
		unsigned int *chmap, int channels);

	/* check that the user-given chmap is supported */
	int (*chmap_validate)(int ca, int channels, unsigned char *chmap);

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
};

struct hdmi_chmap {
	unsigned int channels_max; /* max over all cvts */
	struct hdmi_chmap_ops ops;
	struct hdac_device *hdac;
};

void snd_hdmi_init_channel_allocations(void);
int snd_hdmi_get_channel_allocation_order(int ca);
int snd_hdmi_get_active_channels(int ca);
struct cea_channel_speaker_allocation *snd_hdmi_get_ch_alloc_from_ca(int ca);
void snd_hdmi_print_channel_allocation(int spk_alloc, char *buf, int buflen);
int snd_hdmi_channel_allocation(struct hdac_device *codec,
		   int spk_alloc, int channels);
int snd_hdmi_to_spk_mask(unsigned char c);
int snd_hdmi_spk_to_chmap(int spk);
int snd_hdmi_manual_channel_allocation(int chs, unsigned char *map);
void snd_hdmi_setup_channel_mapping(struct hdmi_chmap *chmap,
		       hda_nid_t pin_nid, bool non_pcm, int ca,
		       int channels, unsigned char *map,
		       bool chmap_set);
int snd_hdmi_pin_set_slot_channel(struct hdac_device *codec, hda_nid_t pin_nid,
					     int asp_slot, int channel);
int snd_hdmi_pin_get_slot_channel(struct hdac_device *codec, hda_nid_t pin_nid,
					     int asp_slot);
void snd_hdmi_set_channel_count(struct hdac_device *codec,
			   hda_nid_t cvt_nid, int chs);
int snd_hdmi_chmap_cea_alloc_validate_get_type(
		struct cea_channel_speaker_allocation *cap,
		int channels);
void snd_hdmi_cea_alloc_to_tlv_chmap(
		struct cea_channel_speaker_allocation *cap,
		unsigned int *chmap, int channels);
int snd_hdmi_add_chmap_ctls(struct snd_pcm *pcm, int pcm_idx,
				struct hdmi_chmap *chmap);
#endif /* __SOUND_HDMI_CHMAP_H */
