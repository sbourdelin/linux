/*
 * Copyright © 2016 Intel Corporation
 *
 * Authors:
 *    Rafael Antognolli <rafael.antognolli@intel.com>
 *    Scott  Bauer      <scott.bauer@intel.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 */

#ifndef _NVME_OPAL_INTERNAL_H
#define _NVME_OPAL_INTERNAL_H

#include <linux/key-type.h>
#include <keys/user-type.h>

#define DTAERROR_NO_METHOD_STATUS 0x89
#define GENERIC_HOST_SESSION_NUM 0x41



/*
 * Derived from:
 * TCG_Storage_Architecture_Core_Spec_v2.01_r1.00
 * Section: 5.1.5 Method Status Codes
 */
static const char *opal_errors[] = {
	"Success",
	"Not Authorized",
	"Unknown Error",
	"SP Busy",
	"SP Failed",
	"SP Disabled",
	"SP Frozen",
	"No Sessions Available",
	"Uniqueness Conflict",
	"Insufficient Space",
	"Insufficient Rows",
	"Invalid Function",
	"Invalid Parameter",
	"Invalid Reference",
	"Unknown Error",
	"TPER Malfunction",
	"Transaction Failure",
	"Response Overflow",
	"Authority Locked Out",
};

static const char *opal_error_to_human(int error)
{
	if (error == 0x3f)
		return "Failed";

	if (error >= ARRAY_SIZE(opal_errors) || error < 0)
		return "Unknown Error";

	return opal_errors[error];
}

/*
 * User IDs used in the TCG storage SSCs
 * Derived from: TCG_Storage_Architecture_Core_Spec_v2.01_r1.00
 * Section: 6.3 Assigned UIDs
 */
static const u8 OPALUID[][8] = {
	/* users */

	/* session management  */
	{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff},
	/* special "thisSP" syntax */
	{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01 },
	/* Administrative SP */
	{ 0x00, 0x00, 0x02, 0x05, 0x00, 0x00, 0x00, 0x01 },
	/* Locking SP */
	{ 0x00, 0x00, 0x02, 0x05, 0x00, 0x00, 0x00, 0x02 },
	/* ENTERPRISE Locking SP  */
	{ 0x00, 0x00, 0x02, 0x05, 0x00, 0x01, 0x00, 0x01 },
	/* anybody */
	{ 0x00, 0x00, 0x00, 0x09, 0x00, 0x00, 0x00, 0x01 },
	/* SID */
	{ 0x00, 0x00, 0x00, 0x09, 0x00, 0x00, 0x00, 0x06 },
	/* ADMIN1 */
	{ 0x00, 0x00, 0x00, 0x09, 0x00, 0x01, 0x00, 0x01 },
	/* USER1 */
	{ 0x00, 0x00, 0x00, 0x09, 0x00, 0x03, 0x00, 0x01 },
	/* USER2 */
	{ 0x00, 0x00, 0x00, 0x09, 0x00, 0x03, 0x00, 0x02 },
	/* PSID user */
	{ 0x00, 0x00, 0x00, 0x09, 0x00, 0x01, 0xff, 0x01 },
	/* BandMaster 0 */
	{ 0x00, 0x00, 0x00, 0x09, 0x00, 0x00, 0x80, 0x01 },
	 /* EraseMaster */
	{ 0x00, 0x00, 0x00, 0x09, 0x00, 0x00, 0x84, 0x01 },

	/* tables */

	/* Locking_GlobalRange */
	{ 0x00, 0x00, 0x08, 0x02, 0x00, 0x00, 0x00, 0x01 },
	/* ACE_Locking_Range_Set_RdLocked UID */
	{ 0x00, 0x00, 0x00, 0x08, 0x00, 0x03, 0xE0, 0x01 },
	/* ACE_Locking_Range_Set_WrLocked UID */
	{ 0x00, 0x00, 0x00, 0x08, 0x00, 0x03, 0xE8, 0x01 },
	/* MBR Control */
	{ 0x00, 0x00, 0x08, 0x03, 0x00, 0x00, 0x00, 0x01 },
	/* Shadow MBR */
	{ 0x00, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00, 0x00 },
	/* AUTHORITY_TABLE */
	{ 0x00, 0x00, 0x00, 0x09, 0x00, 0x00, 0x00, 0x00},
	/* C_PIN_TABLE */
	{ 0x00, 0x00, 0x00, 0x0B, 0x00, 0x00, 0x00, 0x00},
	/* OPAL Locking Info */
	{ 0x00, 0x00, 0x08, 0x01, 0x00, 0x00, 0x00, 0x01 },
	/* Enterprise Locking Info */
	{ 0x00, 0x00, 0x08, 0x01, 0x00, 0x00, 0x00, 0x00 },

	/* C_PIN_TABLE object ID's */

	/* C_PIN_MSID */
	{ 0x00, 0x00, 0x00, 0x0B, 0x00, 0x00, 0x84, 0x02},
	/* C_PIN_SID */
	{ 0x00, 0x00, 0x00, 0x0B, 0x00, 0x00, 0x00, 0x01},
	 /* C_PIN_ADMIN1 */
	{ 0x00, 0x00, 0x00, 0x0B, 0x00, 0x01, 0x00, 0x01},

	/* half UID's (only first 4 bytes used) */

	/* Half-UID – Authority_object_ref */
	{ 0x00, 0x00, 0x0C, 0x05, 0xff, 0xff, 0xff, 0xff },
	/* Half-UID – Boolean ACE */
	{ 0x00, 0x00, 0x04, 0x0E, 0xff, 0xff, 0xff, 0xff },

	/* special value for omitted optional parameter */

	/* HEXFF for omitted */
	{ 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff},
};
static const size_t OPAL_UID_LENGTH = 8;
static const size_t OPAL_MSID_KEYLEN = 15;
static const size_t OPAL_UID_LENGTH_HALF = 4;


/* Enum to index OPALUID array */
enum OPAL_UID {
	/* users */
	OPAL_SMUID_UID,
	OPAL_THISSP_UID,
	OPAL_ADMINSP_UID,
	OPAL_LOCKINGSP_UID,
	OPAL_ENTERPRISE_LOCKINGSP_UID,
	OPAL_ANYBODY_UID,
	OPAL_SID_UID,
	OPAL_ADMIN1_UID,
	OPAL_USER1_UID,
	OPAL_USER2_UID,
	OPAL_PSID_UID,
	OPAL_ENTERPRISE_BANDMASTER0_UID,
	OPAL_ENTERPRISE_ERASEMASTER_UID,
	/* tables */
	OPAL_LOCKINGRANGE_GLOBAL,
	OPAL_LOCKINGRANGE_ACE_RDLOCKED,
	OPAL_LOCKINGRANGE_ACE_WRLOCKED,
	OPAL_MBRCONTROL,
	OPAL_MBR,
	OPAL_AUTHORITY_TABLE,
	OPAL_C_PIN_TABLE,
	OPAL_LOCKING_INFO_TABLE,
	OPAL_ENTERPRISE_LOCKING_INFO_TABLE,
	/* C_PIN_TABLE object ID's */
	OPAL_C_PIN_MSID,
	OPAL_C_PIN_SID,
	OPAL_C_PIN_ADMIN1,
	/* half UID's (only first 4 bytes used) */
	OPAL_HALF_UID_AUTHORITY_OBJ_REF,
	OPAL_HALF_UID_BOOLEAN_ACE,
	/* omitted optional parameter */
	OPAL_UID_HEXFF,
};

/*
 * TCG Storage SSC Methods.
 * Derived from: TCG_Storage_Architecture_Core_Spec_v2.01_r1.00
 * Section: 6.3 Assigned UIDs
 */
static const u8 OPALMETHOD[][8] = {
	/* Properties */
	{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0x01 },
	/* STARTSESSION */
	{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0x02 },
	/* Revert */
	{ 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x02, 0x02 },
	/* Activate */
	{ 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x02, 0x03 },
	/* Enterprise Get */
	{ 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x06 },
	/* Enterprise Set */
	{ 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x07 },
	/* NEXT */
	{ 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x08 },
	/* Enterprise Authenticate */
	{ 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x0c },
	/* GetACL */
	{ 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x0d },
	/* GenKey */
	{ 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x10 },
	/* revertSP */
	{ 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x11 },
	/* Get */
	{ 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x16 },
	/* Set */
	{ 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x17 },
	/* Authenticate */
	{ 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x1c },
	/* Random */
	{ 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x06, 0x01 },
	/* Erase */
	{ 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x08, 0x03 },
};
static const size_t OPAL_METHOD_LENGTH = 8;

/* Enum for indexing the OPALMETHOD array */
enum OPAL_METHOD {
	OPAL_PROPERTIES,
	OPAL_STARTSESSION,
	OPAL_REVERT,
	OPAL_ACTIVATE,
	OPAL_EGET,
	OPAL_ESET,
	OPAL_NEXT,
	OPAL_EAUTHENTICATE,
	OPAL_GETACL,
	OPAL_GENKEY,
	OPAL_REVERTSP,
	OPAL_GET,
	OPAL_SET,
	OPAL_AUTHENTICATE,
	OPAL_RANDOM,
	OPAL_ERASE,
};

/*
 * Token defs derived from:
 * TCG_Storage_Architecture_Core_Spec_v2.01_r1.00
 * 3.2.2 Data Stream Encoding
 */
enum OPAL_RESPONSE_TOKEN {
	OPAL_DTA_TOKENID_BYTESTRING = 0xe0,
	OPAL_DTA_TOKENID_SINT = 0xe1,
	OPAL_DTA_TOKENID_UINT = 0xe2,
	OPAL_DTA_TOKENID_TOKEN = 0xe3, /* actual token is returned */
	OPAL_DTA_TOKENID_INVALID = 0X0
};

enum OPAL_TOKEN {
	/* Boolean */
	OPAL_TRUE = 0x01,
	OPAL_FALSE = 0x00,
	OPAL_BOOLEAN_EXPR = 0x03,
	/* cellblocks */
	OPAL_TABLE = 0x00,
	OPAL_STARTROW = 0x01,
	OPAL_ENDROW = 0x02,
	OPAL_STARTCOLUMN = 0x03,
	OPAL_ENDCOLUMN = 0x04,
	OPAL_VALUES = 0x01,
	/* authority table */
	OPAL_PIN = 0x03,
	/* locking tokens */
	OPAL_RANGESTART = 0x03,
	OPAL_RANGELENGTH = 0x04,
	OPAL_READLOCKENABLED = 0x05,
	OPAL_WRITELOCKENABLED = 0x06,
	OPAL_READLOCKED = 0x07,
	OPAL_WRITELOCKED = 0x08,
	OPAL_ACTIVEKEY = 0x0A,
	/* locking info table */
	OPAL_MAXRANGES = 0x04,
	 /* mbr control */
	OPAL_MBRENABLE = 0x01,
	OPAL_MBRDONE = 0x02,
	/* properties */
	OPAL_HOSTPROPERTIES = 0x00,
	/* atoms */
	OPAL_STARTLIST = 0xf0,
	OPAL_ENDLIST = 0xf1,
	OPAL_STARTNAME = 0xf2,
	OPAL_ENDNAME = 0xf3,
	OPAL_CALL = 0xf8,
	OPAL_ENDOFDATA = 0xf9,
	OPAL_ENDOFSESSION = 0xfa,
	OPAL_STARTTRANSACTON = 0xfb,
	OPAL_ENDTRANSACTON = 0xfC,
	OPAL_EMPTYATOM = 0xff,
	OPAL_WHERE = 0x00,
};

/* Useful tiny atoms.
 * Useful for table columns etc
 */
enum OPAL_TINY_ATOM {
	OPAL_TINY_UINT_00 = 0x00,
	OPAL_TINY_UINT_01 = 0x01,
	OPAL_TINY_UINT_02 = 0x02,
	OPAL_TINY_UINT_03 = 0x03,
	OPAL_TINY_UINT_04 = 0x04,
	OPAL_TINY_UINT_05 = 0x05,
	OPAL_TINY_UINT_06 = 0x06,
	OPAL_TINY_UINT_07 = 0x07,
	OPAL_TINY_UINT_08 = 0x08,
	OPAL_TINY_UINT_09 = 0x09,
	OPAL_TINY_UINT_10 = 0x0a,
	OPAL_TINY_UINT_11 = 0x0b,
	OPAL_TINY_UINT_12 = 0x0c,
	OPAL_TINY_UINT_13 = 0x0d,
	OPAL_TINY_UINT_14 = 0x0e,
	OPAL_TINY_UINT_15 = 0x0f,
};

enum OPAL_ATOM_WIDTH {
	OPAL_WIDTH_TINY,
	OPAL_WIDTH_SHORT,
	OPAL_WIDTH_MEDIUM,
	OPAL_WIDTH_LONG,
	OPAL_WIDTH_TOKEN
};

/* Locking state for a locking range */
enum OPAL_LOCKINGSTATE {
	OPAL_LOCKING_READWRITE = 0x01,
	OPAL_LOCKING_READONLY = 0x02,
	OPAL_LOCKING_LOCKED = 0x03,
};

/*
 * Structures to build and decode the Opal SSC messages
 * fields that are NOT really numeric are defined as u8[] to
 * help reduce the endianness issues
 */

/* Packets derived from:
 * TCG_Storage_Architecture_Core_Spec_v2.01_r1.00
 * Secion: 3.2.3 ComPackets, Packets & Subpackets
 */

/* Comm Packet (header) for transmissions. */
struct opal_compacket {
	u32 reserved0;
	u8 extendedComID[4];
	u32 outstandingData;
	u32 minTransfer;
	u32 length;
};

/* Packet structure. */
struct opal_packet {
	u32 TSN;
	u32 HSN;
	u32 seq_number;
	u16 reserved0;
	u16 ack_type;
	u32 acknowledgment;
	u32 length;
};

/* Data sub packet header */
struct opal_data_subpacket {
	u8 reserved0[6];
	u16 kind;
	u32 length;
};

/* header of a response */
struct opal_header {
	struct opal_compacket cp;
	struct opal_packet pkt;
	struct opal_data_subpacket subpkt;
};

#define FC_TPER       0x0001
#define FC_LOCKING    0x0002
#define FC_GEOMETRY   0x0003
#define FC_ENTERPRISE 0x0100
#define FC_DATASTORE  0x0202
#define FC_SINGLEUSER 0x0201
#define FC_OPALV100   0x0200
#define FC_OPALV200   0x0203

/*
 * The Discovery 0 Header. As defined in
 * Opal SSC Documentation
 * Section: 3.3.5 Capability Discovery
 */
struct d0_header {
	u32 length; /* the length of the header 48 in 2.00.100 */
	u32 revision; /**< revision of the header 1 in 2.00.100 */
	u32 reserved01;
	u32 reserved02;
	/*
	 * the remainder of the structure is vendor specific and will not be
	 * addressed now
	 */
	u8 ignored[32];
};

/*
 * TPer Feature Descriptor. Contains flags indicating support for the
 * TPer features described in the OPAL specification. The names match the
 * OPAL terminology
 *
 * code == 0x001 in 2.00.100
 */
struct d0_tper_features {
	/*
	 * supported_features bits:
	 * bit 7: reserved
	 * bit 6: com ID management
	 * bit 5: reserved
	 * bit 4: streaming support
	 * bit 3: buffer management
	 * bit 2: ACK/NACK
	 * bit 1: async
	 * bit 0: sync
	 */
	u8 supported_features;
	/*
	 * bytes 5 through 15 are reserved, but we represent the first 3 as
	 * u8 to keep the other two 32bits integers aligned.
	 */
	u8 reserved01[3];
	u32 reserved02;
	u32 reserved03;
};

/*
 * Locking Feature Descriptor. Contains flags indicating support for the
 * locking features described in the OPAL specification. The names match the
 * OPAL terminology
 *
 * code == 0x0002 in 2.00.100
 */
struct d0_locking_features {
	/*
	 * supported_features bits:
	 * bits 6-7: reserved
	 * bit 5: MBR done
	 * bit 4: MBR enabled
	 * bit 3: media encryption
	 * bit 2: locked
	 * bit 1: locking enabled
	 * bit 0: locking supported
	 */
	u8 supported_features;
	/*
	 * bytes 5 through 15 are reserved, but we represent the first 3 as
	 * u8 to keep the other two 32bits integers aligned.
	 */
	u8 reserved01[3];
	u32 reserved02;
	u32 reserved03;
};

/*
 * Geometry Feature Descriptor. Contains flags indicating support for the
 * geometry features described in the OPAL specification. The names match the
 * OPAL terminology
 *
 * code == 0x0003 in 2.00.100
 */
struct d0_geometry_features {
	/*
	 * skip 32 bits from header, needed to align the struct to 64 bits.
	 */
	u8 header[4];
	/*
	 * reserved01:
	 * bits 1-6: reserved
	 * bit 0: align
	 */
	u8 reserved01;
	u8 reserved02[7];
	u32 logical_block_size;
	u64 alignment_granularity;
	u64 lowest_aligned_lba;
};

/*
 * Enterprise SSC Feature
 *
 * code == 0x0100
 */
struct d0_enterprise_ssc {
	u16 baseComID;
	u16 numComIDs;
	/* range_crossing:
	 * bits 1-6: reserved
	 * bit 0: range crossing
	 */
	u8 range_crossing;
	u8 reserved01;
	u16 reserved02;
	u32 reserved03;
	u32 reserved04;
};

/*
 * Opal V1 feature
 *
 * code == 0x0200
 */
struct d0_opal_v100 {
	u16 baseComID;
	u16 numComIDs;
};

/*
 * Single User Mode feature
 *
 * code == 0x0201
 */
struct d0_single_user_mode {
	u32 num_locking_objects;
	/* reserved01:
	 * bit 0: any
	 * bit 1: all
	 * bit 2: policy
	 * bits 3-7: reserved
	 */
	u8 reserved01;
	u8 reserved02;
	u16 reserved03;
	u32 reserved04;
};

/*
 * Additonal Datastores feature
 *
 * code == 0x0202
 */
struct d0_datastore_table {
	u16 reserved01;
	u16 max_tables;
	u32 max_size_tables;
	u32 table_size_alignment;
};

/*
 * OPAL 2.0 feature
 *
 * code == 0x0203
 */
struct d0_opal_v200 {
	u16 baseComID;
	u16 numComIDs;
	/* range_crossing:
	 * bits 1-6: reserved
	 * bit 0: range crossing
	 */
	u8 range_crossing;
	/* num_locking_admin_auth:
	 * not aligned to 16 bits, so use two u8.
	 * stored in big endian:
	 * 0: MSB
	 * 1: LSB
	 */
	u8 num_locking_admin_auth[2];
	/* num_locking_user_auth:
	 * not aligned to 16 bits, so use two u8.
	 * stored in big endian:
	 * 0: MSB
	 * 1: LSB
	 */
	u8 num_locking_user_auth[2];
	u8 initialPIN;
	u8 revertedPIN;
	u8 reserved01;
	u32 reserved02;
};

/* Union of features used to parse the discovery 0 response */
struct d0_features {
	u16 code;
	/*
	 * r_version bits:
	 * bits 4-7: version
	 * bits 0-3: reserved
	 */
	u8 r_version;
	u8 length;
	u8 features[];
};

struct key *request_user_key(const char *master_desc, const u8 **master_key,
			     size_t *master_keylen);

#endif /* _NVME_OPAL_INTERNAL_H */
