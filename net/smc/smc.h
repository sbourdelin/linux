/*
 *  Shared Memory Communications over RDMA (SMC-R) and RoCE
 *
 *  Definitions for the SMC module (socket related)
 *
 *  Copyright IBM Corp. 2016
 *
 *  Author(s):  Ursula Braun <ubraun@linux.vnet.ibm.com>
 */
#ifndef _SMC_H
#define _SMC_H

#include <linux/socket.h>
#include <linux/types.h>
#include <net/sock.h>

#define SMCPROTO_SMC		0	/* SMC protocol */

enum smc_state {		/* possible states of an SMC socket */
	SMC_ACTIVE	= 1,
	SMC_INIT	= 2,
	SMC_CLOSED	= 7,
	SMC_LISTEN	= 10,
};

struct smc_sock {				/* smc sock container */
	struct sock		sk;
	struct socket		*clcsock;	/* internal tcp socket */
	u8			use_fallback : 1; /* fallback to tcp */
};

static inline struct smc_sock *smc_sk(const struct sock *sk)
{
	return (struct smc_sock *)sk;
}

#define SMC_SYSTEMID_LEN		8

extern u8	local_systemid[SMC_SYSTEMID_LEN]; /* unique system identifier */

#endif	/* _SMC_H */
