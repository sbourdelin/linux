/*
 * Shared Memory Communications over RDMA (SMC-R) and RoCE
 *
 *  PNET table queries
 *
 *  Copyright IBM Corp. 2016
 *
 *  Author(s):  Thomas Richter <tmricht@linux.vnet.ibm.com>
 */

#ifndef _SMC_PNET_H
#define _SMC_PNET_H

#define SMC_MAX_PORTS		2	/* Max # of ports */

int smc_pnet_init(void) __init;
void smc_pnet_exit(void);

#endif
