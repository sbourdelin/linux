/*
 * Shared Memory Communications over RDMA (SMC-R) and RoCE
 *
 *  Definitions for SMC Rendezvous - SMC capability checking
 *
 *  Copyright IBM Corp. 2017
 *
 *  Author(s):  Hans Wippel <hwippel@linux.vnet.ibm.com>
 *              Ursula Braun <ubraun@linux.vnet.ibm.com>
 */

#ifndef _SMC_RV_H
#define _SMC_RV_H

#include <linux/netfilter.h>

#define SMC_LISTEN_PEND_VALID_TIME	(600 * HZ)

struct smc_nf_hook {
	struct mutex		nf_hook_mutex;	/* serialize nf register ops */
	int			refcount;
	struct nf_hook_ops	*hook;
};

extern struct smc_nf_hook smc_nfho_clnt;
extern struct smc_nf_hook smc_nfho_serv;

int smc_rv_nf_register_hook(struct net *net, struct smc_nf_hook *nfho);
void smc_rv_nf_unregister_hook(struct net *net, struct smc_nf_hook *nfho);
void smc_rv_init(void) __init;
#endif
