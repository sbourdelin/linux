#ifndef _LINUX_AF_XDP_SOCK_H
#define _LINUX_AF_XDP_SOCK_H

struct xdp_sock;
struct xdp_buff;

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
