/*
 * Shared Memory Communications over RDMA (SMC-R) and RoCE
 *
 * Socket Closing
 *
 * Copyright IBM Corp. 2016
 *
 * Author(s):  Ursula Braun <ubraun@linux.vnet.ibm.com>
 */

#ifndef SMC_CLOSE_H
#define SMC_CLOSE_H

#include <linux/workqueue.h>

#include "smc.h"

#define SMC_CLOSE_SOCK_PUT_DELAY		HZ

void smc_close_wake_tx_prepared(struct smc_sock *);
void smc_close_active_abort(struct smc_sock *);
int smc_close_active(struct smc_sock *);
void smc_close_passive_received(struct smc_sock *);
void smc_close_sock_put_work(struct work_struct *);
int smc_close_shutdown_write(struct smc_sock *);

#endif /* SMC_CLOSE_H */
