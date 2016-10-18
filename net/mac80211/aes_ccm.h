/*
 * Copyright 2003-2004, Instant802 Networks, Inc.
 * Copyright 2006, Devicescape Software, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#ifndef AES_CCM_H
#define AES_CCM_H

#include <linux/crypto.h>

#define CCM_AAD_LEN	32

int ieee80211_aes_key_setup_encrypt(struct ieee80211_ccmp_aead *ccmp,
				    const u8 key[], size_t key_len,
				    size_t mic_len);
int ieee80211_aes_ccm_encrypt(struct ieee80211_ccmp_aead *ccmp, u8 *b_0,
			      u8 *aad, u8 *data, size_t data_len, u8 *mic,
			      size_t mic_len);
int ieee80211_aes_ccm_decrypt(struct ieee80211_ccmp_aead *ccmp, u8 *b_0,
			      u8 *aad, u8 *data, size_t data_len, u8 *mic,
			      size_t mic_len);
void ieee80211_aes_key_free(struct ieee80211_ccmp_aead *ccmp);

#endif /* AES_CCM_H */
