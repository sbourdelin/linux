#include <net/mac80211.h>
#include "ath5k.h"
#include "base.h"
#include "reg.h"

/*
 * Copyright (c) 2016 Bob Copeland <me@bobcopeland.com>
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer,
 *    without modification.
 * 2. Redistributions in binary form must reproduce at minimum a disclaimer
 *    similar to the "NO WARRANTY" disclaimer below ("Disclaimer") and any
 *    redistribution must be conditioned upon including a substantially
 *    similar Disclaimer requirement for further binary redistribution.
 * 3. Neither the names of the above-listed copyright holders nor the names
 *    of any contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * Alternatively, this software may be distributed under the terms of the
 * GNU General Public License ("GPL") version 2 as published by the Free
 * Software Foundation.
 *
 * NO WARRANTY
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF NONINFRINGEMENT, MERCHANTIBILITY
 * AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * THE COPYRIGHT HOLDERS OR CONTRIBUTORS BE LIABLE FOR SPECIAL, EXEMPLARY,
 * OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER
 * IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGES.
 */

static void ath5k_tx99_queue_frames(struct ath5k_hw *ah, int count)
{
	int i, j;
	int frame_len = 1500;
	__le16 fc;
	u8 rate_idx;
	u8 qnum = 3;
	int max_loops = 1000, loop;
	struct ieee80211_hdr *frame;
	struct ieee80211_tx_info *info;

	struct sk_buff *skb;
	struct ieee80211_tx_control control = {
		.sta = NULL
	};

	for (i = 0; i < count; i++) {
		fc = cpu_to_le16(IEEE80211_FTYPE_DATA | IEEE80211_FCTL_TODS);
		skb = dev_alloc_skb(ah->hw->extra_tx_headroom + frame_len);
		if (!skb)
			continue;
		skb_reserve(skb, ah->hw->extra_tx_headroom);

		frame = (struct ieee80211_hdr *) skb_put(skb, frame_len);
		memset(frame, 0, frame_len);
		frame->frame_control = fc;
		memcpy(frame->addr1, ah->common.macaddr, ETH_ALEN);
		memcpy(frame->addr2, ah->common.macaddr, ETH_ALEN);
		memcpy(frame->addr3, ah->common.macaddr, ETH_ALEN);
		skb_set_queue_mapping(skb, IEEE80211_AC_VO);

		info = IEEE80211_SKB_CB(skb);
		memset(info, 0, sizeof(*info));
		info->flags |= IEEE80211_TX_CTL_INJECTED;

		/* use highest rate on whichever band we're on */
		info->band = ah->curchan->band;
		rate_idx = ah->sbands[info->band].n_bitrates - 1;

		for (j=0; j < IEEE80211_TX_MAX_RATES; j++)
			info->control.rates[j].idx = -1;

		info->control.rates[0].idx = rate_idx;
		info->control.rates[0].count = 15;

		rcu_read_lock();
		ath5k_tx_queue(ah->hw, skb, &ah->txqs[qnum], &control);
		rcu_read_unlock();
	}

	/* wait for queued frames to be sent */
	printk(KERN_INFO "ath5k: tx99: sending initial frames...\n");
	loop = 0;
	while (ath5k_hw_num_tx_pending(ah, qnum) > 0) {
		msleep(10);
		if (loop++ > max_loops)
			break;
	}
	printk(KERN_INFO "ath5k: tx99: done sending initial frames\n");
}

void ath5k_tx99_cw_start(struct ath5k_hw *ah)
{
	u32 val, xpa_orig;
	bool xpaa_active_high, xpab_active_high;

	if (ah->tx99_active)
		return;

	printk(KERN_INFO "ath5k: entering tx99 mode on freq %d, txpower %d dBm\n",
	       ah->curchan->center_freq, ah->ah_txpower.txp_requested),
	ah->tx99_active = true;

	/*
	 * disable TX hang queue check -- if we don't do this then
	 * the tx watchdog will issue a reset eventually and we'll
	 * leave tx99 mode
	 */
	clear_bit(ATH_STAT_STARTED, ah->status);

	/* toggle XPA -- A for 5G or B for 2G */
	ath5k_hw_reg_write(ah, 7, AR5K_PHY_PA_CTL);

	val = ath5k_hw_reg_read(ah, AR5K_PHY_PA_CTL);
	xpaa_active_high = !!(val & AR5K_PHY_PA_CTL_XPA_A_HI);
	xpab_active_high = !!(val & AR5K_PHY_PA_CTL_XPA_B_HI);
	xpa_orig = val;

	if (ah->curchan->band == NL80211_BAND_5GHZ) {
		val &= ~AR5K_PHY_PA_CTL_XPA_A_HI;
		if (!xpaa_active_high)
			val |= AR5K_PHY_PA_CTL_XPA_A_HI;
	} else {
		val &= ~AR5K_PHY_PA_CTL_XPA_B_HI;
		if (!xpab_active_high)
			val |= AR5K_PHY_PA_CTL_XPA_B_HI;
	}
	ath5k_hw_reg_write(ah, val, AR5K_PHY_PA_CTL);

	/*
	 * baseband operates in receive mode in continuous wave mode so
	 * use non-transmitting antenna
	 */
	ah->ah_tx_ant = 2;

	/* send a few frames to ramp up output power */
	ath5k_tx99_queue_frames(ah, 20);

	mdelay(20);

	/* disable interrupts */
	ath5k_hw_set_imr(ah, 0);

	/* force AGC clear */
	AR5K_REG_ENABLE_BITS(ah, AR5K_PHY_TST2, AR5K_PHY_TST2_FORCE_AGC_CLR);
	AR5K_REG_ENABLE_BITS(ah, 0x9864, 0x7f000);
	AR5K_REG_ENABLE_BITS(ah, 0x9924, 0x7f00fe);

	/* disable carrier sense */
	AR5K_REG_ENABLE_BITS(ah, AR5K_DIAG_SW, AR5K_DIAG_SW_RX_CLEAR_HIGH | AR5K_DIAG_SW_IGNORE_CARR_SENSE);

	/* disable receive */
	ath5k_hw_reg_write(ah, AR5K_CR_RXD, AR5K_CR);

	/* set constant values */
	ath5k_hw_reg_write(ah, (0x1ff << 9) | 0x1ff, 0x983c);

	/* enable testmode on the ADC */
	AR5K_REG_MASKED_BITS(ah, AR5K_PHY_TST1, (1 << 7) | (1 << 1),  0xffffff7d);

	/* turn on the ADC */
	ath5k_hw_reg_write(ah, 0x80038ffc, AR5K_PHY_ADC_CTL);
	mdelay(10);

	/* turn on RF */
	val = 0x10a098c2;
	if (ah->curchan->band == NL80211_BAND_2GHZ) {
		val |= 0x400000;
	}
	ath5k_hw_reg_write(ah, val, 0x98dc);
	mdelay(10);
	val |= 0x4000;

	ath5k_hw_reg_write(ah, val, 0x98dc);
}

void ath5k_tx99_cw_stop(struct ath5k_hw *ah)
{
	if (!ah->tx99_active)
		return;

	printk(KERN_INFO "ath5k: leaving tx99 mode\n");
	set_bit(ATH_STAT_STARTED, ah->status);
	ah->tx99_active = false;

	/* just reset the device */
	ath5k_hw_reset(ah, ah->opmode, ah->curchan, false, false);
}
