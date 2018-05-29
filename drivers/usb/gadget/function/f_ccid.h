/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2018 Marcus Folkesson <marcus.folkesson@gmail.com>
 */

#ifndef F_CCID_H
#define F_CCID_H

#define CCID1_10                0x0110
#define CCID_DECRIPTOR_TYPE     0x21
#define ABDATA_SIZE		512
#define SMART_CARD_DEVICE_CLASS	0x0B

/* CCID Class Specific Request */
#define CCIDGENERICREQ_ABORT                    0x01
#define CCIDGENERICREQ_GET_CLOCK_FREQUENCIES    0x02
#define CCIDGENERICREQ_GET_DATA_RATES           0x03

/* Supported voltages */
#define CCID_VOLTS_AUTO                             0x00
#define CCID_VOLTS_5_0                              0x01
#define CCID_VOLTS_3_0                              0x02
#define CCID_VOLTS_1_8                              0x03

struct f_ccidg_opts {
	struct usb_function_instance func_inst;
	int	minor;
	__u32	features;
	__u32	protocols;
	__u8	pinsupport;
	__u8	nslots;
	__u8	lcdlayout;

	/*
	 * Protect the data form concurrent access by read/write
	 * and create symlink/remove symlink.
	 */
	struct mutex	lock;
	int		refcnt;
};

struct ccidg_bulk_in_header {
	__u8	bMessageType;
	__le32	wLength;
	__u8	bSlot;
	__u8	bSeq;
	__u8	bStatus;
	__u8	bError;
	__u8	bSpecific;
	__u8	abData[ABDATA_SIZE];
	__u8	bSizeToSend;
} __packed;

struct ccidg_bulk_out_header {
	__u8	 bMessageType;
	__le32	 wLength;
	__u8	 bSlot;
	__u8	 bSeq;
	__u8	 bSpecific_0;
	__u8	 bSpecific_1;
	__u8	 bSpecific_2;
	__u8	 APDU[ABDATA_SIZE];
} __packed;

struct ccid_class_descriptor {
	__u8	bLength;
	__u8	bDescriptorType;
	__le16	bcdCCID;
	__u8	bMaxSlotIndex;
	__u8	bVoltageSupport;
	__le32	dwProtocols;
	__le32	dwDefaultClock;
	__le32	dwMaximumClock;
	__u8	bNumClockSupported;
	__le32	dwDataRate;
	__le32	dwMaxDataRate;
	__u8	bNumDataRatesSupported;
	__le32	dwMaxIFSD;
	__le32	dwSynchProtocols;
	__le32	dwMechanical;
	__le32	dwFeatures;
	__le32	dwMaxCCIDMessageLength;
	__u8	bClassGetResponse;
	__u8	bClassEnvelope;
	__le16	wLcdLayout;
	__u8	bPINSupport;
	__u8	bMaxCCIDBusySlots;
} __packed;


#endif
