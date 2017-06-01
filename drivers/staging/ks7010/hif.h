#ifndef _KS7010_HIF_H
#define _KS7010_HIF_H

#include <linux/compiler.h>
#include <linux/skbuff.h>
#include <linux/ieee80211.h>

#include "common.h"

int ks7010_hif_tx(struct ks7010 *ks, struct sk_buff *skb);
void ks7010_hif_rx(struct ks7010 *ks, u8 *data, size_t data_size);

void ks7010_hif_set_power_mgmt_active(struct ks7010 *ks);
void ks7010_hif_set_power_mgmt_sleep(struct ks7010 *ks);
void ks7010_hif_set_power_mgmt_deep_sleep(struct ks7010 *ks);

void ks7010_hif_init(struct ks7010 *ks);
void ks7010_hif_cleanup(struct ks7010 *ks);

void ks7010_hif_create(struct ks7010 *ks);
void ks7010_hif_destroy(struct ks7010 *ks);

#endif	/* _KS7010_HIF_H */
