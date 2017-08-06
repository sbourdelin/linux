/*
 *  FC Transport Netlink Interface
 *
 *  Copyright (C) 2006   James Smart, Emulex Corporation
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */
#ifndef SCSI_NETLINK_FC_H
#define SCSI_NETLINK_FC_H

#include <scsi/scsi_netlink.h>

/*
 * This file intended to be included by both kernel and user space
 */

/*
 * FC Transport Message Types
 */
	/* kernel -> user */
#define FC_NL_ASYNC_EVENT			0x0100
	/* user -> kernel */
/* none */


/*
 * Message Structures :
 */

/* macro to round up message lengths to 8byte boundary */
#define FC_NL_MSGALIGN(len)		(((len) + 7) & ~7)


/*
 * FC Transport Broadcast Event Message :
 *   FC_NL_ASYNC_EVENT
 *
 * Note: if Vendor Unique message, &event_data will be  start of
 * 	 vendor unique payload, and the length of the payload is
 *       per event_datalen
 *
 * Note: When specifying vendor_id, be sure to read the Vendor Type and ID
 *   formatting requirements specified in scsi_netlink.h
 */
struct fc_nl_event {
	struct scsi_nl_hdr snlh;		/* must be 1st element ! */
	__u64 seconds;
	__u64 vendor_id;
	__u16 host_no;
	__u16 event_datalen;
	__u32 event_num;
	__u32 event_code;
	__u32 event_data;
} __attribute__((aligned(sizeof(__u64))));


#endif /* SCSI_NETLINK_FC_H */

