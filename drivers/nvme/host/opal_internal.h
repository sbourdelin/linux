/*
 * Copyright © 2016 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 * Authors:
 *    Rafael Antognolli <rafael.antognolli@intel.com>
 */

#ifndef _NVME_OPAL_INTERNAL_H
#define _NVME_OPAL_INTERNAL_H

#include "opal.h"

#define DTAERROR_NO_METHOD_STATUS 0x89

/* User IDs used in the TCG storage SSCs */
static const uint8_t OPALUID[][8] = {
	/* users */
	{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff}, /* session management  */
	{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01 }, /* special "thisSP" syntax */
	{ 0x00, 0x00, 0x02, 0x05, 0x00, 0x00, 0x00, 0x01 }, /* Administrative SP */
	{ 0x00, 0x00, 0x02, 0x05, 0x00, 0x00, 0x00, 0x02 }, /* Locking SP */
	{ 0x00, 0x00, 0x02, 0x05, 0x00, 0x01, 0x00, 0x01 }, /* ENTERPRISE Locking SP  */
	{ 0x00, 0x00, 0x00, 0x09, 0x00, 0x00, 0x00, 0x01 }, /* anybody */
	{ 0x00, 0x00, 0x00, 0x09, 0x00, 0x00, 0x00, 0x06 }, /* SID */
	{ 0x00, 0x00, 0x00, 0x09, 0x00, 0x01, 0x00, 0x01 }, /* ADMIN1 */
	{ 0x00, 0x00, 0x00, 0x09, 0x00, 0x03, 0x00, 0x01 }, /* USER1 */
	{ 0x00, 0x00, 0x00, 0x09, 0x00, 0x03, 0x00, 0x02 }, /* USER2 */
	{ 0x00, 0x00, 0x00, 0x09, 0x00, 0x01, 0xff, 0x01 }, /* PSID user */
	{ 0x00, 0x00, 0x00, 0x09, 0x00, 0x00, 0x80, 0x01 }, /* BandMaster 0 */
	{ 0x00, 0x00, 0x00, 0x09, 0x00, 0x00, 0x84, 0x01 }, /* EraseMaster */
	/* tables */
	{ 0x00, 0x00, 0x08, 0x02, 0x00, 0x00, 0x00, 0x01 }, /* Locking_GlobalRange */
	{ 0x00, 0x00, 0x00, 0x08, 0x00, 0x03, 0xE0, 0x01 }, /* ACE_Locking_Range_Set_RdLocked UID */
	{ 0x00, 0x00, 0x00, 0x08, 0x00, 0x03, 0xE8, 0x01 }, /* ACE_Locking_Range_Set_WrLocked UID */
	{ 0x00, 0x00, 0x08, 0x03, 0x00, 0x00, 0x00, 0x01 }, /* MBR Control */
	{ 0x00, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00, 0x00 }, /* Shadow MBR */
	{ 0x00, 0x00, 0x00, 0x09, 0x00, 0x00, 0x00, 0x00}, /* AUTHORITY_TABLE */
	{ 0x00, 0x00, 0x00, 0x0B, 0x00, 0x00, 0x00, 0x00}, /* C_PIN_TABLE */
	{ 0x00, 0x00, 0x08, 0x01, 0x00, 0x00, 0x00, 0x01 }, /* OPAL Locking Info */
	{ 0x00, 0x00, 0x08, 0x01, 0x00, 0x00, 0x00, 0x00 }, /* Enterprise Locking Info */
	/* C_PIN_TABLE object ID's */
	{ 0x00, 0x00, 0x00, 0x0B, 0x00, 0x00, 0x84, 0x02}, /* C_PIN_MSID */
	{ 0x00, 0x00, 0x00, 0x0B, 0x00, 0x00, 0x00, 0x01}, /* C_PIN_SID */
	{ 0x00, 0x00, 0x00, 0x0B, 0x00, 0x01, 0x00, 0x01}, /* C_PIN_ADMIN1 */
	/* half UID's (only first 4 bytes used) */
	{ 0x00, 0x00, 0x0C, 0x05, 0xff, 0xff, 0xff, 0xff }, /* Half-UID – Authority_object_ref */
	{ 0x00, 0x00, 0x04, 0x0E, 0xff, 0xff, 0xff, 0xff }, /* Half-UID – Boolean ACE */
	/* special value for omitted optional parameter */
	{ 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff}, /* HEXFF for omitted */
};

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
 */
static const uint8_t OPALMETHOD[][8] = {
	{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0x01 }, /* Properties */
	{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0x02 }, /* STARTSESSION */
	{ 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x02, 0x02 }, /* Revert */
	{ 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x02, 0x03 }, /* Activate */
	{ 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x06 }, /* Enterprise Get */
	{ 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x07 }, /* Enterprise Set */
	{ 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x08 }, /* NEXT */
	{ 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x0c }, /* Enterprise Authenticate */
	{ 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x0d }, /* GetACL */
	{ 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x10 }, /* GenKey */
	{ 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x11 }, /* revertSP */
	{ 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x16 }, /* Get */
	{ 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x17 }, /* Set */
	{ 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x1c }, /* Authenticate */
	{ 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x06, 0x01 }, /* Random */
	{ 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x08, 0x03 }, /* Erase */
};

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

enum OPAL_RESPONSE_TOKEN {
	OPAL_DTA_TOKENID_BYTESTRING = 0xe0,
	OPAL_DTA_TOKENID_SINT = 0xe1,
	OPAL_DTA_TOKENID_UINT = 0xe2,
	OPAL_DTA_TOKENID_TOKEN = 0xe3, /* actual token is returned */
	OPAL_DTA_TOKENID_INVALID = 0X0
};

/*
 * TODO: Split this enum
 * Apparently these different types of enums were grouped together so they
 * could be matched in a single C++ method. Since we don't have polymorphism in
 * C, and we are going to cast it to uint8_t anyway, there's no need to keep
 * these enums, and we can split them into separate declarations.
 */
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
	OPAL_HOSTPROPERTIES =0x00,
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

/*
 * Useful short atoms.
 */
enum OPAL_SHORT_ATOM {
	OPAL_SHORT_UINT_3 = 0x83,
	OPAL_SHORT_BYTESTRING4 = 0xa4,
	OPAL_SHORT_BYTESTRING8 = 0xa8,
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
 * fields that are NOT really numeric are defined as uint8_t[] to
 * help reduce the endianess issues
 */

/* Comm Packet (header) for transmissions. */
struct opal_compacket {
	uint32_t reserved0;
	uint8_t extendedComID[4];
	uint32_t outstandingData;
	uint32_t minTransfer;
	uint32_t length;
};

/* Packet structure. */
struct opal_packet {
	uint32_t TSN;
	uint32_t HSN;
	uint32_t seq_number;
	uint16_t reserved0;
	uint16_t ack_type;
	uint32_t acknowledgement;
	uint32_t length;
};

/* Data sub packet header */
struct opal_data_subpacket {
	uint8_t reserved0[6];
	uint16_t kind;
	uint32_t length;
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
 */
struct d0_header {
	uint32_t length; /* the length of the header 48 in 2.00.100 */
	uint32_t revision; /**< revision of the header 1 in 2.00.100 */
	uint32_t reserved01;
	uint32_t reserved02;
	/*
	 * the remainder of the structure is vendor specific and will not be
	 * addressed now
	 */
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
	uint8_t supported_features;
	/*
	 * bytes 5 through 15 are reserved, but we represent the first 3 as
	 * uint8_t to keep the other two 32bits integers aligned.
	 */
	uint8_t reserved01[3];
	uint32_t reserved02;
	uint32_t reserved03;
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
	uint8_t supported_features;
	/*
	 * bytes 5 through 15 are reserved, but we represent the first 3 as
	 * uint8_t to keep the other two 32bits integers aligned.
	 */
	uint8_t reserved01[3];
	uint32_t reserved02;
	uint32_t reserved03;
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
	 * reserved01:
	 * bits 1-6: reserved
	 * bit 0: align
	 */
	uint8_t reserved01;
	uint8_t reserved02[7];
	uint32_t logical_block_size;
	uint64_t alignment_granularity;
	uint64_t lowest_aligned_lba;
};

/*
 * Enterprise SSC Feature
 *
 * code == 0x0100
 */
struct d0_enterprise_ssc {
	uint16_t baseComID;
	uint16_t numComIDs;
	/* range_crossing:
	 * bits 1-6: reserved
	 * bit 0: range crossing
	 */
	uint8_t range_crossing;
	uint8_t reserved01;
	uint16_t reserved02;
	uint32_t reserved03;
	uint32_t reserved04;
};

/*
 * Opal V1 feature
 *
 * code == 0x0200
 */
struct d0_opal_v100 {
	uint16_t baseComID;
	uint16_t numComIDs;
};

/*
 * Single User Mode feature
 *
 * code == 0x0201
 */
struct d0_single_user_mode {
	uint32_t num_locking_objects;
	/* reserved01:
	 * bit 0: any
	 * bit 1: all
	 * bit 2: policy
	 * bits 3-7: reserved
	 */
	uint8_t reserved01;
	uint8_t reserved02;
	uint16_t reserved03;
	uint32_t reserved04;
};

/*
 * Additonal Datastores feature
 *
 * code == 0x0202
 */
struct d0_datastore_table {
	uint16_t reserved01;
	uint16_t max_tables;
	uint32_t max_size_tables;
	uint32_t table_size_alignment;
};

/*
 * OPAL 2.0 feature
 *
 * code == 0x0203
 */
struct d0_opal_v200 {
	uint16_t baseComID;
	uint16_t numComIDs;
	/* range_crossing:
	 * bits 1-6: reserved
	 * bit 0: range crossing
	 */
	uint8_t range_crossing;
	/* num_locking_admin_auth:
	 * not aligned to 16 bits, so use two uint8_t.
	 * stored in big endian:
	 * 0: MSB
	 * 1: LSB
	 */
	uint8_t num_locking_admin_auth[2];
	/* num_locking_user_auth:
	 * not aligned to 16 bits, so use two uint8_t.
	 * stored in big endian:
	 * 0: MSB
	 * 1: LSB
	 */
	uint8_t num_locking_user_auth[2];
	uint8_t initialPIN;
	uint8_t revertedPIN;
	uint8_t reserved01;
	uint32_t reserved02;
};

/* Union of features used to parse the discovery 0 response */
struct d0_features {
	uint16_t code;
	/*
	 * r_version bits:
	 * bits 4-7: version
	 * bits 0-3: reserved
	 */
	uint8_t r_version;
	uint8_t length;
	uint8_t features[];
};

#endif /* _NVME_OPAL_INTERNAL_H */
