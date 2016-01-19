/*
 * EMCCTD: EMC Cut-Through HBA Driver for SCSI subsystem.
 *
 * Copyright (C) 2015 by EMC Corporation, Hopkinton, MA.
 *
 * Authors:
 *	fredette, matt <matt.fredette@emc.com>
 *	Pirotte, Serge <serge.pirotte@emc.com>
 *	Singh Animesh <Animesh.Singh@emc.com>
 *	Singhal, Maneesh <Maneesh.Singhal@emc.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#ifndef _EMC_CTD_INTERFACE_H
#define _EMC_CTD_INTERFACE_H
/*
 *
 * DESCRIPTION: Data structures used by the Cut-through subsystem.
 *
 * NOTES: Changes to any of these structures will mean that any Clients that
 *        depend on them will also need to be modified. Since many of those
 *        Clients are not part of the Enginuity build process, this will almost
 *        certainly require new versions of Guest OS CTD Client code to be
 *        released.
 *
 *        Before modifying any data structures in this file please discuss the
 *        change with the maintainers.
 *
 */


/* macros: */

/* the PCI vendor ID for all devices: */
#define EMC_CTD_PCI_VENDOR				(0x1120)

/* the PCI product ID for all version 1.x devices: */
#define EMC_CTD_V010_PCI_PRODUCT			(0x1b00)

/* the PCI revision ID for the first version 1.0 device: */
#define EMC_CTD_V010_PCI_REVISION			(1)

/* the 64-bit BAR pair for the transmit and receive rings: */
#define EMC_CTD_V010_BAR_RINGS				(0)

/* the 64-bit BAR pair for the fast registers: */
#define EMC_CTD_V010_BAR_FREGS				(2)

/* the 32-bit BAR for the slow registers: */
#define EMC_CTD_V010_BAR_SREGS				(4)

/* the maximum number of immediate SGL entries: */
#define EMC_CTD_V010_SGL_IMMEDIATE_MAX			(7)

/* the CTD v010 whats: */
#define EMC_CTD_V010_WHAT_DETECT			(0)
#define EMC_CTD_V010_WHAT_SCSI_COMMAND			(1)
#define EMC_CTD_V010_WHAT_SCSI_PHASE			(2)
#define EMC_CTD_V010_WHAT_SCSI_RESPONSE			(3)

/* the CTD v010 detect flags.  all undefined flags must be zero: */

/* if this is set, the name is a SCSI target: */
#define EMC_CTD_V010_DETECT_FLAG_SCSI_TARGET		(1 << 0)

/* if this is set, the name is a SCSI initiator: */
#define EMC_CTD_V010_DETECT_FLAG_SCSI_INITIATOR		(1 << 1)

/* the CTD v010 SCSI command flags.  all undefined flags must be zero: */

/* when the guest receives a SCSI command message, this flag is undefined.
 *
 * If this is set, at the beginning of any data phase the target is the data
 * source.  if this is clear, at the beginning of any data phase the target is
 * the data sink.
 */
#define EMC_CTD_V010_SCSI_COMMAND_FLAG_SOURCE		(1 << 0)

/* when the guest receives a SCSI command message, this flag is undefined.
 *
 * if this is set, the first SGL entry in the message points to an extended SGL,
 * and the remaining SGL entries in the message are undefined.  if this is
 * clear, the SGL entries in the message are used:
 */
#define EMC_CTD_V010_SCSI_COMMAND_FLAG_ESGL		(1 << 1)

/* the CTD v010 SCSI response flags.  all undefined flags must be zero: */

/* if this is set, the SCSI command failed.  if this is clear, the
 * command succeeded:
 */
#define EMC_CTD_V010_SCSI_RESPONSE_FLAG_FAILED		(1 << 0)

/* if this is set, any extra information is sense data.  if this is clear, any
 * extra information is 64-bit timestamps:
 */
#define EMC_CTD_V010_SCSI_RESPONSE_FLAG_SENSE		(1 << 1)

/* the CTD v010 SCSI phase flags.  all undefined flags must be zero: */

/* when the guest receives a SCSI phase message, this flag is undefined.
 *
 * if this is set, at this point in the data phase the message receiver is the
 * data source.  if this is clear, at this point in the data phase the message
 * receiver is the data sink:
 */
#define EMC_CTD_V010_SCSI_PHASE_FLAG_SOURCE		(1 << 0)

/* when the guest receives a SCSI phase message, this flag is undefined.
 *
 * if this is set, the first SGL entry in the message points to an extended SGL,
 * and the remaining SGL entries in the message are undefined.  if this is
 * clear, the SGL entries in the message are used:
 */
#define EMC_CTD_V010_SCSI_PHASE_FLAG_ESGL		(1 << 1)

/* if this is set, the message receiver is the target.  if this is clear, the
 * message receiver is the initiator:
 */
#define EMC_CTD_V010_SCSI_PHASE_FLAG_TARGET		(1 << 2)

/* if this is set, the SCSI command is aborted: */
#define EMC_CTD_V010_SCSI_PHASE_FLAG_ABORT		(1 << 3)

/* the size of the log of errored transmit messages: */
#define EMC_CTD_V010_LOG_ERROR_TX_SIZE			(4)

/* errors: */

/* no error: */
#define EMC_CTD_V010_ERROR_NULL				(0)

/* the guest tried to transmit a message on a disconnected channel: */
#define EMC_CTD_V010_ERROR_TX_CHANNEL_DISCONNECTED	(1)

/* the guest tried to transmit a message with a bad what: */
#define EMC_CTD_V010_ERROR_TX_MESSAGE_WHAT		(2)

/* the guest tried to transmit a message with a reserved field set to
 * the wrong value:
 */
#define EMC_CTD_V010_ERROR_TX_MESSAGE_RESERVED		(3)

/* the guest tried to transmit an out-of-order message: */
#define EMC_CTD_V010_ERROR_TX_MESSAGE_ORDER		(4)

/* the guest tried to transmit a message to an endpoint whose type
 * doesn't support it:
 */
#define EMC_CTD_V010_ERROR_TX_ENDPOINT_TYPE		(5)

/* the guest tried to transmit a message with an unknown message
 * receiver's opaque value:
 */
#define EMC_CTD_V010_ERROR_TX_OPAQUE_RX_UNKNOWN		(6)

/* types: */

/* a CTD v010 scatter/gather list entry: */
struct emc_ctd_v010_sgl {

	/* the physical address of the buffer: */
	emc_ctd_uint32_t emc_ctd_v010_sgl_paddr_0_31;
	emc_ctd_uint32_t emc_ctd_v010_sgl_paddr_32_63;

	/* the size of the buffer: */
	emc_ctd_uint32_t emc_ctd_v010_sgl_size;
};

/* a CTD v010 header: */
struct emc_ctd_v010_header {

	/* the other address: */
	emc_ctd_uint16_t emc_ctd_v010_header_address;

	/* the minor version: */
	emc_ctd_uint8_t emc_ctd_v010_header_minor;

	/* the what: */
	emc_ctd_uint8_t emc_ctd_v010_header_what;
};

/* a CTD v010 name: */
struct emc_ctd_v010_name {

	/* the name: */
	emc_ctd_uint8_t emc_ctd_v010_name_bytes[8];
};

/* a CTD v010 detect message: */
struct emc_ctd_v010_detect {

	/* the header: */
	struct emc_ctd_v010_header emc_ctd_v010_detect_header;

	/* the flags: */
	emc_ctd_uint32_t emc_ctd_v010_detect_flags;

	/* the name: */
	struct emc_ctd_v010_name emc_ctd_v010_detect_name;

	/* the key: */
	emc_ctd_uint64_t emc_ctd_v010_detect_key;
};

/* a CTD v010 SCSI command message: */
struct emc_ctd_v010_scsi_command {

	/* the header: */
	struct emc_ctd_v010_header emc_ctd_v010_scsi_command_header;

	/* the flags: */
	emc_ctd_uint32_t emc_ctd_v010_scsi_command_flags;

	/* the initiator's opaque value: */
	emc_ctd_uint64_t emc_ctd_v010_scsi_command_opaque;

	/* the SCSI LUN: */
	emc_ctd_uint8_t emc_ctd_v010_scsi_command_lun[8];

	/* the SCSI CDB: */
	emc_ctd_uint8_t emc_ctd_v010_scsi_command_cdb[16];

	/* the data size: */
	emc_ctd_uint32_t emc_ctd_v010_scsi_command_data_size;

	union {

		/* any SGL entries: */
		/* when received by the guest, these are undefined: */
		struct emc_ctd_v010_sgl
			emc_ctd_v010_scsi_command_sgl[EMC_CTD_V010_SGL_IMMEDIATE_MAX];

	} emc_ctd_v010_scsi_command_u;
};

/* a CTD v010 SCSI response message: */
struct emc_ctd_v010_scsi_response {

	/* the header: */
	struct emc_ctd_v010_header emc_ctd_v010_scsi_response_header;

	/* the flags: */
	emc_ctd_uint16_t emc_ctd_v010_scsi_response_flags;

	/* the extra information size: */
	emc_ctd_uint8_t emc_ctd_v010_scsi_response_extra_size;

	/* the SCSI status: */
	emc_ctd_uint8_t emc_ctd_v010_scsi_response_status;

	/* the initiator's opaque value: */
	emc_ctd_uint64_t emc_ctd_v010_scsi_response_opaque;

	/* the data size: */
	emc_ctd_uint32_t emc_ctd_v010_scsi_response_data_size;

	union {

		/* any extra information: */
		emc_ctd_uint8_t emc_ctd_v010_scsi_response_extra[108];

	} emc_ctd_v010_scsi_response_u;
};

/* a CTD v010 SCSI phase message: */
struct emc_ctd_v010_scsi_phase {

	/* the header: */
	struct emc_ctd_v010_header emc_ctd_v010_scsi_phase_header;

	/* the flags: */
	emc_ctd_uint32_t emc_ctd_v010_scsi_phase_flags;

	/* the message receiver's opaque value: */
	emc_ctd_uint64_t emc_ctd_v010_scsi_phase_opaque_rx;

	/* the message transmitter's opaque value: */
	emc_ctd_uint64_t emc_ctd_v010_scsi_phase_opaque_tx;

	union {

		/* any SGL entries: */
		/* when received by the guest, these are undefined: */
		struct emc_ctd_v010_sgl
			emc_ctd_v010_scsi_phase_sgl[EMC_CTD_V010_SGL_IMMEDIATE_MAX];

	} emc_ctd_v010_scsi_phase_u;
};

/* a CTD v010 message: */
union emc_ctd_v010_message {

	/* the header: */
	struct emc_ctd_v010_header emc_ctd_v010_message_header;

	/* a detect message: */
	struct emc_ctd_v010_detect emc_ctd_v010_message_detect;

	/* a SCSI command message: */
	struct emc_ctd_v010_scsi_command emc_ctd_v010_message_scsi_command;

	/* a SCSI response message: */
	struct emc_ctd_v010_scsi_response emc_ctd_v010_message_scsi_response;

	/* a SCSI phase message: */
	struct emc_ctd_v010_scsi_phase emc_ctd_v010_message_scsi_phase;

	/* padding: */
	emc_ctd_uint8_t emc_ctd_v010_message_padding[128];
};

/* the fast registers: */
struct emc_ctd_v010_fregs {

	/* the transmit ring producer index (TPI): */
	volatile emc_ctd_uint32_t emc_ctd_v010_fregs_tx_index_producer;

	/* the error flag: */
	volatile emc_ctd_uint32_t emc_ctd_v010_fregs_error_flag;

	/* errors 1..14: */
	volatile emc_ctd_uint32_t emc_ctd_v010_fregs_errors_1_14[14];

	/* the transmit ring consumer index (TCI): */
	volatile emc_ctd_uint32_t emc_ctd_v010_fregs_tx_index_consumer;

	/* the device name: */
	struct emc_ctd_v010_name emc_ctd_v010_fregs_device_name;

	/* padding: */
	emc_ctd_uint8_t
		emc_ctd_v010_fregs_pad_07f[64 -
			(sizeof(emc_ctd_uint32_t) +
				sizeof(struct emc_ctd_v010_name))];

	/* the receive ring producer index (RPI): */
	volatile emc_ctd_uint32_t emc_ctd_v010_fregs_rx_index_producer;

	/* the interrupt throttle, in units of nanoseconds.  zero disables
	 * the throttle:
	 */
	emc_ctd_uint32_t emc_ctd_v010_fregs_interrupt_throttle_nsecs;

	/* padding: */
	emc_ctd_uint8_t
		emc_ctd_v010_fregs_pad_0bf[64 -
			(sizeof(emc_ctd_uint32_t) + sizeof(emc_ctd_uint32_t))];

	/* the receive ring consumer index (RCI): */
	volatile emc_ctd_uint32_t emc_ctd_v010_fregs_rx_index_consumer;

	/* padding: */
	emc_ctd_uint8_t
		emc_ctd_v010_fregs_pad_0ff[64 -
			(sizeof(emc_ctd_uint32_t) +
				(sizeof(emc_ctd_uint32_t) *
					EMC_CTD_V010_LOG_ERROR_TX_SIZE))];

	/* the errors for the log of errored transmit messages: */
	volatile emc_ctd_uint32_t
		emc_ctd_v010_fregs_log_error_tx_error[EMC_CTD_V010_LOG_ERROR_TX_SIZE];

	/* the log of errored transmit messages: */
	union emc_ctd_v010_message
		emc_ctd_v010_fregs_log_error_tx_message[EMC_CTD_V010_LOG_ERROR_TX_SIZE];
};

/* the slow registers: */
struct emc_ctd_v010_sregs {

	/* the reset register: */
	emc_ctd_uint32_t emc_ctd_v010_sregs_reset;
};

#endif /* _EMC_CTD_INTERFACE_H */
