#include <net/cfg80211.h>
#include <linux/inetdevice.h>

#include "ks7010.h"
#include "cfg80211.h"

static struct cfg80211_ops ks7010_cfg80211_ops = {
};

static const struct ethtool_ops ks7010_ethtool_ops = {
	.get_drvinfo = cfg80211_get_drvinfo,
	.get_link = ethtool_op_get_link,
};

/**
 * ks7010_cfg80211_create() - Create wiphy.
 */
struct ks7010 *ks7010_cfg80211_create(void)
{
	struct ks7010 *ks;
	struct wiphy *wiphy;

	/* create a new wiphy for use with cfg80211 */
	wiphy = wiphy_new(&ks7010_cfg80211_ops, sizeof(*ks));

	if (!wiphy) {
		ks_err("couldn't allocate wiphy device\n");
		return NULL;
	}

	ks = wiphy_priv(wiphy);
	ks->wiphy = wiphy;

	return ks;
}

/**
 * ks7010_cfg80211_destroy() - Free wiphy.
 * @ks: The ks7010 device.
 */
void ks7010_cfg80211_destroy(struct ks7010 *ks)
{
	wiphy_free(ks->wiphy);
}

