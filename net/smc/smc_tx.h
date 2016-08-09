/*
 * Shared Memory Communications over RDMA (SMC-R) and RoCE
 *
 * Manage send buffer
 *
 * Copyright IBM Corp. 2016
 *
 * Author(s):  Ursula Braun <ursula.braun@de.ibm.com>
 */

#ifndef SMC_TX_H
#define SMC_TX_H

#include <linux/socket.h>
#include <linux/types.h>

#include "smc.h"
#include "smc_cdc.h"

static inline int smc_tx_prepared_sends(struct smc_connection *conn)
{
	union smc_host_cursor_ovl sent, prep;

	sent.acurs = smc_curs_read(conn->tx_curs_sent.acurs);
	prep.acurs = smc_curs_read(conn->tx_curs_prep.acurs);
	return smc_curs_diff(conn->sndbuf_size, &sent, &prep);
}

void smc_tx_init(struct smc_sock *);
int smc_tx_sendmsg(struct smc_sock *, struct msghdr *, size_t);
int smc_tx_sndbuf_nonempty(struct smc_connection *);
void smc_tx_sndbuf_nonfull(struct smc_sock *);
void smc_tx_consumer_update(struct smc_connection *);

#endif /* SMC_TX_H */
