/*
 * Driver for KeyStream wireless LAN cards.
 *
 * Copyright (C) 2005-2008 KeyStream Corp.
 * Copyright (C) 2009 Renesas Technology Corp.
 * Copyright (C) 2017 Tobin C. Harding.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */

#ifndef _KS7010_EAP_H
#define _KS7010_EAP_H

/*
 * FIXME does the kernel already define these?
 */

/**
 * enum protocol_id - Ethernet frame protocol identity.
 * @PROTO_ID_EAPOL: EAP over LAN (802.1X)
 * @PROTO_ID_IP: Internet Protocol version 4
 * @PROTO_ID_ARP: Address resolution protocol
 */
enum protocol_id {
	PROTO_ID_EAPOL	= 0x888e,
	PROTO_ID_IP	= 0x0800,
	PROTO_ID_ARP	= 0x0806
};

#define OUI_SIZE 3

/**
 * struct snap_hdr - EAPOL on 802.11 SNAP header.
 * @dsap: Destination Service Access Point.
 * @ssap: Source Service Access Point.
 * @cntl: Control, set to 0x03 for Unnumbered Information.
 * @oui: Organizationally Unique Identifier.
 */
struct snap_hdr {
	u8 dsap;
	u8 ssap;
	u8 cntl;
	u8 oui[OUI_SIZE];
} __packed;

/**
 * struct fil_eap_hdr - Firmware Interface Layer EAP header
 * @da: Destination MAC address.
 * @sa: Source MAC address.
 * @dsap: Destination Service Access Point.
 * @ssap: Source Service Access Point.
 * @cntl: Control, set to 0x03 for Unnumbered Information.
 * @oui: Organizationally Unique Identifier.
 * @type: Ethernet protocol type.
 */
struct fil_eap_hdr {
	u8 *da;
	u8 *sa;
	u8 dsap;
	u8 ssap;
	u8 cntl;
	u8 oui[OUI_SIZE];
	__be16 type;
} __packed;

#endif	/* _KS7010_EAP_H */
