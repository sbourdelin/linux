/*
 * Copyright (c) 2015-2016 Quantenna Communications, Inc.
 * All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#ifndef _QTN_FMAC_CFG80211_H_
#define _QTN_FMAC_CFG80211_H_

#include <net/cfg80211.h>

#include "core.h"

int qtnf_register_wiphy(struct qtnf_bus *bus, struct qtnf_wmac *mac);
int qtnf_del_virtual_intf(struct wiphy *wiphy, struct wireless_dev *wdev);
void qtnf_virtual_intf_local_reset(struct net_device *ndev);
struct wireless_dev *qtnf_add_virtual_intf(struct wiphy *wiphy,
					   const char *name,
					   unsigned char name_assign_type,
					   enum nl80211_iftype type, u32 *flags,
					   struct vif_params *params);
void qtnf_band_init_rates(struct ieee80211_supported_band *band);
void qtnf_band_setup_htvht_caps(struct qtnf_mac_info *macinfo,
				struct ieee80211_supported_band *band);

static inline void qtnf_scan_done(struct qtnf_wmac *mac, bool aborted)
{
	struct cfg80211_scan_info info = {
		.aborted = aborted,
	};

	if (mac->scan_req) {
		cfg80211_scan_done(mac->scan_req, &info);
		mac->scan_req = NULL;
	}
}

#endif /* _QTN_FMAC_CFG80211_H_ */
