/*
 *  Shared Memory Communications over RDMA (SMC-R) and RoCE
 *
 *  Definitions for LLC (link layer control) message handling
 *
 *  Copyright IBM Corp. 2016
 *
 *  Author(s):  Klaus Wacker <Klaus.Wacker@de.ibm.com>
 *              Ursula Braun <ursula.braun@de.ibm.com>
 */

#ifndef SMC_LLC_H
#define SMC_LLC_H

#include "smc_wr.h"

#define SMC_LLC_FLAG_RESP		0x80

#define SMC_LLC_WAIT_FIRST_TIME		(5 * HZ)

enum smc_llc_reqresp {
	SMC_LLC_REQ,
	SMC_LLC_RESP
};

enum smc_llc_msg_type {
	SMC_LLC_CONFIRM_LINK		= 0x01,
};

#define SMC_LLC_DATA_LEN		40

struct smc_llc_hdr {
	struct smc_wr_rx_hdr common;
	u8 length;	/* 44 */
	u8 reserved;
	u8 flags;
} __packed;

struct smc_llc_msg_confirm_link {	/* type 0x01 */
	struct smc_llc_hdr hd;
	u8 sender_mac[ETH_ALEN];
	union ib_gid sender_gid;
	u8 sender_qp_num[3];
	u8 link_num;
	__be32 link_uid;
	u8 max_links;
	u8 reserved[9];
} __packed;

union smc_llc_msg {
	struct smc_llc_msg_confirm_link confirm_link;
	struct {
		struct smc_llc_hdr hdr;
		u8 data[SMC_LLC_DATA_LEN];
	} __packed raw;
} __packed;

/* transmit */
int smc_llc_send_confirm_link(struct smc_link *, u8 *, union ib_gid *,
			      enum smc_llc_reqresp);
int smc_llc_init(void) __init;

#endif /* SMC_LLC_H */
