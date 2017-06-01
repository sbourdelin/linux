#include <linux/skbuff.h>
#include <linux/netdevice.h>

#include "ks7010.h"

/**
 * ks7010_tx_start() - Start transmit.
 * @ndev: The net_device associated with this sk_buff.
 * @skb: sk_buff passed down from the networking stack.
 *
 * Tx data path initiation function called by the networking stack.
 */
int ks7010_tx_start(struct sk_buff *skb, struct net_device *ndev)
{
	return 0;
}

/**
 * ks7010_tx() - Queue tx frame for transmission.
 * @ks: The ks7010 device.
 * @data: Data to transmit.
 * @size: Size of data.
 * @skb: Pointer to associated sk_buff, NULL for SME frames.
 */
int ks7010_tx(struct ks7010 *ks, u8 *data, size_t size, struct sk_buff *skb)
{
	return 0;
}

