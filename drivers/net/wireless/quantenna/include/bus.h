/*
 * Copyright (c) 2015-2016 Quantenna Communications
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION
 * OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#ifndef QTNFMAC_BUS_H
#define QTNFMAC_BUS_H

#include <linux/workqueue.h>

/* bitmap for EP status and flags: updated by EP, read by RC */

#define QTN_EP_HAS_UBOOT	BIT(0)
#define QTN_EP_HAS_FIRMWARE	BIT(1)
#define QTN_EP_REQ_UBOOT	BIT(2)
#define QTN_EP_REQ_FIRMWARE	BIT(3)
#define QTN_EP_ERROR_UBOOT	BIT(4)
#define QTN_EP_ERROR_FIRMWARE	BIT(5)

#define QTN_EP_FW_LOADRDY	BIT(8)
#define QTN_EP_FW_SYNC		BIT(9)
#define QTN_EP_FW_RETRY		BIT(10)
#define QTN_EP_FW_QLINK_DONE	BIT(15)
#define QTN_EP_FW_DONE		BIT(16)

/* bitmap for RC status and flags: updated by RC, read by EP */

#define QTN_RC_PCIE_LINK	BIT(0)
#define QTN_RC_NET_LINK		BIT(1)
#define QTN_RC_FW_QLINK		BIT(7)
#define QTN_RC_FW_LOADRDY	BIT(8)
#define QTN_RC_FW_SYNC		BIT(9)

/* state transition timeouts */

#define QTN_FW_DL_TIMEOUT_MS	3000
#define QTN_FW_QLINK_TIMEOUT_MS	20000

/* */

#define QLINK_MAC_MASK		0x04
#define QTNF_MAX_MAC		3

enum qtnf_bus_state {
	QTNF_BUS_DOWN,
	QTNF_BUS_UP
};

enum qtnf_bus_end {
	QTN_BUS_DEVICE,
	QTN_BUS_HOST,
};

enum qtnf_fw_state {
	QTNF_FW_STATE_RESET,
	QTNF_FW_STATE_FW_DNLD_DONE,
	QTNF_FW_STATE_BOOT_DONE,
	QTNF_FW_STATE_ACTIVE,
	QTNF_FW_STATE_DEAD,
};

struct qtnf_bus;

struct qtnf_bus_ops {
	int (*preinit)(struct qtnf_bus *dev);
	void (*stop)(struct qtnf_bus *dev);

	/* boot state methods */
	int (*is_state)(struct qtnf_bus *, enum qtnf_bus_end, u32);
	void (*set_state)(struct qtnf_bus *, enum qtnf_bus_end, u32);
	void (*clear_state)(struct qtnf_bus *, enum qtnf_bus_end, u32);
	int (*poll_state)(struct qtnf_bus *, enum qtnf_bus_end, u32, u32);

	/* data xfer methods */
	int (*data_tx)(struct qtnf_bus *, struct sk_buff *);
	int (*control_tx)(struct qtnf_bus *, struct sk_buff *);
	void (*data_rx_start)(struct qtnf_bus *);
	void (*data_rx_stop)(struct qtnf_bus *);
};

struct qtnf_bus {
	struct device *dev;
	enum qtnf_bus_state state;
	enum qtnf_fw_state fw_state;
	u32 chip;
	u32 chiprev;
	struct qtnf_bus_ops *bus_ops;
	struct qtnf_wmac *mac[QTNF_MAX_MAC];
	struct qtnf_qlink_transport trans;
	struct qtnf_hw_info hw_info;
	char fwname[32];
	struct napi_struct mux_napi;
	struct net_device mux_dev;
	struct completion request_firmware_complete;
	struct workqueue_struct *workqueue;
	struct work_struct event_work;
	struct mutex bus_lock; /* lock during command/event processing */
	/* bus private data */
	char bus_priv[0];
};

static inline void *get_bus_priv(struct qtnf_bus *bus)
{
	if (WARN_ON(!bus)) {
		pr_err("qtnfmac: invalid bus pointer!\n");
		return NULL;
	}

	return &bus->bus_priv;
}

/* This function returns the pointer to transport block. */

static inline struct qtnf_qlink_transport *
qtnf_wmac_get_trans(struct qtnf_wmac *mac)
{
	if (!mac->bus)
		return ERR_PTR(ENODEV);

	return (void *)(&mac->bus->trans);
}

/* callback wrappers */

static inline int qtnf_bus_preinit(struct qtnf_bus *bus)
{
	if (!bus->bus_ops->preinit)
		return 0;
	return bus->bus_ops->preinit(bus);
}

static inline void qtnf_bus_stop(struct qtnf_bus *bus)
{
	bus->bus_ops->stop(bus);
}

static inline int qtnf_bus_data_tx(struct qtnf_bus *bus, struct sk_buff *skb)
{
	return bus->bus_ops->data_tx(bus, skb);
}

static inline int qtnf_bus_control_tx(struct qtnf_bus *bus, struct sk_buff *skb)
{
	return bus->bus_ops->control_tx(bus, skb);
}

static inline int
qtnf_bus_poll_state(struct qtnf_bus *bus, enum qtnf_bus_end ep,
		    u32 state, u32 delay_ms)
{
	return bus->bus_ops->poll_state(bus, ep, state, delay_ms);
}

static inline void qtnf_bus_data_rx_start(struct qtnf_bus *bus)
{
	return bus->bus_ops->data_rx_start(bus);
}

static inline void qtnf_bus_data_rx_stop(struct qtnf_bus *bus)
{
	return bus->bus_ops->data_rx_stop(bus);
}

static __always_inline void qtnf_bus_lock(struct qtnf_bus *bus)
{
	mutex_lock(&bus->bus_lock);
}

static __always_inline void qtnf_bus_unlock(struct qtnf_bus *bus)
{
	mutex_unlock(&bus->bus_lock);
}

/* interface functions from common layer */

void qtnf_rx_frame(struct device *dev, struct sk_buff *rxp);
int qtnf_core_attach(struct qtnf_bus *bus);
void qtnf_core_detach(struct qtnf_bus *bus);
void qtnf_dev_reset(struct device *dev);
void qtnf_txflowblock(struct device *dev, bool state);
void qtnf_txcomplete(struct device *dev, struct sk_buff *txp, bool success);
void qtnf_bus_change_state(struct qtnf_bus *bus, enum qtnf_bus_state state);

#endif /* QTNFMAC_BUS_H */
