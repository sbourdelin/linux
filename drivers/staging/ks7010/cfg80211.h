/*
 * Driver for KeyStream wireless LAN cards.
 *
 * Copyright (C) 2005-2008 KeyStream Corp.
 * Copyright (C) 2009 Renesas Technology Corp.
 * Copyright (C) 2017 Tobin C. Harding.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */

#ifndef _KS7010_CFG80211_H
#define _KS7010_CFG80211_H

#include "common.h"

void ks7010_cfg80211_rm_interface(struct ks7010 *ks);
struct wireless_dev *ks7010_cfg80211_add_interface(
	struct ks7010 *ks, const char *name,
	unsigned char name_assign_type,
	enum hif_network_type nw_type);

void ks7010_cfg80211_scan_aborted(struct ks7010 *ks);
void ks7010_cfg80211_scan_complete(struct ks7010 *ks);

void ks7010_cfg80211_stop(struct ks7010_vif *vif);

int ks7010_cfg80211_init(struct ks7010 *ks);
void ks7010_cfg80211_cleanup(struct ks7010 *ks);

struct ks7010 *ks7010_cfg80211_create(void);
void ks7010_cfg80211_destroy(struct ks7010 *ks);

#endif	/* _KS7010_CFG80211_H */
