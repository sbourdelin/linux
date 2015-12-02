#ifndef __SOUND_PCM_IEC958_H
#define __SOUND_PCM_IEC958_H

#include <linux/types.h>

#ifdef CONFIG_SND_PCM_IEC958
int snd_pcm_create_iec958_consumer(struct snd_pcm_runtime *runtime, u8 *cs,
				   size_t len);

int snd_pcm_update_iec958_consumer(struct snd_pcm_runtime *runtime, u8 *cs,
				   size_t len);
#else
int snd_pcm_create_iec958_consumer(struct snd_pcm_runtime *runtime, u8 *cs,
				   size_t len)
{
	return len;
}

int snd_pcm_update_iec958_consumer(struct snd_pcm_runtime *runtime, u8 *cs,
				   size_t len)
{
	return len;
}
#endif

#endif
