/*
 * Shared Memory Communications over RDMA (SMC-R) and RoCE
 *
 * Connection Data Control (CDC)
 *
 * Copyright IBM Corp. 2016
 *
 * Author(s):  Ursula Braun <ursula.braun@de.ibm.com>
 */

#ifndef SMC_CDC_H
#define SMC_CDC_H

#include <linux/kernel.h> /* max_t */
#include <linux/compiler.h> /* __packed */
#include <linux/atomic.h> /* xchg */

#include "smc.h"
#include "smc_core.h"
#include "smc_wr.h"

#define	SMC_CDC_MSG_TYPE		0xFE

/* in network byte order */
struct smc_cdc_cursor {		/* SMC cursor */
	__be16	reserved;
	__be16	wrap;
	__be32	count;
} __packed __aligned(8);

/* in network byte order */
union smc_cdc_cursor_ovl {
	struct	smc_cdc_cursor	curs;
	__be64			acurs;
} __packed __aligned(8);

/* in network byte order */
struct smc_cdc_msg {
	struct smc_wr_rx_hdr		common; /* .type = 0xFE */
	u8				len;	/* 44 */
	__be16				seqno;
	__be32				token;
	union smc_cdc_cursor_ovl	prod;
	union smc_cdc_cursor_ovl	cons;	/* piggy backed "ack" */
	struct smc_cdc_producer_flags	prod_flags;
	struct smc_cdc_conn_state_flags	conn_state_flags;
	u8				reserved[18];
} __packed;

static inline bool smc_cdc_rxed_any_close(struct smc_connection *conn)
{
	return conn->local_rx_ctrl.conn_state_flags.peer_conn_abort ||
	       conn->local_rx_ctrl.conn_state_flags.peer_conn_closed;
}

static inline bool smc_cdc_rxed_any_close_or_senddone(
	struct smc_connection *conn)
{
	return smc_cdc_rxed_any_close(conn) ||
	       conn->local_rx_ctrl.conn_state_flags.peer_done_writing;
}

static inline void smc_curs_add(int size, struct smc_host_cursor *curs,
				int value)
{
	curs->wrap += (curs->count + value) / size;
	curs->count = (curs->count + value) % size;
}

static inline u64 smc_curs_read(u64 c)
{
#if BITS_PER_LONG != 64
	/* We must enforce atomic readout on 32bit, otherwise the
	 * update on another cpu can hit inbetween the readout of
	 * the low 32bit and the high 32bit portion.
	 */
	return cmpxchg64(&c, 0, 0);
#else
	/* On 64 bit the cursor read is atomic versus the update */
	return c;
#endif
}

static inline __be64 smc_curs_read_net(__be64 c)
{
#if BITS_PER_LONG != 64
	/* We must enforce atomic readout on 32bit, otherwise the
	 * update on another cpu can hit inbetween the readout of
	 * the low 32bit and the high 32bit portion.
	 */
	return cmpxchg64(&c, 0, 0);
#else
	/* On 64 bit the cursor read is atomic versus the update */
	return c;
#endif
}

/* calculate cursor difference between old and new, where old <= new */
static inline int smc_curs_diff(unsigned int size,
				union smc_host_cursor_ovl *old,
				union smc_host_cursor_ovl *new)
{
	if (old->curs.wrap != new->curs.wrap)
		return max_t(int, 0,
			     ((size - old->curs.count) + new->curs.count));

	return max_t(int, 0, (new->curs.count - old->curs.count));
}

static inline void smc_host_cursor_to_cdc(struct smc_cdc_cursor *peer,
					  union smc_host_cursor_ovl *local)
{
	union smc_host_cursor_ovl temp;

	temp.acurs = smc_curs_read(local->acurs);
	peer->count = htonl(temp.curs.count);
	peer->wrap = htons(temp.curs.wrap);
	/* peer->reserved = htons(0); must be ensured by caller */
}

static inline void smc_host_msg_to_cdc(struct smc_cdc_msg *peer,
				       struct smc_host_cdc_msg *local)
{
	peer->common.type = local->common.type;
	peer->len = local->len;
	peer->seqno = htons(local->seqno);
	peer->token = htonl(local->token);
	smc_host_cursor_to_cdc(&peer->prod.curs, &local->prod);
	smc_host_cursor_to_cdc(&peer->cons.curs, &local->cons);
	peer->prod_flags = local->prod_flags;
	peer->conn_state_flags = local->conn_state_flags;
}

static inline void smc_cdc_cursor_to_host(union smc_host_cursor_ovl *local,
					  union smc_cdc_cursor_ovl *peer)
{
	union smc_host_cursor_ovl temp, old;
	union smc_cdc_cursor_ovl net;

	old.acurs = smc_curs_read(local->acurs);
	net.acurs = smc_curs_read_net(peer->acurs);
	temp.curs.count = ntohl(net.curs.count);
	temp.curs.wrap = ntohs(net.curs.wrap);
	if ((old.curs.wrap > temp.curs.wrap) && temp.curs.wrap)
		return;
	if ((old.curs.wrap == temp.curs.wrap) &&
	    (old.curs.count > temp.curs.count))
		return;
	xchg(&local->acurs, temp.acurs);
}

static inline void smc_cdc_msg_to_host(struct smc_host_cdc_msg *local,
				       struct smc_cdc_msg *peer)
{
	local->common.type = peer->common.type;
	local->len = peer->len;
	local->seqno = ntohs(peer->seqno);
	local->token = ntohl(peer->token);
	smc_cdc_cursor_to_host(&local->prod, &peer->prod);
	smc_cdc_cursor_to_host(&local->cons, &peer->cons);
	local->prod_flags = peer->prod_flags;
	local->conn_state_flags = peer->conn_state_flags;
}

struct smc_cdc_tx_pend;

int smc_cdc_get_free_slot(struct smc_link *, struct smc_wr_buf **,
			  struct smc_cdc_tx_pend **);
void smc_cdc_tx_dismiss_slots(struct smc_connection *);
int smc_cdc_msg_send(struct smc_connection *, struct smc_wr_buf *,
		     struct smc_cdc_tx_pend *);
int smc_cdc_get_slot_and_msg_send(struct smc_connection *);
bool smc_cdc_tx_has_pending(struct smc_connection *);
int smc_cdc_init(void) __init;

#endif /* SMC_CDC_H */
