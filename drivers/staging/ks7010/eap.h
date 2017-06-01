#ifndef _KS7010_EAP_H
#define _KS7010_EAP_H

/*
 * FIXME these headers may be defined in the kernel already?
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

#endif	/* _KS7010_EAP_H */
