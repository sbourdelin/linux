/*
 * Shared Memory Communications over RDMA (SMC-R) and RoCE
 *
 * Handle /proc entries for SMC sockets
 *
 * Copyright IBM Corp. 2016
 *
 * Author(s):  Ursula Braun <ubraun@linux.vnet.ibm.com>
 */

#ifndef SMC_PROC_H
#define SMC_PROC_H

void smc_proc_sock_list_add(struct smc_sock *);
void smc_proc_sock_list_del(struct smc_sock *);
int smc_proc_init(void) __init;
void smc_proc_exit(void);

#endif /* SMC_PROC_H */
