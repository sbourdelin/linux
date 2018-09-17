/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2018 Western Digital Corporation
 */
#ifndef UFS_BSG_H
#define UFS_BSG_H

#include <linux/bsg-lib.h>
#include <scsi/scsi.h>
#include <scsi/scsi_host.h>

#include "ufshcd.h"
#include "ufs.h"

#define UPIU_TRANSACTION_UIC_CMD 0x1F

enum {
	REQ_UPIU_SIZE_DWORDS	= 8,
	RSP_UPIU_SIZE_DWORDS	= 8,
};

/* request (CDB) structure of the sg_io_v4 */
struct ufs_bsg_request {
	uint32_t msgcode;
	struct utp_upiu_header header;
	union {
		struct utp_upiu_query		qr;
		struct utp_upiu_query		tr;
		/* use utp_upiu_query to host the 4 dwords of uic command */
		struct utp_upiu_query		uc;
	} tsf;
};

/* response (request sense data) structure of the sg_io_v4 */
struct ufs_bsg_reply {
	/*
	 * The completion result. Result exists in two forms:
	 * if negative, it is an -Exxx system errno value. There will
	 * be no further reply information supplied.
	 * else, it's the 4-byte scsi error result, with driver, host,
	 * msg and status fields. The per-msgcode reply structure
	 * will contain valid data.
	 */
	uint32_t result;

	/* If there was reply_payload, how much was received? */
	uint32_t reply_payload_rcv_len;

	struct utp_upiu_header header;
	union {
		struct utp_upiu_query		qr;
		struct utp_upiu_query		tr;
		struct utp_upiu_query		uc;
	} tsf;
};

#ifdef CONFIG_SCSI_UFS_BSG
void ufs_bsg_remove(struct ufs_hba *hba);
int ufs_bsg_probe(struct ufs_hba *hba);
#else
static inline void ufs_bsg_remove(struct ufs_hba *hba) {}
static inline int ufs_bsg_probe(struct ufs_hba *hba) {return 0; }
#endif

#endif /* UFS_BSG_H */
