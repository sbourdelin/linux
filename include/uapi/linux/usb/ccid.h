/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2018 Marcus Folkesson <marcus.folkesson@gmail.com>
 *
 * This file holds USB constants defined by the CCID Specification.
 */

#ifndef CCID_H
#define CCID_H

/* Slot error register when bmCommandStatus = 1 */
#define CCID_CMD_ABORTED                            0xFF
#define CCID_ICC_MUTE                               0xFE
#define CCID_XFR_PARITY_ERROR                       0xFD
#define CCID_XFR_OVERRUN                            0xFC
#define CCID_HW_ERROR                               0xFB
#define CCID_BAD_ATR_TS                             0xF8
#define CCID_BAD_ATR_TCK                            0xF7
#define CCID_ICC_PROTOCOL_NOT_SUPPORTED             0xF6
#define CCID_ICC_CLASS_NOT_SUPPORTED                0xF5
#define CCID_PROCEDURE_BYTE_CONFLICT                0xF4
#define CCID_DEACTIVATED_PROTOCOL                   0xF3
#define CCID_BUSY_WITH_AUTO_SEQUENCE                0xF2
#define CCID_PIN_TIMEOUT                            0xF0
#define CCID_PIN_CANCELLED                          0xEF
#define CCID_CMD_SLOT_BUSY                          0xE0

/* PC to RDR messages (bulk out) */
#define CCID_PC_TO_RDR_ICCPOWERON                   0x62
#define CCID_PC_TO_RDR_ICCPOWEROFF                  0x63
#define CCID_PC_TO_RDR_GETSLOTSTATUS                0x65
#define CCID_PC_TO_RDR_XFRBLOCK                     0x6F
#define CCID_PC_TO_RDR_GETPARAMETERS                0x6C
#define CCID_PC_TO_RDR_RESETPARAMETERS              0x6D
#define CCID_PC_TO_RDR_SETPARAMETERS                0x61
#define CCID_PC_TO_RDR_ESCAPE                       0x6B
#define CCID_PC_TO_RDR_ICCCLOCK                     0x6E
#define CCID_PC_TO_RDR_T0APDU                       0x6A
#define CCID_PC_TO_RDR_SECURE                       0x69
#define CCID_PC_TO_RDR_MECHANICAL                   0x71
#define CCID_PC_TO_RDR_ABORT                        0x72
#define CCID_PC_TO_RDR_SETDATARATEANDCLOCKFREQUENCY 0x73

/* RDR to PC messages (bulk in) */
#define CCID_RDR_TO_PC_DATABLOCK                    0x80
#define CCID_RDR_TO_PC_SLOTSTATUS                   0x81
#define CCID_RDR_TO_PC_PARAMETERS                   0x82
#define CCID_RDR_TO_PC_ESCAPE                       0x83
#define CCID_RDR_TO_PC_DATARATEANDCLOCKFREQUENCY    0x84

/* Class Features */

/* No special characteristics */
#define CCID_FEATURES_NADA       0x00000000
/* Automatic parameter configuration based on ATR data */
#define CCID_FEATURES_AUTO_PCONF 0x00000002
/* Automatic activation of ICC on inserting */
#define CCID_FEATURES_AUTO_ACTIV 0x00000004
/* Automatic ICC voltage selection */
#define CCID_FEATURES_AUTO_VOLT  0x00000008
/* Automatic ICC clock frequency change */
#define CCID_FEATURES_AUTO_CLOCK 0x00000010
/* Automatic baud rate change */
#define CCID_FEATURES_AUTO_BAUD  0x00000020
/*Automatic parameters negotiation made by the CCID */
#define CCID_FEATURES_AUTO_PNEGO 0x00000040
/* Automatic PPS made by the CCID according to the active parameters */
#define CCID_FEATURES_AUTO_PPS   0x00000080
/* CCID can set ICC in clock stop mode */
#define CCID_FEATURES_ICCSTOP    0x00000100
/* NAD value other than 00 accepted (T=1 protocol in use) */
#define CCID_FEATURES_NAD        0x00000200
/* Automatic IFSD exchange as first exchange (T=1 protocol in use) */
#define CCID_FEATURES_AUTO_IFSD  0x00000400
/* TPDU level exchanges with CCID */
#define CCID_FEATURES_EXC_TPDU   0x00010000
/* Short APDU level exchange with CCID */
#define CCID_FEATURES_EXC_SAPDU  0x00020000
/* Short and Extended APDU level exchange with CCID */
#define CCID_FEATURES_EXC_APDU   0x00040000
/* USB Wake up signaling supported on card insertion and removal */
#define CCID_FEATURES_WAKEUP	0x00100000

/* Supported protocols */
#define CCID_PROTOCOL_NOT_SEL	0x00
#define CCID_PROTOCOL_T0	0x01
#define CCID_PROTOCOL_T1	0x02

#define CCID_PINSUPOORT_NONE		0x00
#define CCID_PINSUPOORT_VERIFICATION	(1 << 1)
#define CCID_PINSUPOORT_MODIFICATION	(1 << 2)

#endif
