/*
 * include/uapi/linux/if_macsec.h - MACsec device
 *
 * Copyright (c) 2015 Sabrina Dubroca <sd@queasysnail.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef _UAPI_MACSEC_H
#define _UAPI_MACSEC_H

#include <linux/types.h>

#define MACSEC_GENL_NAME "macsec"
#define MACSEC_GENL_VERSION 1

#define MACSEC_MAX_KEY_LEN 128


#define DEFAULT_CIPHER_NAME "GCM-AES-128"
#define DEFAULT_CIPHER_ID   0x0080020001000001ULL
#define DEFAULT_CIPHER_ALT  0x0080C20001000001ULL

#define MACSEC_MIN_ICV_LEN 8
#define MACSEC_MAX_ICV_LEN 32

enum macsec_attrs {
	MACSEC_ATTR_UNSPEC,
	MACSEC_ATTR_IFINDEX,
	MACSEC_ATTR_SCI,
	MACSEC_ATTR_ENCODING_SA,
	MACSEC_ATTR_WINDOW,
	MACSEC_ATTR_CIPHER_SUITE,
	MACSEC_ATTR_ICV_LEN,
	MACSEC_TXSA_LIST,
	MACSEC_RXSC_LIST,
	MACSEC_TXSC_STATS,
	MACSEC_SECY_STATS,
	MACSEC_ATTR_PROTECT,
	MACSEC_ATTR_REPLAY,
	MACSEC_ATTR_OPER,
	MACSEC_ATTR_VALIDATE,
	MACSEC_ATTR_ENCRYPT,
	MACSEC_ATTR_INC_SCI,
	MACSEC_ATTR_ES,
	MACSEC_ATTR_SCB,
	__MACSEC_ATTR_END,
	NUM_MACSEC_ATTR = __MACSEC_ATTR_END,
	MACSEC_ATTR_MAX = __MACSEC_ATTR_END - 1,
};

enum macsec_sa_list_attrs {
	MACSEC_SA_LIST_UNSPEC,
	MACSEC_SA,
	__MACSEC_ATTR_SA_LIST_MAX,
	MACSEC_ATTR_SA_LIST_MAX = __MACSEC_ATTR_SA_LIST_MAX - 1,
};

enum macsec_rxsc_list_attrs {
	MACSEC_RXSC_LIST_UNSPEC,
	MACSEC_RXSC,
	__MACSEC_ATTR_RXSC_LIST_MAX,
	MACSEC_ATTR_RXSC_LIST_MAX = __MACSEC_ATTR_RXSC_LIST_MAX - 1,
};

enum macsec_rxsc_attrs {
	MACSEC_ATTR_SC_UNSPEC,
	/* use the same value to allow generic helper function, see
	 * get_*_from_nl in drivers/net/macsec.c */
	MACSEC_ATTR_SC_IFINDEX = MACSEC_ATTR_IFINDEX,
	MACSEC_ATTR_SC_SCI = MACSEC_ATTR_SCI,
	MACSEC_ATTR_SC_ACTIVE,
	MACSEC_RXSA_LIST,
	MACSEC_RXSC_STATS,
	__MACSEC_ATTR_SC_END,
	NUM_MACSEC_SC_ATTR = __MACSEC_ATTR_SC_END,
	MACSEC_ATTR_SC_MAX = __MACSEC_ATTR_SC_END - 1,
};

enum macsec_sa_attrs {
	MACSEC_ATTR_SA_UNSPEC,
	/* use the same value to allow generic helper function, see
	 * get_*_from_nl in drivers/net/macsec.c */
	MACSEC_ATTR_SA_IFINDEX = MACSEC_ATTR_IFINDEX,
	MACSEC_ATTR_SA_SCI = MACSEC_ATTR_SCI,
	MACSEC_ATTR_SA_AN,
	MACSEC_ATTR_SA_ACTIVE,
	MACSEC_ATTR_SA_PN,
	MACSEC_ATTR_SA_KEY,
	MACSEC_ATTR_SA_KEYID,
	MACSEC_SA_STATS,
	__MACSEC_ATTR_SA_END,
	NUM_MACSEC_SA_ATTR = __MACSEC_ATTR_SA_END,
	MACSEC_ATTR_SA_MAX = __MACSEC_ATTR_SA_END - 1,
};

enum macsec_nl_commands {
	MACSEC_CMD_GET_TXSC,
	MACSEC_CMD_ADD_RXSC,
	MACSEC_CMD_DEL_RXSC,
	MACSEC_CMD_UPD_RXSC,
	MACSEC_CMD_ADD_TXSA,
	MACSEC_CMD_DEL_TXSA,
	MACSEC_CMD_UPD_TXSA,
	MACSEC_CMD_ADD_RXSA,
	MACSEC_CMD_DEL_RXSA,
	MACSEC_CMD_UPD_RXSA,
};

enum validation_type {
	MACSEC_VALIDATE_DISABLED = 0,
	MACSEC_VALIDATE_CHECK = 1,
	MACSEC_VALIDATE_STRICT = 2,
	__MACSEC_VALIDATE_MAX,
};
#define MACSEC_VALIDATE_MAX (__MACSEC_VALIDATE_MAX - 1)

struct macsec_rx_sc_stats {
	__u64 InOctetsValidated;
	__u64 InOctetsDecrypted;
	__u64 InPktsUnchecked;
	__u64 InPktsDelayed;
	__u64 InPktsOK;
	__u64 InPktsInvalid;
	__u64 InPktsLate;
	__u64 InPktsNotValid;
	__u64 InPktsNotUsingSA;
	__u64 InPktsUnusedSA;
};

struct macsec_rx_sa_stats {
	__u32 InPktsOK;
	__u32 InPktsInvalid;
	__u32 InPktsNotValid;
	__u32 InPktsNotUsingSA;
	__u32 InPktsUnusedSA;
};

struct macsec_tx_sc_stats {
	__u64 OutPktsProtected;
	__u64 OutPktsEncrypted;
	__u64 OutOctetsProtected;
	__u64 OutOctetsEncrypted;
};

struct macsec_tx_sa_stats {
	__u32 OutPktsProtected;
	__u32 OutPktsEncrypted;
};

struct macsec_dev_stats {
	__u64 OutPktsUntagged;
	__u64 InPktsUntagged;
	__u64 OutPktsTooLong;
	__u64 InPktsNoTag;
	__u64 InPktsBadTag;
	__u64 InPktsUnknownSCI;
	__u64 InPktsNoSCI;
	__u64 InPktsOverrun;
};

#endif /* _UAPI_MACSEC_H */
