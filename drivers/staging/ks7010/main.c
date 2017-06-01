#include <linux/firmware.h>
#include <linux/mmc/card.h>
#include <linux/mmc/sdio_func.h>
#include <linux/workqueue.h>
#include <linux/delay.h>
#include <linux/inetdevice.h>

#include "ks7010.h"
#include "sdio.h"
#include "cfg80211.h"

/**
 * ks7010_is_asleep() - True if the device is asleep.
 * @ks: The ks7010 device.
 */
bool ks7010_is_asleep(struct ks7010 *ks)
{
	ks_debug("not implemented yet");
	return false;
}

/**
 * ks7010_request_wakeup() - Request the device to enter active mode.
 * @ks: The ks7010 device.
 */
void ks7010_request_wakeup(struct ks7010 *ks)
{
	ks_debug("not implemented yet");
}

/**
 * ks7010_request_sleep() - Request the device to enter sleep mode.
 * @ks: The ks7010 device.
 */
void ks7010_request_sleep(struct ks7010 *ks)
{
	ks_debug("not implemented yet");
}

static int ks7010_open(struct net_device *ndev)
{
	return 0;
}

static int ks7010_close(struct net_device *ndev)
{
	ks_debug("not implemented yet");
	return 0;
}

static int
ks7010_set_features(struct net_device *dev, netdev_features_t features)
{
	ks_debug("not implemented yet");
	return 0;
}

static void ks7010_set_multicast_list(struct net_device *dev)
{
	ks_debug("not implemented yet");
}

static const unsigned char dummy_addr[] = {
	0x00, 0x0b, 0xe3, 0x00, 0x00, 0x00
};

static const struct net_device_ops ks7010_netdev_ops = {
	.ndo_open               = ks7010_open,
	.ndo_stop               = ks7010_close,
	.ndo_start_xmit         = ks7010_tx_start,
	.ndo_set_features       = ks7010_set_features,
	.ndo_set_rx_mode	= ks7010_set_multicast_list,
};

/**
 * ks7010_init() - Initialize the ks7010 device.
 * @ks: The ks7010 device.
 */
int ks7010_init(struct ks7010 *ks)
{
	ks_debug("not implemented yet");
	return 0;
}

/**
 * ks7010_cleanup() - Cleanup the ks7010 device.
 * @ks: The ks7010 device.
 */
void ks7010_cleanup(struct ks7010 *ks)
{
	ks_debug("not implemented yet");
}

/* FIXME what about the device embedded in the net_device? */

/**
 * ks7010 *ks7010_create() - Create the ks7010 device.
 * @dev: The device embedded within the SDIO function.
 */
struct ks7010 *ks7010_create(struct device *dev)
{
	struct ks7010 *ks;

	ks = ks7010_cfg80211_create();
	if (!ks)
		return NULL;

	ks->dev = dev;
	ks->state = KS7010_STATE_OFF;

	return ks;
}

/**
 * ks7010_destroy() - Destroy the ks7010 device.
 * @ks: The ks7010 device.
 */
void ks7010_destroy(struct ks7010 *ks)
{
	ks->dev = NULL;
	ks7010_cfg80211_destroy(ks);
}
