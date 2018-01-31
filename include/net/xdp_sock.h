#ifndef _LINUX_AF_XDP_SOCK_H
#define _LINUX_AF_XDP_SOCK_H

#include <linux/dma-direction.h>

struct buff_pool;
struct net_device;
struct xdp_buff;
struct xdp_sock;

/* These two functions have to be called from the same serializing conext,
 * for example the same NAPI context.
 * They should not be called for the XDP_SKB path, only XDP_DRV.
 */

struct xsk_tx_parms {
	struct buff_pool *buff_pool;
	int (*dma_map)(struct buff_pool *bp, struct device *dev,
		       enum dma_data_direction dir,
		       unsigned long attr);
	void (*tx_completion)(u32 start, u32 npackets,
			      unsigned long ctx1, unsigned long ctx2);
	unsigned long ctx1;
	unsigned long ctx2;
	int (*get_tx_packet)(struct net_device *dev, u32 queue_id,
			     dma_addr_t *dma, void **data, u32 *len,
			     u32 *offset);
};

struct xsk_rx_parms {
	struct buff_pool *buff_pool;
	int (*dma_map)(struct buff_pool *bp, struct device *dev,
		       enum dma_data_direction dir,
		       unsigned long attr);
	void *error_report_ctx;
	void (*error_report)(void *ctx, int errno);
};

#ifdef CONFIG_XDP_SOCKETS
int xsk_generic_rcv(struct xdp_buff *xdp);
struct xdp_sock *xsk_rcv(struct xdp_sock *xsk, struct xdp_buff *xdp);
void xsk_flush(struct xdp_sock *xsk);
#else
static inline int xsk_generic_rcv(struct xdp_buff *xdp)
{
	return -ENOTSUPP;
}

static inline struct xdp_sock *xsk_rcv(struct xdp_sock *xsk,
				       struct xdp_buff *xdp)
{
	return ERR_PTR(-ENOTSUPP);
}

static inline void xsk_flush(struct xdp_sock *xsk)
{
}
#endif /* CONFIG_XDP_SOCKETS */

#endif /* _LINUX_AF_XDP_SOCK_H */
