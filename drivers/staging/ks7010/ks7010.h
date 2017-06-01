#ifndef _KS7010_H
#define _KS7010_H

#include <net/cfg80211.h>
#include <linux/list.h>
#include <linux/if_ether.h>
#include <linux/interrupt.h>
#include <linux/wireless.h>
#include <linux/skbuff.h>
#include <crypto/hash.h>

#include "common.h"
#include "fil.h"

#define DRIVER_PREFIX "ks7010: "

#define ks_err(fmt, arg...) \
	pr_err(DRIVER_PREFIX "ERROR " fmt "\n", ##arg)

#define ks_info(fmt, arg...) \
	pr_info(DRIVER_PREFIX "INFO " fmt "\n", ##arg)

#define ks_warn(fmt, arg...) \
	pr_warn(DRIVER_PREFIX "WARNING " fmt "\n", ##arg)

#define ks_debug(fmt, arg...) \
	pr_debug(DRIVER_PREFIX "%s: " fmt "\n", __func__, ##arg)

/**
 * enum ks7010_state - ks7010 device state.
 * @KS7010_STATE_OFF: Device is off.
 * @KS7010_STATE_READY:	Device ready.
 */
enum ks7010_state {
	KS7010_STATE_OFF,
	KS7010_STATE_READY
};

struct ks7010_sdio;

/**
 * ks7010_vif - Virtual interface (net_device private data).
 * @ndev: Pointer to the net_device for this VIF.
 */
struct ks7010_vif {
	struct net_device *ndev;
};

/**
 * struct ks7010 - The ks7010 device.
 * @priv: Pointer to the SDIO private data.
 * @vif: The virtual interface (driver supports single VIF only).
 * @state: The device state, &enum ks7010_state.
 * @wiphy: The device wiphy.
 * @dev: Pointer to the device embedded within the SDIO func.
 * @fil_ops: Firmware interface layer operations.
 * @mac_addr: Device MAC address.
 * @mac_addr_valid: True if @mac_addr is valid.
 */
struct ks7010 {
	struct ks7010_sdio *priv;
	struct ks7010_vif *vif;

	enum ks7010_state state;

	struct wiphy *wiphy;

	struct device *dev;

	struct fil_ops *fil_ops;

	u8 mac_addr[ETH_ALEN];
	bool mac_addr_valid;

	struct crypto_shash *tx_tfm_mic;
	struct crypto_shash *rx_tfm_mic;
};

/* main.c */
bool ks7010_is_asleep(struct ks7010 *ks);
void ks7010_request_wakeup(struct ks7010 *ks);
void ks7010_request_sleep(struct ks7010 *ks);

int ks7010_init(struct ks7010 *ks);
void ks7010_cleanup(struct ks7010 *ks);

struct ks7010 *ks7010_create(struct device *dev);
void ks7010_destroy(struct ks7010 *ks);

/* tx.c */
int ks7010_tx_start(struct sk_buff *skb, struct net_device *dev);
int ks7010_tx(struct ks7010 *ks, u8 *data, size_t size, struct sk_buff *skb);

#endif	/* _KS7010_H */
