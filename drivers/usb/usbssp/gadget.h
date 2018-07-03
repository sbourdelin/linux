// SPDX-License-Identifier: GPL-2.0
/*
 * USBSSP device controller driver
 *
 * Copyright (C) 2018 Cadence.
 *
 * Author: Pawel Laszczak
 * Some code borrowed from the Linux XHCI driver.
 */
#ifndef __LINUX_USBSSP_GADGET_H
#define __LINUX_USBSSP_GADGET_H

#include <linux/usb.h>
#include <linux/timer.h>
#include <linux/kernel.h>
#include <linux/io-64-nonatomic-lo-hi.h>
#include <linux/usb/gadget.h>
#include "gadget-ext-caps.h"

/* Max number slots - only 1 is allowed */
#define DEV_MAX_SLOTS 1

/* max ports for USBSSP-Dev - only 2 are allowed*/
#define MAX_USBSSP_PORTS	2

#define USBSSP_EP0_SETUP_SIZE 512

/*16 for in and 16 for out */
#define USBSSP_ENDPOINTS_NUM	32

/* HCSPARAMS1 - hcs_params1 - bitmasks */
/* bits 0:7, Max Device Slots */
#define DEV_HCS_MAX_SLOTS(p)	(((p) >> 0) & 0xff)
#define DEV_HCS_SLOTS_MASK	0xff

/*
 * USBSSP register interface.
 */
/**
 * struct usbssp_cap_regs - USBSSP Registers.
 * @hc_capbase:		length of the capabilities register and DC version number
 * @hcs_params1:	HCSPARAMS1 - Structural Parameters 1
 * @hcs_params2:	HCSPARAMS2 - Structural Parameters 2
 * @hcs_params3:	HCSPARAMS3 - Structural Parameters 3
 * @hcc_params:		HCCPARAMS - Capability Parameters
 * @db_off:		DBOFF - Doorbell array offset
 * @run_regs_off:	RTSOFF - Runtime register space offset
 * @hcc_params2:	HCCPARAMS2 Capability Parameters 2,
 */
struct usbssp_cap_regs {
	__le32	hc_capbase;
	__le32	hcs_params1;
	__le32	hcs_params2;
	__le32	hcs_params3;
	__le32	hcc_params;
	__le32	db_off;
	__le32	run_regs_off;
	__le32	hcc_params2;
	/* Reserved up to (CAPLENGTH - 0x1C) */
};

/* hc_capbase bitmasks */
/* bits 7:0 - how long is the Capabilities register */
#define HC_LENGTH(p)		USBSSP_HC_LENGTH(p)
/* bits 31:16	*/
#define HC_VERSION(p)		(((p) >> 16) & 0xffff)

/* HCSPARAMS1 - hcs_params1 - bitmasks */
/* bits 0:7, Max Device Slots */
#define HCS_MAX_SLOTS(p)	(((p) >> 0) & 0xff)
#define HCS_SLOTS_MASK		0xff
/* bits 8:18, Max Interrupters */
#define HCS_MAX_INTRS(p)	(((p) >> 8) & 0x7ff)
/* bits 24:31, Max Ports - max value is 0x7F = 127 ports */
#define HCS_MAX_PORTS(p)	(((p) >> 24) & 0x7f)

/* HCSPARAMS2 - hcs_params2 - bitmasks */
/* bits 0:3, Isochronous Scheduling Threshold (IST)*/
#define HCS_IST(p)		(((p) >> 0) & 0xf)
/* bits 4:7, max number of Event Ring segments */
#define HCS_ERST_MAX(p)		(((p) >> 4) & 0xf)
/* bits 21:25 Hi 5 bits of Scratchpad buffers SW must allocate for the HW */
/* bit 26 Scratchpad restore - for save/restore HW state - not used yet */
/* bits 27:31 Lo 5 bits of Scratchpad buffers SW must allocate for the HW */
#define HCS_MAX_SCRATCHPAD(p)   ((((p) >> 16) & 0x3e0) | (((p) >> 27) & 0x1f))

/* HCSPARAMS3 - hcs_params3 - bitmasks */
/* bits 0:7, Max U1 to U0 latency for the roothub ports */
#define HCS_U1_LATENCY(p)	(((p) >> 0) & 0xff)
/* bits 16:31, Max U2 to U0 latency for the roothub ports */
#define HCS_U2_LATENCY(p)	(((p) >> 16) & 0xffff)

/* HCCPARAMS - hcc_params - bitmasks */
/* true: DC can use 64-bit address pointers */
#define HCC_64BIT_ADDR(p)	((p) & (1 << 0))
/* true: DC uses 64-byte Device Context structures*/
#define HCC_64BYTE_CONTEXT(p)	((p) & (1 << 2))
/* true: DC has port indicators */
#define HCS_INDICATOR(p)	((p) & (1 << 4))
/* true: no secondary Stream ID Support */
#define HCC_NSS(p)		((p) & (1 << 7))
/*true: DC parse All Event Data*/
#define HCC_PAE(p)		((p) & (1<<8))
/* true: DC supports Stopped - Short Packet */
#define HCC_SPC(p)		((p) & (1 << 9))
/*true: DC support a stopped EDTLA Capability (SEC) */
#define HCC_SEC(p)		((p) & (1 << 10))
/* true: DC has Contiguous Frame ID Capability */
#define HCC_CFC(p)		((p) & (1 << 11))
/* Max size for Primary Stream Arrays - 2^(n+1), where n is bits 12:15 */
#define HCC_MAX_PSA(p)		(1 << ((((p) >> 12) & 0xf) + 1))
/* Extended Capabilities pointer from PCI base - section 5.3.6 */
#define HCC_EXT_CAPS(p)		USBSSP_HCC_EXT_CAPS(p)


#define CTX_SIZE(_hcc)		(HCC_64BYTE_CONTEXT(_hcc) ? 64 : 32)

/* db_off bitmask - bits 0:1 reserved */
#define	DBOFF_MASK	(~0x3)

/* run_regs_off bitmask - bits 0:4 reserved */
#define	RTSOFF_MASK	(~0x1f)

/* HCCPARAMS2 - hcc_params2 - bitmasks */
/* true: DC supports U3 entry Capability */
#define	HCC2_U3C(p)	((p) & (1 << 0))
/* true: DC supports Force Save context Capability */
#define	HCC2_FSC(p)	((p) & (1 << 2))
/* true: DC supports Compliance Transition Capability */
#define	HCC2_CTC(p)	((p) & (1 << 3))
/* true: DC support Large ESIT payload Capability > 48k */
#define	HCC2_LEC(p)	((p) & (1 << 4))
/* true: DC support Extended TBC Capability, Isoc burst count > 65535 */
#define	HCC2_ETC(p)	((p) & (1 << 6))

/* Number of registers per port */
#define	NUM_PORT_REGS	4

#define PORTSC		0
#define PORTPMSC	1
#define PORTLI		2
#define PORTHLPMC	3

/**
 * struct usbssp_op_regs - Device Controller Operational Registers.
 * @command:		USBCMD - DC command register
 * @status:		USBSTS - DC status register
 * @page_size:		This indicates the page size that the device controller
 *			supports.  If bit n is set, the DC supports a page size
 *			of 2^(n+12), up to a 128MB page size.
 *			4K is the minimum page size.
 * @cmd_ring:		CRP - 64-bit Command Ring Pointer
 * @dcbaa_ptr:		DCBAAP - 64-bit Device Context Base Address Array Pointer
 * @config_reg:		CONFIG - Configure Register
 * @port_status_base:	PORTSCn - base address for Port Status and Control
 *			Each port has a Port Status and Control register,
 *			followed by a Port Power Management Status and Control
 *			register, a Port Link Info register, and a reserved
 *			register.
 * @port_power_base:	PORTPMSCn - base address for
 *			Port Power Management Status and Control
 * @port_link_base:	PORTLIn - base address for Port Link Info (current
 *			Link PM state and control) for USB 2.1 and USB 3.0
 *			devices.
 */
struct usbssp_op_regs {
	__le32	command;
	__le32	status;
	__le32	page_size;
	__le32	reserved1;
	__le32	reserved2;
	__le32	dev_notification;
	__le64	cmd_ring;
	/* rsvd: offset 0x20-2F */
	__le32	reserved3[4];
	__le64	dcbaa_ptr;
	__le32	config_reg;
	/* rsvd: offset 0x3C-3FF */
	__le32	reserved4[241];
	/* port 1 registers, which serve as a base address for other ports */
	__le32	port_status_base;
	__le32	port_power_base;
	__le32	port_link_base;
	__le32	reserved5;
	/* registers for ports 2-255 */
	__le32	reserved6[NUM_PORT_REGS*254];
};

/* USBCMD - USB command - command bitmasks */
/* start/stop DC execution - do not write unless DC is halted*/
#define CMD_RUN		USBSSP_CMD_RUN

/* Reset DC - resets internal DC state machine and all registers (except
 * PCI config regs).
 */
#define CMD_RESET	(1 << 1)
/* Event Interrupt Enable - a '1' allows interrupts from the device controller*/
#define CMD_EIE		USBSSP_CMD_EIE
/* Device System Error Interrupt Enable - get out-of-band signal for DC errors*/
#define CMD_HSEIE	USBSSP_CMD_HSEIE
/* device controller save/restore state. */
#define CMD_CSS		(1 << 8)
#define CMD_CRS		(1 << 9)
/* Enable Wrap Event - '1' means DC generates an event when MFINDEX wraps. */
#define CMD_EWE		USBSSP_CMD_EWE
/* bit 14 Extended TBC Enable, changes Isoc TRB fields to support larger TBC */
#define CMD_ETE		(1 << 14)
/*bit 13 CEM Enable (CME) */
#define CMD_CEM		(1 << 13)
/* Device Enable bit */
#define CMD_DEVEN	(1 << 15)
/* bits 16:31 are reserved (and should be preserved on writes). */

/* IMAN - Interrupt Management Register */
#define IMAN_IE		(1 << 1)
#define IMAN_IP		(1 << 0)

/* USBSTS - USB status - status bitmasks */
/* DC not running - set to 1 when run/stop bit is cleared. */
#define STS_HALT	USBSSP_STS_HALT
/* serious error, e.g. PCI parity error.  The DC will clear the run/stop bit. */
#define STS_FATAL	(1 << 2)
/* event interrupt - clear this prior to clearing any IP flags in IR set*/
#define STS_EINT	(1 << 3)
/* port change detect */
#define STS_PORT	(1 << 4)
/* bits 5:7 reserved and zeroed */
/* save state status - '1' means DC is saving state */
#define STS_SAVE	(1 << 8)
/* restore state status - '1' means DC is restoring state */
#define STS_RESTORE	(1 << 9)
/* true: save or restore error */
#define STS_SRE		(1 << 10)
/* true: Controller Not Ready to accept doorbell or op reg writes after reset */
#define STS_CNR		USBSSP_STS_CNR
/* true: internal Device Controller Error - SW needs to reset and reinitialize*/
#define STS_HCE		(1 << 12)
/* bits 13:31 reserved and should be preserved */

/* CRCR - Command Ring Control Register - cmd_ring bitmasks */
/* bit 0 is the command ring cycle state */
/* stop ring operation after completion of the currently executing command */
#define CMD_RING_PAUSE		(1 << 1)
/* stop ring immediately - abort the currently executing command */
#define CMD_RING_ABORT		(1 << 2)
/* true: command ring is running */
#define CMD_RING_RUNNING	(1 << 3)
/* bits 4:5 reserved and should be preserved */
/* Command Ring pointer - bit mask for the lower 32 bits. */
#define CMD_RING_RSVD_BITS	(0x3f)

/* CONFIG - Configure Register - config_reg bitmasks */
/* bits 0:7 - maximum number of device slots enabled (NumSlotsEn) */
#define MAX_DEVS(p)		((p) & 0xff)
/* bit 8: U3 Entry Enabled, assert PLC when  controller  enters U3*/
#define CONFIG_U3E		(1 << 8)
/* bit 9: Configuration Information Enable */
#define CONFIG_CIE		(1 << 9)
/* bits 10:31 - reserved and should be preserved */

/* PORTSC - Port Status and Control Register - port_status_base bitmasks */
/* true: device connected */
#define PORT_CONNECT		(1 << 0)
/* true: port enabled */
#define PORT_PE			(1 << 1)
/* bit 2 reserved and zeroed */
/* true: port has an over-current condition */
#define PORT_OC			(1 << 3)
/* true: port reset signaling asserted */
#define PORT_RESET		(1 << 4)
/* Port Link State - bits 5:8
 * A read gives the current link PM state of the port,
 * a write with Link State Write Strobe set sets the link state.
 */

#define PORT_PLS_MASK	(0xf << 5)
#define XDEV_U0		(0x0 << 5)
#define XDEV_U1		(0x1 << 5)
#define XDEV_U2		(0x2 << 5)
#define XDEV_U3		(0x3 << 5)
#define XDEV_DISABLED	(0x4 << 5)
#define XDEV_RXDETECT	(0x5 << 5)
#define XDEV_INACTIVE	(0x6 << 5)
#define XDEV_POLLING	(0x7 << 5)
#define XDEV_RECOVERY	(0x8 << 5)
#define XDEV_HOT_RESET	(0x9 << 5)
#define XDEV_COMP_MODE	(0xa << 5)
#define XDEV_TEST_MODE	(0xb << 5)
#define XDEV_RESUME		(0xf << 5)

/* true: port has power (see HCC_PPC) */
#define PORT_POWER	(1 << 9)
/* bits 10:13 indicate device speed:
 * 0 - undefined speed - port hasn't be initialized by a reset yet
 * 1 - full speed
 * 2 - low speed
 * 3 - high speed
 * 4 - super speed
 * 5-15 reserved
 */
#define DEV_SPEED_MASK		(0xf << 10)
#define	XDEV_FS			(0x1 << 10)
#define	XDEV_LS			(0x2 << 10)
#define	XDEV_HS			(0x3 << 10)
#define	XDEV_SS			(0x4 << 10)
#define	XDEV_SSP		(0x5 << 10)
#define DEV_UNDEFSPEED(p)	(((p) & DEV_SPEED_MASK) == (0x0<<10))
#define DEV_FULLSPEED(p)	(((p) & DEV_SPEED_MASK) == XDEV_FS)
#define DEV_LOWSPEED(p)		(((p) & DEV_SPEED_MASK) == XDEV_LS)
#define DEV_HIGHSPEED(p)	(((p) & DEV_SPEED_MASK) == XDEV_HS)
#define DEV_SUPERSPEED(p)	(((p) & DEV_SPEED_MASK) == XDEV_SS)
#define DEV_SUPERSPEEDPLUS(p)	(((p) & DEV_SPEED_MASK) == XDEV_SSP)
#define DEV_SUPERSPEED_ANY(p)	(((p) & DEV_SPEED_MASK) >= XDEV_SS)
#define DEV_PORT_SPEED(p)	(((p) >> 10) & 0x0f)

/* Bits 20:23 in the Slot Context are twice */
#define	SLOT_SPEED_FS		(XDEV_FS << 10)
#define	SLOT_SPEED_LS		(XDEV_LS << 10)
#define	SLOT_SPEED_HS		(XDEV_HS << 10)
#define	SLOT_SPEED_SS		(XDEV_SS << 10)
#define	SLOT_SPEED_SSP		(XDEV_SSP << 10)

/* Port Link State Write Strobe - set this when changing link state */
#define PORT_LINK_STROBE	(1 << 16)
/* true: connect status change */
#define PORT_CSC	(1 << 17)
/* true: port enable change */
#define PORT_PEC	(1 << 18)
/* true: warm reset for a USB 3.0 device is done.  A "hot" reset puts the port
 * into an enabled state, and the device into the default state.  A "warm" reset
 * also resets the link, forcing the device through the link training sequence.
 * SW can also look at the Port Reset register to see when warm reset is done.
 */
#define PORT_WRC	(1 << 19)
/* true: over-current change */
#define PORT_OCC	(1 << 20)
/* true: reset change - 1 to 0 transition of PORT_RESET */
#define PORT_RC		(1 << 21)
/* port link status change - set on some port link state transitions:
 *  Transition			Reason
 *  ------------------------------------------------------------------------------
 *  - U3 to Resume		Wakeup signaling from a device
 *  - Resume to Recovery to U0	USB 3.0 device resume
 *  - Resume to U0		USB 2.0 device resume
 *  - U3 to Recovery to U0	Software resume of USB 3.0 device complete
 *  - U3 to U0			Software resume of USB 2.0 device complete
 *  - U2 to U0			L1 resume of USB 2.1 device complete
 *  - U0 to U0			L1 entry rejection by USB 2.1 device
 *  - U0 to disabled		L1 entry error with USB 2.1 device
 *  - Any state to inactive	Error on USB 3.0 port
 */
#define PORT_PLC		(1 << 22)
/* port configure error change - port failed to configure its link partner */
#define PORT_CEC		(1 << 23)
/* wake on connect (enable) */
#define PORT_WKCONN_E		(1 << 25)
/* wake on disconnect (enable) */
#define PORT_WKDISC_E		(1 << 26)
/* wake on over-current (enable) */
#define PORT_WKOC_E		(1 << 27)
/* bits 28:30 reserved */
/* Indicates if Warm Reset is being received*/
#define PORT_WR			(1 << 31)

/* We mark duplicate entries with -1 */
#define DUPLICATE_ENTRY ((u8)(-1))

/* Port Power Management Status and Control - port_power_base bitmasks */
/* Inactivity timer value for transitions into U1, in microseconds.
 * Timeout can be up to 127us.  0xFF means an infinite timeout.
 */
#define PORT_U1_TIMEOUT(p)	((p) & 0xff)
#define PORT_U1_TIMEOUT_MASK	0xff
/* Inactivity timer value for transitions into U2 */
#define PORT_U2_TIMEOUT(p)	(((p) & 0xff) << 8)
#define PORT_U2_TIMEOUT_MASK	(0xff << 8)
/* Bits 24:31 for port testing */

/* USB2 Protocol PORTSPMSC */
#define	PORT_L1S_MASK		7
#define	PORT_L1S_SUCCESS	1
#define	PORT_RWE		(1 << 3)
#define	PORT_HIRD(p)		(((p) & 0xf) << 4)
#define	PORT_HIRD_MASK		(0xf << 4)
#define	PORT_L1DS_MASK		(0xff << 8)
#define	PORT_L1DS(p)		(((p) & 0xff) << 8)
#define	PORT_HLE		(1 << 16)
#define PORT_TEST_MODE_SHIFT	28

/* USB3 Protocol PORTLI  Port Link Information */
#define PORT_RX_LANES(p)	(((p) >> 16) & 0xf)
#define PORT_TX_LANES(p)	(((p) >> 20) & 0xf)

/* USB2 Protocol PORTHLPMC */
#define PORT_HIRDM(p)		((p) & 3)
#define PORT_L1_TIMEOUT(p)	(((p) & 0xff) << 2)
#define PORT_BESLD(p)		(((p) & 0xf) << 10)

/* use 512 microseconds as USB2 LPM L1 default timeout. */
#define USBSSP_L1_TIMEOUT	512

#define USBSSP_DEFAULT_BESL	4

/**
 * struct usbssp_intr_reg - Interrupt Register Set
 * @irq_pending:	IMAN - Interrupt Management Register.  Used to enable
 *			interrupts and check for pending interrupts.
 * @irq_control:	IMOD - Interrupt Moderation Register.
 *			Used to throttle interrupts.
 * @erst_size:		Number of segments in the Event Ring Segment
 *			Table (ERST).
 * @erst_base:		ERST base address.
 * @erst_dequeue:	Event ring dequeue pointer.
 *
 * Each interrupter (defined by a MSI-X vector) has an event ring and an Event
 * Ring Segment Table (ERST) associated with it.  The event ring is comprised of
 * multiple segments of the same size.  The DC places events on the ring and
 * "updates the Cycle bit in the TRBs to indicate to software the current
 * position of the Enqueue Pointer." The driver processes those events and
 * updates the dequeue pointer.
 */
struct usbssp_intr_reg {
	__le32	irq_pending;
	__le32	irq_control;
	__le32	erst_size;
	__le32	rsvd;
	__le64	erst_base;
	__le64	erst_dequeue;
};

/* irq_pending bitmasks */
#define	ER_IRQ_PENDING(p)	((p) & 0x1)
/* bits 2:31 need to be preserved */
/* THIS IS BUGGY - FIXME - IP IS WRITE 1 TO CLEAR */
#define	ER_IRQ_CLEAR(p)		((p) & 0xfffffffe)
#define	ER_IRQ_ENABLE(p)	((ER_IRQ_CLEAR(p)) | 0x2)
#define	ER_IRQ_DISABLE(p)	((ER_IRQ_CLEAR(p)) & ~(0x2))

/* irq_control bitmasks */
/* Minimum interval between interrupts (in 250ns intervals).  The interval
 * between interrupts will be longer if there are no events on the event ring.
 * Default is 4000 (1 ms).
 */
#define ER_IRQ_INTERVAL_MASK	(0xffff)
/* Counter used to count down the time to the next interrupt - HW use only */
#define ER_IRQ_COUNTER_MASK	(0xffff << 16)

/* erst_size bitmasks */
/* Preserve bits 16:31 of erst_size */
#define	ERST_SIZE_MASK		(0xffff << 16)

/* erst_dequeue bitmasks */
/* Dequeue ERST Segment Index (DESI) - Segment number (or alias)
 * where the current dequeue pointer lies.  This is an optional HW hint.
 */
#define ERST_DESI_MASK		(0x7)
/* Event Handler Busy (EHB) - is the event ring scheduled to be serviced by
 * a work queue (or delayed service routine)?
 */
#define ERST_EHB		(1 << 3)
#define ERST_PTR_MASK		(0xf)

/**
 * struct usbssp_run_regs
 * @microframe_index:
 *		MFINDEX - current microframe number
 *
 *  Device Controller Runtime Registers:
 * "Software should read and write these registers using only Dword (32 bit)
 * or larger accesses"
 */
struct usbssp_run_regs {
	__le32			microframe_index;
	__le32			rsvd[7];
	struct usbssp_intr_reg	ir_set[128];
};

/**
 * struct doorbell_array
 *
 * Bits  0 -  7: Endpoint target
 * Bits  8 - 15: RsvdZ
 * Bits 16 - 31: Stream ID
 *
 */
struct usbssp_doorbell_array {
	__le32	doorbell[2];
};

#define DB_VALUE(ep, stream)		((((ep) + 1) & 0xff) | ((stream) << 16))
#define DB_VALUE_EP0_OUT(ep, stream)	((((ep) + 1) & 0xff) | ((stream) << 16))
#define DB_VALUE_CMD			0x00000000

/**
 * struct usbssp_protocol_caps
 * @revision:	major revision, minor revision, capability ID,
 *				and next capability pointer.
 * @name_string:Four ASCII characters to say which spec this DC
 *				follows, typically "USB ".
 * @port_info:	Port offset, count, and protocol-defined information.
 */
struct usbssp_protocol_caps {
	u32	revision;
	u32	name_string;
	u32	port_info;
};

#define	USBSSP_EXT_PORT_MAJOR(x)	(((x) >> 24) & 0xff)
#define	USBSSP_EXT_PORT_MINOR(x)	(((x) >> 16) & 0xff)
#define	USBSSP_EXT_PORT_PSIC(x)		(((x) >> 28) & 0x0f)
#define	USBSSP_EXT_PORT_OFF(x)		((x) & 0xff)
#define	USBSSP_EXT_PORT_COUNT(x)	(((x) >> 8) & 0xff)

#define	USBSSP_EXT_PORT_PSIV(x)	(((x) >> 0) & 0x0f)
#define	USBSSP_EXT_PORT_PSIE(x)	(((x) >> 4) & 0x03)
#define	USBSSP_EXT_PORT_PLT(x)	(((x) >> 6) & 0x03)
#define	USBSSP_EXT_PORT_PFD(x)	(((x) >> 8) & 0x01)
#define	USBSSP_EXT_PORT_LP(x)	(((x) >> 14) & 0x03)
#define	USBSSP_EXT_PORT_PSIM(x)	(((x) >> 16) & 0xffff)

#define PLT_MASK        (0x03 << 6)
#define PLT_SYM         (0x00 << 6)
#define PLT_ASYM_RX     (0x02 << 6)
#define PLT_ASYM_TX     (0x03 << 6)

/**
 * struct usbssp_container_ctx
 * @type: Type of context.  Used to calculated offsets to contained contexts.
 * @size: Size of the context data
 * @bytes: The raw context data given to HW
 * @dma: dma address of the bytes
 *
 * Represents either a Device or Input context.  Holds a pointer to the raw
 * memory used for the context (bytes) and dma address of it (dma).
 */
struct usbssp_container_ctx {
	unsigned int type;
#define USBSSP_CTX_TYPE_DEVICE  0x1
#define USBSSP_CTX_TYPE_INPUT   0x2
	int size;
	u8 *bytes;
	dma_addr_t dma;
};

/**
 * struct usbssp_slot_ctx
 * @dev_info:	device speed, and last valid endpoint
 * @dev_info2:	Max exit latency for device number
 * @int_target: interrupter target number
 * @dev_state:	slot state and device address
 *
 * Slot Context -  This assumes the DC uses 32-byte context
 * structures.  If the DC uses 64-byte contexts, there is an additional 32 bytes
 * reserved at the end of the slot context for DC internal use.
 */
struct usbssp_slot_ctx {
	__le32	dev_info;
	__le32	dev_info2;
	__le32	int_target;
	__le32	dev_state;
	/* offset 0x10 to 0x1f reserved for DC internal use */
	__le32	reserved[4];
};

/* dev_info bitmasks */
/* Device speed - values defined by PORTSC Device Speed field - 20:23 */
#define DEV_SPEED		(0xf << 20)
#define GET_DEV_SPEED(n)	(((n) & DEV_SPEED) >> 20)
/* bit 24-26 reserved */
/* Index of the last valid endpoint context in this device context - 27:31 */
#define LAST_CTX_MASK		(0x1f << 27)
#define LAST_CTX(p)		((p) << 27)
#define LAST_CTX_TO_EP_NUM(p)	(((p) >> 27) - 1)
#define SLOT_FLAG		(1 << 0)
#define EP0_FLAG		(1 << 1)

/* dev_info2 bitmasks */
/* Max Exit Latency (ms) - worst case time to wake up all links in dev path */
#define MAX_EXIT	(0xffff)
/* Root device port number that is needed to access the USB device */
#define ROOT_DEV_PORT(p)	(((p) & 0xff) << 16)
#define DEVINFO_TO_ROOT_DEV_PORT(p)	(((p) >> 16) & 0xff)

/* dev_state bitmasks */
/* USB device address - assigned by the usbssp */
#define DEV_ADDR_MASK		(0xff)
/* bits 8:26 reserved */
/* Slot state */
#define SLOT_STATE		(0x1f << 27)
#define GET_SLOT_STATE(p)	(((p) & (0x1f << 27)) >> 27)

#define SLOT_STATE_DISABLED	0
#define SLOT_STATE_ENABLED	SLOT_STATE_DISABLED
#define SLOT_STATE_DEFAULT	1
#define SLOT_STATE_ADDRESSED	2
#define SLOT_STATE_CONFIGURED	3

/**
 * struct usbssp_ep_ctx
 * @ep_info:	endpoint state, streams, mult, and interval information.
 * @ep_info2:	information on endpoint type, max packet size, max burst size,
 *		error count, and whether the DC will force an event for all
 *		transactions.
 * @deq: 64-bit ring dequeue pointer address.  If the endpoint only
 *		defines one stream, this points to the endpoint transfer ring.
 *		Otherwise, it points to a stream context array, which has a
 *		ring pointer for each flow.
 * @tx_info:
 *		Average TRB lengths for the endpoint ring and
 *		max payload within an Endpoint Service Interval Time (ESIT).
 *
 * Endpoint Context - This assumes the DC uses 32-byte context
 * structures.  If the DC uses 64-byte contexts, there is an additional 32 bytes
 * reserved at the end of the endpoint context for DC internal use.
 */
struct usbssp_ep_ctx {
	__le32	ep_info;
	__le32	ep_info2;
	__le64	deq;
	__le32	tx_info;
	/* offset 0x14 - 0x1f reserved for DC internal use */
	__le32	reserved[3];
};

/* ep_info bitmasks */
/*
 * Endpoint State - bits 0:2
 * 0 - disabled
 * 1 - running
 * 2 - halted due to halt condition - ok to manipulate endpoint ring
 * 3 - stopped
 * 4 - TRB error
 * 5-7 - reserved
 */
#define EP_STATE_MASK		(0xf)
#define EP_STATE_DISABLED	0
#define EP_STATE_RUNNING	1
#define EP_STATE_HALTED		2
#define EP_STATE_STOPPED	3
#define EP_STATE_ERROR		4
#define GET_EP_CTX_STATE(ctx)	(le32_to_cpu((ctx)->ep_info) & EP_STATE_MASK)

/* Mult - Max number of burtst within an interval, in EP companion desc. */
#define EP_MULT(p)			(((p) & 0x3) << 8)
#define CTX_TO_EP_MULT(p)		(((p) >> 8) & 0x3)
/* bits 10:14 are Max Primary Streams */
/* bit 15 is Linear Stream Array */
/* Interval - period between requests to an endpoint - 125u increments. */
#define EP_INTERVAL(p)			(((p) & 0xff) << 16)
#define EP_INTERVAL_TO_UFRAMES(p)	(1 << (((p) >> 16) & 0xff))
#define CTX_TO_EP_INTERVAL(p)		(((p) >> 16) & 0xff)
#define EP_MAXPSTREAMS_MASK		(0x1f << 10)
#define EP_MAXPSTREAMS(p)		(((p) << 10) & EP_MAXPSTREAMS_MASK)
#define CTX_TO_EP_MAXPSTREAMS(p)	(((p) & EP_MAXPSTREAMS_MASK) >> 10)
/* Endpoint is set up with a Linear Stream Array (vs. Secondary Stream Array) */
#define	EP_HAS_LSA			(1 << 15)
#define CTX_TO_MAX_ESIT_PAYLOAD_HI(p)	(((p) >> 24) & 0xff)

/* ep_info2 bitmasks */
/*
 * Force Event - generate transfer events for all TRBs for this endpoint
 * This will tell the DC to ignore the IOC and ISP flags (for debugging only).
 */
#define	FORCE_EVENT		(0x1)
#define ERROR_COUNT(p)		(((p) & 0x3) << 1)
#define CTX_TO_EP_TYPE(p)	(((p) >> 3) & 0x7)
#define EP_TYPE(p)		((p) << 3)
#define ISOC_OUT_EP		1
#define BULK_OUT_EP		2
#define INT_OUT_EP		3
#define CTRL_EP			4
#define ISOC_IN_EP		5
#define BULK_IN_EP		6
#define INT_IN_EP		7
/* bit 6 reserved */
/* bit 7 is Device Initiate Disable - for disabling stream selection */
#define MAX_BURST(p)		(((p)&0xff) << 8)
#define CTX_TO_MAX_BURST(p)	(((p) >> 8) & 0xff)
#define MAX_PACKET(p)		(((p)&0xffff) << 16)
#define MAX_PACKET_MASK		(0xffff << 16)
#define MAX_PACKET_DECODED(p)	(((p) >> 16) & 0xffff)

/* Get max packet size from ep desc. Bit 10..0 specify the max packet size.
 * USB2.0 spec 9.6.6.
 */
#define GET_MAX_PACKET(p)		((p) & 0x7ff)

/* tx_info bitmasks */
#define EP_AVG_TRB_LENGTH(p)		((p) & 0xffff)
#define EP_MAX_ESIT_PAYLOAD_LO(p)	(((p) & 0xffff) << 16)
#define EP_MAX_ESIT_PAYLOAD_HI(p)	((((p) >> 16) & 0xff) << 24)
#define CTX_TO_MAX_ESIT_PAYLOAD(p)	(((p) >> 16) & 0xffff)

/* deq bitmasks */
#define EP_CTX_CYCLE_MASK		(1 << 0)
#define SCTX_DEQ_MASK			(~0xfL)

/**
 * struct usbssp_input_control_context
 * Input control context;
 *
 * @drop_context:	set the bit of the endpoint context you want to disable
 * @add_context:	set the bit of the endpoint context you want to enable
 */
struct usbssp_input_control_ctx {
	__le32	drop_flags;
	__le32	add_flags;
	__le32	rsvd2[6];
};

#define	EP_IS_ADDED(ctrl_ctx, i) \
	(le32_to_cpu(ctrl_ctx->add_flags) & (1 << (i + 1)))
#define	EP_IS_DROPPED(ctrl_ctx, i)       \
	(le32_to_cpu(ctrl_ctx->drop_flags) & (1 << (i + 1)))

/* Represents everything that is needed to issue a command on the command ring.
 * It's useful to pre-allocate these for commands that cannot fail due to
 * out-of-memory errors, like freeing streams.
 */
struct usbssp_command {
	/* Input context for changing device state */
	struct usbssp_container_ctx	*in_ctx;
	u32	status;
	/* If completion is null, no one is waiting on this command
	 * and the structure can be freed after the command completes.
	 */
	struct completion		*completion;
	union usbssp_trb		*command_trb;
	struct list_head		cmd_list;
};

/* drop context bitmasks */
#define	DROP_EP(x)	(0x1 << x)
/* add context bitmasks */
#define	ADD_EP(x)	(0x1 << x)

struct usbssp_stream_ctx {
	/* 64-bit stream ring address, cycle state, and stream type */
	__le64	stream_ring;
	/* offset 0x14 - 0x1f reserved for DC internal use */
	__le32	reserved[2];
};

/* Stream Context Types - bits 3:1 of stream ctx deq ptr */
#define	SCT_FOR_CTX(p)		(((p) & 0x7) << 1)
/* Secondary stream array type, dequeue pointer is to a transfer ring */
#define	SCT_SEC_TR		0
/* Primary stream array type, dequeue pointer is to a transfer ring */
#define	SCT_PRI_TR		1
/* Dequeue pointer is for a secondary stream array (SSA) with 8 entries */
#define SCT_SSA_8		2
#define SCT_SSA_16		3
#define SCT_SSA_32		4
#define SCT_SSA_64		5
#define SCT_SSA_128		6
#define SCT_SSA_256		7

/* Assume no secondary streams for now */
struct usbssp_stream_info {
	struct usbssp_ring		**stream_rings;
	/* Number of streams, including stream 0 (which drivers can't use) */
	unsigned int			num_streams;
	/* The stream context array may be bigger than
	 * the number of streams the driver asked for
	 */
	struct usbssp_stream_ctx	*stream_ctx_array;
	unsigned int			num_stream_ctxs;
	dma_addr_t			ctx_array_dma;
	/* For mapping physical TRB addresses to segments in stream rings */
	struct radix_tree_root		trb_address_map;
	struct usbssp_command		*free_streams_command;
};

#define	SMALL_STREAM_ARRAY_SIZE		256
#define	MEDIUM_STREAM_ARRAY_SIZE	1024

struct usbssp_ep {
	struct usb_ep endpoint;
	struct list_head pending_list;
	struct usbssp_udc *usbssp_data;

	u8 number;
	u8 type;
	u32 interval;
	char name[20];
	u8 direction;
	u8 stream_capable;

	struct usbssp_ring		*ring;
	/* Related to endpoints that are configured to use stream IDs only */
	struct usbssp_stream_info	*stream_info;
	/* Temporary storage in case the configure endpoint command fails and we
	 * have to restore the device state to the previous state
	 */
	struct usbssp_ring		*new_ring;
	unsigned int			ep_state;

#define SET_DEQ_PENDING		(1 << 0)
#define EP_HALTED		(1 << 1)  /* For stall handling */
#define EP_STOP_CMD_PENDING	(1 << 2)  /* For USB request cancellation */
/* Transitioning the endpoint to using streams, don't enqueue request */
#define EP_GETTING_STREAMS	(1 << 3)
#define EP_HAS_STREAMS		(1 << 4)
/* Transitioning the endpoint to not using streams, don't enqueue requests */
#define EP_GETTING_NO_STREAMS	(1 << 5)
#define USBSSP_EP_ENABLED	(1<<6)
#define USBSSP_EP_WEDGE		(1<<8)
#define USBSSP_EP_BUSY		(1<<9)
#define USBSSP_EP_CONF_PENDING	(1<<10)
#define USBSSP_EP_DISABLE_PENDING (1<<11)
#define EP0_HALTED_STATUS	(1 << 12) /*For stall handling of Status Stage*/

	struct usbssp_td	*stopped_td;
	unsigned int		stopped_stream;

	/* Dequeue pointer and dequeue segment for a submitted Set TR Dequeue
	 * command.  We'll need to update the ring's dequeue segment and dequeue
	 * pointer after the command completes.
	 */
	struct usbssp_segment	*queued_deq_seg;
	union usbssp_trb	*queued_deq_ptr;
	/*
	 * Sometimes the DC can not process isochronous endpoint ring quickly
	 * enough, and it will miss some isoc tds on the ring and generate
	 * a Missed Service Error Event.
	 * Set skip flag when receive a Missed Service Error Event and
	 * process the missed tds on the endpoint ring.
	 */
	bool			skip;
	/* Isoch Frame ID checking storage */
	int			next_frame_id;
	/* Use new Isoch TRB layout needed for extended TBC support */
	bool			use_extended_tbc;
};


struct usbssp_device {
	struct usb_gadget *gadget;

	/*
	 * Commands to the hardware are passed an "input context" that
	 * tells the hardware what to change in its data structures.
	 * The hardware will return changes in an "output context" that
	 * software must allocate for the hardware.  We need to keep
	 * track of input and output contexts separately because
	 * these commands might fail and we don't trust the hardware.
	 */
	struct usbssp_container_ctx	*out_ctx;
	/* Used for addressing devices and configuration changes */
	struct usbssp_container_ctx	*in_ctx;

	struct usbssp_ep		eps[USBSSP_ENDPOINTS_NUM];
	u8				port_num;

	/* The current max exit latency for the enabled USB3 link states. */
	u16				current_mel;

	u8				usb2_hw_lpm_capable:1;
	/* Used for the debugfs interfaces. */
	void				*debugfs_private;
};

/**
 * struct usbssp_device_context_array
 * @dev_context_ptr	array of 64-bit DMA addresses for device contexts
 */
struct usbssp_device_context_array {
	/* 64-bit device addresses; we only write 32-bit addresses */
	__le64		dev_context_ptrs[DEV_MAX_SLOTS+1];
	/* private pointers */
	dma_addr_t	dma;
};

struct usbssp_transfer_event {
	/* 64-bit buffer address, or immediate data */
	__le64	buffer;
	__le32	transfer_len;
	/* This field is interpreted differently based on the type of TRB */
	__le32	flags;
};

/* Transfer event TRB length bit mask */
/* bits 0:23 */
#define	EVENT_TRB_LEN(p)		((p) & 0xffffff)

/** Transfer Event bit fields **/
#define	TRB_TO_EP_ID(p)			(((p) >> 16) & 0x1f)

/* Completion Code - only applicable for some types of TRBs */
#define	COMP_CODE_MASK			(0xff << 24)
#define GET_COMP_CODE(p)		(((p) & COMP_CODE_MASK) >> 24)
#define COMP_INVALID			0
#define COMP_SUCCESS			1
#define COMP_DATA_BUFFER_ERROR		2
#define COMP_BABBLE_DETECTED_ERROR	3
#define COMP_USB_TRANSACTION_ERROR	4
#define COMP_TRB_ERROR			5
#define COMP_RESOURCE_ERROR		7
#define COMP_NO_SLOTS_AVAILABLE_ERROR	9
#define COMP_INVALID_STREAM_TYPE_ERROR	10
#define COMP_SLOT_NOT_ENABLED_ERROR	11
#define COMP_ENDPOINT_NOT_ENABLED_ERROR	12
#define COMP_SHORT_PACKET		13
#define COMP_RING_UNDERRUN		14
#define COMP_RING_OVERRUN		15
#define COMP_VF_EVENT_RING_FULL_ERROR	16
#define COMP_PARAMETER_ERROR		17
#define COMP_CONTEXT_STATE_ERROR	19
#define COMP_EVENT_RING_FULL_ERROR	21
#define COMP_INCOMPATIBLE_DEVICE_ERROR	22
#define COMP_MISSED_SERVICE_ERROR	23
#define COMP_COMMAND_RING_STOPPED	24
#define COMP_COMMAND_ABORTED		25
#define COMP_STOPPED			26
#define COMP_STOPPED_LENGTH_INVALID	27
#define COMP_STOPPED_SHORT_PACKET	28
#define COMP_MAX_EXIT_LATENCY_TOO_LARGE_ERROR	29
#define COMP_ISOCH_BUFFER_OVERRUN	31
#define COMP_EVENT_LOST_ERROR		32
#define COMP_UNDEFINED_ERROR		33
#define COMP_INVALID_STREAM_ID_ERROR	34

static inline const char *usbssp_trb_comp_code_string(u8 status)
{
	switch (status) {
	case COMP_INVALID:
		return "Invalid";
	case COMP_SUCCESS:
		return "Success";
	case COMP_DATA_BUFFER_ERROR:
		return "Data Buffer Error";
	case COMP_BABBLE_DETECTED_ERROR:
		return "Babble Detected";
	case COMP_USB_TRANSACTION_ERROR:
		return "USB Transaction Error";
	case COMP_TRB_ERROR:
		return "TRB Error";
	case COMP_RESOURCE_ERROR:
		return "Resource Error";
	case COMP_NO_SLOTS_AVAILABLE_ERROR:
		return "No Slots Available Error";
	case COMP_INVALID_STREAM_TYPE_ERROR:
		return "Invalid Stream Type Error";
	case COMP_SLOT_NOT_ENABLED_ERROR:
		return "Slot Not Enabled Error";
	case COMP_ENDPOINT_NOT_ENABLED_ERROR:
		return "Endpoint Not Enabled Error";
	case COMP_SHORT_PACKET:
		return "Short Packet";
	case COMP_RING_UNDERRUN:
		return "Ring Underrun";
	case COMP_RING_OVERRUN:
		return "Ring Overrun";
	case COMP_VF_EVENT_RING_FULL_ERROR:
		return "VF Event Ring Full Error";
	case COMP_PARAMETER_ERROR:
		return "Parameter Error";
	case COMP_CONTEXT_STATE_ERROR:
		return "Context State Error";
	case COMP_EVENT_RING_FULL_ERROR:
		return "Event Ring Full Error";
	case COMP_INCOMPATIBLE_DEVICE_ERROR:
		return "Incompatible Device Error";
	case COMP_MISSED_SERVICE_ERROR:
		return "Missed Service Error";
	case COMP_COMMAND_RING_STOPPED:
		return "Command Ring Stopped";
	case COMP_COMMAND_ABORTED:
		return "Command Aborted";
	case COMP_STOPPED:
		return "Stopped";
	case COMP_STOPPED_LENGTH_INVALID:
		return "Stopped - Length Invalid";
	case COMP_STOPPED_SHORT_PACKET:
		return "Stopped - Short Packet";
	case COMP_MAX_EXIT_LATENCY_TOO_LARGE_ERROR:
		return "Max Exit Latency Too Large Error";
	case COMP_ISOCH_BUFFER_OVERRUN:
		return "Isoch Buffer Overrun";
	case COMP_EVENT_LOST_ERROR:
		return "Event Lost Error";
	case COMP_UNDEFINED_ERROR:
		return "Undefined Error";
	case COMP_INVALID_STREAM_ID_ERROR:
		return "Invalid Stream ID Error";
	default:
		return "Unknown!!";
	}
}
struct usbssp_link_trb {
	/* 64-bit segment pointer*/
	__le64 segment_ptr;
	__le32 intr_target;
	__le32 control;
};

/* control bitfields */
#define LINK_TOGGLE	(0x1<<1)

/* Command completion event TRB */
struct usbssp_event_cmd {
	/* Pointer to command TRB, or the value passed by the event data trb */
	__le64 cmd_trb;
	__le32 status;
	__le32 flags;
};

/* flags bitmasks */

/* Address device - disable SetAddress */
#define TRB_BSR		(1<<9)

/* Configure Endpoint - Deconfigure */
#define TRB_DC		(1<<9)

/* Stop Ring - Transfer State Preserve */
#define TRB_TSP		(1<<9)

enum usbssp_ep_reset_type {
	EP_HARD_RESET,
	EP_SOFT_RESET,
};

/* Force Event */
#define TRB_TO_VF_INTR_TARGET(p)	(((p) & (0x3ff << 22)) >> 22)
#define TRB_TO_VF_ID(p)			(((p) & (0xff << 16)) >> 16)

/* Set Latency Tolerance Value */
#define TRB_TO_BELT(p)			(((p) & (0xfff << 16)) >> 16)

/* Get Port Bandwidth */
#define TRB_TO_DEV_SPEED(p)		(((p) & (0xf << 16)) >> 16)

/* Force Header */
#define TRB_TO_PACKET_TYPE(p)		((p) & 0x1f)
#define TRB_TO_DEV_PORT(p)		(((p) & (0xff << 24)) >> 24)

enum usbssp_setup_dev {
	SETUP_CONTEXT_ONLY,
	SETUP_CONTEXT_ADDRESS,
};

/* bits 16:23 are the virtual function ID */
/* bits 24:31 are the slot ID */
#define TRB_TO_SLOT_ID(p)		(((p) & (0xff<<24)) >> 24)
#define SLOT_ID_FOR_TRB(p)		(((p) & 0xff) << 24)

/* Stop Endpoint TRB - ep_index to endpoint ID for this TRB */
#define TRB_TO_EP_INDEX(p)		((((p) & (0x1f << 16)) >> 16) - 1)
#define	EP_ID_FOR_TRB(p)		((((p) + 1) & 0x1f) << 16)

#define SUSPEND_PORT_FOR_TRB(p)		(((p) & 1) << 23)
#define TRB_TO_SUSPEND_PORT(p)		(((p) & (1 << 23)) >> 23)
#define LAST_EP_INDEX			30

/* Set TR Dequeue Pointer command TRB fields. */
#define TRB_TO_STREAM_ID(p)		((((p) & (0xffff << 16)) >> 16))
#define STREAM_ID_FOR_TRB(p)		((((p)) & 0xffff) << 16)
#define SCT_FOR_TRB(p)			(((p) << 1) & 0x7)

/* Link TRB specific fields */
#define TRB_TC				(1<<1)

/* Port Status Change Event TRB fields */
/* Port ID - bits 31:24 */
#define GET_PORT_ID(p)			(((p) & (0xff << 24)) >> 24)

#define EVENT_DATA			(1 << 2)

/* Normal TRB fields */
/* transfer_len bitmasks - bits 0:16 */
#define	TRB_LEN(p)			((p) & 0x1ffff)
/* TD Size, packets remaining in this TD, bits 21:17 (5 bits, so max 31) */
#define TRB_TD_SIZE(p)		(min((p), (u32)31) << 17)
#define GET_TD_SIZE(p)			(((p) & 0x3e0000) >> 17)
/* DC uses the TD_SIZE field for TBC if Extended TBC is enabled (ETE) */
#define TRB_TD_SIZE_TBC(p)		(min((p), (u32)31) << 17)
/* Interrupter Target - which MSI-X vector to target the completion event at */
#define TRB_INTR_TARGET(p)		(((p) & 0x3ff) << 22)
#define GET_INTR_TARGET(p)		(((p) >> 22) & 0x3ff)
/* Total burst count field, Rsvdz on DC with Extended TBC enabled (ETE) */
#define TRB_TBC(p)			(((p) & 0x3) << 7)
#define TRB_TLBPC(p)			(((p) & 0xf) << 16)

/* Cycle bit - indicates TRB ownership by DC or driver*/
#define TRB_CYCLE		(1<<0)
/*
 * Force next event data TRB to be evaluated before task switch.
 * Used to pass OS data back after a TD completes.
 */
#define TRB_ENT			(1<<1)
/* Interrupt on short packet */
#define TRB_ISP			(1<<2)
/* Set PCIe no snoop attribute */
#define TRB_NO_SNOOP		(1<<3)
/* Chain multiple TRBs into a TD */
#define TRB_CHAIN		(1<<4)
/* Interrupt on completion */
#define TRB_IOC			(1<<5)
/* The buffer pointer contains immediate data */
#define TRB_IDT			(1<<6)

/* Block Event Interrupt */
#define	TRB_BEI			(1<<9)

/* Control transfer TRB specific fields */
#define TRB_DIR_IN		(1<<16)
#define	TRB_TX_TYPE(p)		((p) << 16)
#define	TRB_DATA_OUT		2
#define	TRB_DATA_IN		3

/* TRB bit mask in Data Stage TRB */
#define	TRB_SETUPID_BITMASK	(0x300)
#define TRB_SETUPID(p)		((p) << 8)
#define TRB_SETUPID_TO_TYPE(p)	(((p) & TRB_SETUPID_BITMASK) >> 8)

#define TRB_SETUP_SPEEDID_USB3	0x1
#define TRB_SETUP_SPEEDID_USB2	0x0
#define TRB_SETUP_SPEEDID(p)	((p) & (1 << 7))

#define TRB_SETUPSTAT_ACK	0x1
#define TRB_SETUPSTAT_STALL	0x0
#define TRB_SETUPSTAT(p)	((p) << 6)

/* Isochronous TRB specific fields */
#define TRB_SIA			(1<<31)
#define TRB_FRAME_ID(p)		(((p) & 0x7ff) << 20)

struct usbssp_generic_trb {
	__le32 field[4];
};

union usbssp_trb {
	struct usbssp_link_trb		link;
	struct usbssp_transfer_event	trans_event;
	struct usbssp_event_cmd		event_cmd;
	struct usbssp_generic_trb	generic;
};

/* TRB bit mask */
#define	TRB_TYPE_BITMASK	(0xfc00)
#define TRB_TYPE(p)		((p) << 10)
#define TRB_FIELD_TO_TYPE(p)	(((p) & TRB_TYPE_BITMASK) >> 10)
/* TRB type IDs */
/* bulk, interrupt, isoc scatter/gather, and control data stage */
#define TRB_NORMAL		1
/* setup stage for control transfers */
#define TRB_SETUP		2

/* data stage for control transfers */
#define TRB_DATA		3
/* status stage for control transfers */
#define TRB_STATUS		4
/* isoc transfers */
#define TRB_ISOC		5
/* TRB for linking ring segments */
#define TRB_LINK		6
#define TRB_EVENT_DATA	7
/* Transfer Ring No-op (not for the command ring) */
#define TRB_TR_NOOP		8
/* Command TRBs */
/* Enable Slot Command */
#define TRB_ENABLE_SLOT		9
/* Disable Slot Command */
#define TRB_DISABLE_SLOT	10
/* Address Device Command */
#define TRB_ADDR_DEV		11
/* Configure Endpoint Command */
#define TRB_CONFIG_EP		12
/* Evaluate Context Command */
#define TRB_EVAL_CONTEXT	13
/* Reset Endpoint Command */
#define TRB_RESET_EP		14
/* Stop Transfer Ring Command */
#define TRB_STOP_RING		15
/* Set Transfer Ring Dequeue Pointer Command */
#define TRB_SET_DEQ		16
/* Reset Device Command */
#define TRB_RESET_DEV		17
/* Force Event Command (opt) */
#define TRB_FORCE_EVENT		18
/* Set Latency Tolerance Value Command (opt) */
#define TRB_SET_LT		20
/* Force Header Command - generate a transaction or link management packet */
#define TRB_FORCE_HEADER	22
/* No-op Command - not for transfer rings */
#define TRB_CMD_NOOP		23
/* TRB IDs 24-31 reserved */
/* Event TRBS */
/* Transfer Event */
#define TRB_TRANSFER		32
/* Command Completion Event */
#define TRB_COMPLETION		33
/* Port Status Change Event */
#define TRB_PORT_STATUS		34
/* Doorbell Event (opt) */
#define TRB_DOORBELL		36
/* Device Controller Event */
#define TRB_HC_EVENT		37
/* Device Notification Event - device sent function wake notification */
#define TRB_DEV_NOTE		38
/* MFINDEX Wrap Event - microframe counter wrapped */
#define TRB_MFINDEX_WRAP	39
/* TRB IDs 40-47 reserved, 48-63 is vendor-defined */
/* Halt Endpoint Command */
#define TRB_HALT_ENDPOINT	54
/* Flush Endpoint Command */
#define TRB_FLUSH_ENDPOINT	58

static inline const char *usbssp_trb_type_string(u8 type)
{
	switch (type) {
	case TRB_NORMAL:
		return "Normal";
	case TRB_SETUP:
		return "Setup Stage";
	case TRB_DATA:
		return "Data Stage";
	case TRB_STATUS:
		return "Status Stage";
	case TRB_ISOC:
		return "Isoch";
	case TRB_LINK:
		return "Link";
	case TRB_EVENT_DATA:
		return "Event Data";
	case TRB_TR_NOOP:
		return "No-Op";
	case TRB_ENABLE_SLOT:
		return "Enable Slot Command";
	case TRB_DISABLE_SLOT:
		return "Disable Slot Command";
	case TRB_ADDR_DEV:
		return "Address Device Command";
	case TRB_CONFIG_EP:
		return "Configure Endpoint Command";
	case TRB_EVAL_CONTEXT:
		return "Evaluate Context Command";
	case TRB_RESET_EP:
		return "Reset Endpoint Command";
	case TRB_STOP_RING:
		return "Stop Ring Command";
	case TRB_SET_DEQ:
		return "Set TR Dequeue Pointer Command";
	case TRB_RESET_DEV:
		return "Reset Device Command";
	case TRB_FORCE_EVENT:
		return "Force Event Command";
	case TRB_SET_LT:
		return "Set Latency Tolerance Value Command";
	case TRB_FORCE_HEADER:
		return "Force Header Command";
	case TRB_CMD_NOOP:
		return "No-Op Command";
	case TRB_TRANSFER:
		return "Transfer Event";
	case TRB_COMPLETION:
		return "Command Completion Event";
	case TRB_PORT_STATUS:
		return "Port Status Change Event";
	case TRB_DOORBELL:
		return "Doorbell Event";
	case TRB_HC_EVENT:
		return "Device Controller Event";
	case TRB_DEV_NOTE:
		return "Device Notification Event";
	case TRB_MFINDEX_WRAP:
		return "MFINDEX Wrap Event";
	default:
		return "UNKNOWN";
	}
}

#define TRB_TYPE_LINK(x)	(((x) & TRB_TYPE_BITMASK) == TRB_TYPE(TRB_LINK))
/* Above, but for __le32 types -- can avoid work by swapping constants: */
#define TRB_TYPE_LINK_LE32(x)	(((x) & cpu_to_le32(TRB_TYPE_BITMASK)) == \
					cpu_to_le32(TRB_TYPE(TRB_LINK)))
#define TRB_TYPE_NOOP_LE32(x)	(((x) & cpu_to_le32(TRB_TYPE_BITMASK)) == \
					cpu_to_le32(TRB_TYPE(TRB_TR_NOOP)))

/*
 * TRBS_PER_SEGMENT must be a multiple of 4,
 * since the command ring is 64-byte aligned.
 * It must also be greater than 16.
 */
#define TRBS_PER_SEGMENT	16 /*was 256 */
/* Allow two commands + a link TRB, along with any reserved command TRBs */
#define MAX_RSVD_CMD_TRBS	(TRBS_PER_SEGMENT - 3)
#define TRB_SEGMENT_SIZE	(TRBS_PER_SEGMENT*16)
#define TRB_SEGMENT_SHIFT	(ilog2(TRB_SEGMENT_SIZE))
/* TRB buffer pointers can't cross 64KB boundaries */
#define TRB_MAX_BUFF_SHIFT	16
#define TRB_MAX_BUFF_SIZE	(1 << TRB_MAX_BUFF_SHIFT)
/* How much data is left before the 64KB boundary? */
#define TRB_BUFF_LEN_UP_TO_BOUNDARY(addr) (TRB_MAX_BUFF_SIZE - \
					  (addr & (TRB_MAX_BUFF_SIZE - 1)))

struct usbssp_segment {
	union usbssp_trb	*trbs;
	/* private to DC */
	struct usbssp_segment	*next;
	dma_addr_t		dma;
	/* Max packet sized bounce buffer for td-fragmant alignment */
	dma_addr_t		bounce_dma;
	void			*bounce_buf;
	unsigned int		bounce_offs;
	unsigned int		bounce_len;
};

struct usbssp_td {
	struct list_head	td_list;
	struct usbssp_request	*priv_request;
	struct usbssp_segment	*start_seg;
	union usbssp_trb	*first_trb;
	union usbssp_trb	*last_trb;
	struct usbssp_segment	*bounce_seg;
	/* actual_length of the request has already been set */
	bool request_length_set;
};


/* DC command default timeout value */
#define USBSSP_CMD_DEFAULT_TIMEOUT	(5 * HZ)

struct usbssp_dequeue_state {
	struct usbssp_segment	*new_deq_seg;
	union usbssp_trb	*new_deq_ptr;
	int			new_cycle_state;
	unsigned int		stream_id;
};

enum usbssp_ring_type {
	TYPE_CTRL = 0,
	TYPE_ISOC,
	TYPE_BULK,
	TYPE_INTR,
	TYPE_STREAM,
	TYPE_COMMAND,
	TYPE_EVENT,
};

static inline const char *usbssp_ring_type_string(enum usbssp_ring_type type)
{
	switch (type) {
	case TYPE_CTRL:
		return "CTRL";
	case TYPE_ISOC:
		return "ISOC";
	case TYPE_BULK:
		return "BULK";
	case TYPE_INTR:
		return "INTR";
	case TYPE_STREAM:
		return "STREAM";
	case TYPE_COMMAND:
		return "CMD";
	case TYPE_EVENT:
		return "EVENT";
	}

	return "UNKNOWN";
}
struct usbssp_ring {
	struct usbssp_segment	*first_seg;
	struct usbssp_segment	*last_seg;
	union  usbssp_trb	*enqueue;
	struct usbssp_segment	*enq_seg;
	union  usbssp_trb	*dequeue;
	struct usbssp_segment	*deq_seg;
	struct list_head	td_list;
	/*
	 * Write the cycle state into the TRB cycle field to give ownership of
	 * the TRB to the device controller (if we are the producer),
	 * or to check if we own the TRB (if we are the consumer).
	 */
	u32			cycle_state;
	unsigned int		stream_id;
	unsigned int		num_segs;
	unsigned int		num_trbs_free;
	unsigned int		num_trbs_free_temp;
	unsigned int		bounce_buf_len;
	enum usbssp_ring_type	type;
	bool			last_td_was_short;
	struct radix_tree_root	*trb_address_map;
};

struct usbssp_erst_entry {
	/* 64-bit event ring segment address */
	__le64	seg_addr;
	__le32	seg_size;
	/* Set to zero */
	__le32	rsvd;
};

struct usbssp_erst {
	struct usbssp_erst_entry	*entries;
	unsigned int			num_entries;
	/* usbssp_udc->event_ring keeps track of segment dma addresses */
	dma_addr_t			erst_dma_addr;
	/* Num entries the ERST can contain */
	unsigned int			erst_size;
};

struct usbssp_scratchpad {
	u64		*sp_array;
	dma_addr_t	sp_dma;
	void		**sp_buffers;
};

struct usbssp_request {
	/*number of TDs associated with this request*/
	int			num_tds;
	/*number of actually handled TDs*/
	int			num_tds_done;
	struct	usbssp_td	*td;

	struct usb_request	request;
	struct list_head	list;
	struct usbssp_ep	*dep;

	struct scatterlist	*sg;
	unsigned int		num_pending_sgs;
	u8			epnum;
	unsigned		direction:1;
	unsigned		mapped:1;
	uint32_t		start_frame;
	int			stream_id;
};


/*
 * Each segment table entry is 4*32bits long.  1K seems like an ok size:
 * (1K bytes * 8bytes/bit) / (4*32 bits) = 64 segment entries in the table,
 * meaning 64 ring segments.
 * Initial allocated size of the ERST, in number of entries
 */
#define	ERST_NUM_SEGS	1
/* Initial allocated size of the ERST, in number of entries */
#define	ERST_SIZE	64
/* Initial number of event segment rings allocated */
#define	ERST_ENTRIES	1
/* Poll every 60 seconds */
#define	POLL_TIMEOUT	60


struct s3_save {
	u32	command;
	u32	dev_nt;
	u64	dcbaa_ptr;
	u32	config_reg;
	u32	irq_pending;
	u32	irq_control;
	u32	erst_size;
	u64	erst_base;
	u64	erst_dequeue;
};

enum usbssp_ep0_state {
	USBSSP_EP0_UNCONNECTED = 0,
	USBSSP_EP0_SETUP_PHASE,
	USBSSP_EP0_DATA_PHASE,
	USBSSP_EP0_STATUS_PHASE,
};

struct usbssp_ports {
	u8	maj_rev;
	u8	min_rev;
	u32	*psi;		/* array of protocol speed ID entries */
	u8	psi_count;
	u8	psi_uid_count;
};

struct usbssp_udc {
	struct device		 *dev;
	struct usb_gadget	 gadget;
	struct usb_gadget_driver *gadget_driver;

	unsigned int		irq;		/* irq allocated */
	void __iomem		*regs;		/* device memory/io */
	resource_size_t		rsrc_start;	/* memory/io resource start */
	resource_size_t		rsrc_len;	/* memory/io resource length */
	u8 msi_enabled;
	/* USBSSP Registers */
	struct usbssp_cap_regs __iomem		*cap_regs;
	struct usbssp_op_regs __iomem		*op_regs;
	struct usbssp_run_regs __iomem		*run_regs;
	struct usbssp_doorbell_array __iomem	*dba;
	/* current interrupter register set */
	struct	usbssp_intr_reg __iomem	*ir_set;

	/* Cached register copies of read-only USBSSP data */
	__u32		hcs_params1;
	__u32		hcs_params2;
	__u32		hcs_params3;
	__u32		hcc_params;
	__u32		hcc_params2;

	unsigned int num_endpoints;

	u8			setupId;
	u8			setup_speed;
	enum usbssp_ep0_state	ep0state;
	/*three state or two state setup */
	u8			ep0_expect_in;
	struct usbssp_request	usb_req_ep0_in;
	u8			three_stage_setup;
	u32			delayed_status;
	/*temporary buffer for setup packet*/
	struct usb_ctrlrequest setup;
	void			*setup_buf;
	u8			device_address;
	u8			bos_event_detected :1;

	uint8_t			defered_event;
#define EVENT_DEV_CONNECTED 1
#define EVENT_DEV_DISCONECTED 2
#define EVENT_SETUP_PACKET 4
#define EVENT_USB_RESET 8
	int			remote_wakeup_allowed;

	spinlock_t		lock;
	spinlock_t		irq_thread_lock;
	unsigned long		irq_thread_flag;

	/* packed release number */
	u16			hci_version;
	u8			max_slots;
	u8			max_interrupters;
	u8			max_ports;
	u8			isoc_threshold;
	/* imod_interval in ns (I * 250ns) */
	u32			imod_interval;

	/*revision of current used port*/
	u8			 port_major_revision;
	/* 4KB min, 128MB max */
	int			page_size;
	/* Valid values are 12 to 20, inclusive */
	int			page_shift;
	/* msi-x vectors */
	int			msix_count;
	struct msix_entry	*msix_entries;

	/* data structures */
	struct usbssp_device_context_array *dcbaa;
	struct usbssp_ring	*cmd_ring;
	unsigned int		cmd_ring_state;

#define CMD_RING_STATE_RUNNING	(1 << 0)
#define CMD_RING_STATE_ABORTED	(1 << 1)
#define CMD_RING_STATE_STOPPED	(1 << 2)

	struct list_head	cmd_list;
	unsigned int		cmd_ring_reserved_trbs;
	struct delayed_work	cmd_timer;
	struct work_struct	bottom_irq;
	struct workqueue_struct *bottom_irq_wq;
	struct completion	cmd_ring_stop_completion;
	struct usbssp_command	*current_cmd;
	struct usbssp_ring	*event_ring;
	struct usbssp_erst	erst;
	/* Scratchpad */
	struct usbssp_scratchpad *scratchpad;

	/* slot enabling and address device helpers */
	/* these are not thread safe so use mutex */
	struct			mutex mutex;
	int			slot_id;

	/* Internal mirror of the HW's dcbaa */
	struct usbssp_device	devs;

	/* DMA pools */
	struct dma_pool		*device_pool;
	struct dma_pool		*segment_pool;
	struct dma_pool		*small_streams_pool;
	struct dma_pool		*medium_streams_pool;

	unsigned int		usbssp_state;

	u32			command;
	struct s3_save		s3;

	/* Device controller is dying - not responding to commands.
	 *
	 * DC interrupts have been disabled and a watchdog timer
	 * will (or has already) halt the device controller, and complete all
	 * requests with an -ESHUTDOWN code.  Any code that sees this status
	 * (other than the timer that set it) should stop touching
	 * hardware immediately.  Interrupt handlers should return
	 * immediately when they see this status.
	 */
	#define USBSSP_STATE_DYING	(1 << 0)
	#define USBSSP_STATE_HALTED	(1 << 1)
	#define USBSSP_STATE_REMOVING	(1 << 2)

	#define USBSSP_STATE_DISCONNECT_PENDING	(1 << 4)
	#define USBSSP_STATE_DISCONNECTED	(1 << 8)

	unsigned int		num_active_eps;

	/* Is each DC port a USB 3.0, USB 2.0, or USB 1.1 port? */
	u8			*port_array;
	/* Pointers to USB 3.0 PORTSC registers */
	__le32 __iomem		*usb3_ports;
	unsigned int		num_usb3_ports;
	/* Pointers to USB 2.0 PORTSC registers */
	__le32 __iomem		*usb2_ports;
	unsigned int		num_usb2_ports;
	struct usbssp_ports	usb2_rhub;
	struct usbssp_ports	usb3_rhub;
	/* support software LPM */
	unsigned		sw_lpm_support:1;
	/* support USB2 hardware LPM */
	unsigned		hw_lpm_support:1;
	/* cached usb2 extended protocol capabilities */
	u32			*ext_caps;
	unsigned int		num_ext_caps;

	u32			port_suspended;
	u32			port_remote_wakeup;
	u16			test_mode;

	struct dentry		*debugfs_root;
	struct dentry		*debugfs_slots;
	struct list_head	regset_list;
};

#define GET_PORT_RRBESL(p)	(((p) >> 17) & 0xf)
#define PORT_RBESL(p)		(((p) & 0xf) << 4)
#define	PORT_BESL_MASK		(0xf << 4)
#define	PORT_HLE_MASK		(1 << 6)

#define PORT_L1S_HLE0_STALL 1

#define	USBSSP_CFC_DELAY	10

#define usbssp_dbg(usbssp_data, fmt, args...) \
	dev_dbg(usbssp_data->dev, fmt, ## args)

#define usbssp_err(usbssp_data, fmt, args...) \
	dev_err(usbssp_data->dev, fmt, ## args)

#define usbssp_warn(usbssp_data, fmt, args...) \
	dev_warn(usbssp_data->dev, fmt, ## args)

#define usbssp_warn_ratelimited(usbssp_data, fmt, args...) \
	dev_warn_ratelimited(usbssp_data->dev, fmt, ## args)

#define usbssp_info(usbssp_data, fmt, args...) \
	dev_info(usbssp_data->dev, fmt, ## args)

/*
 * Registers should always be accessed with double word or quad word accesses.
 *
 * Registers with 64-bit address pointers should be written to with
 * dword accesses by writing the low dword first (ptr[0]), then the high dword
 * (ptr[1]) second. DC implementations that do not support 64-bit address
 * pointers will ignore the high dword, and write order is irrelevant.
 */
static inline u64 usbssp_read_64(const struct usbssp_udc *usbssp_data,
		__le64 __iomem *regs)
{
	return lo_hi_readq(regs);
}

static inline void usbssp_write_64(struct usbssp_udc *usbssp_data,
				 const u64 val, __le64 __iomem *regs)
{
	lo_hi_writeq(val, regs);
}

/* USBSSP memory management */
char *usbssp_get_slot_state(struct usbssp_udc *usbssp_data,
		struct usbssp_container_ctx *ctx);
void usbssp_dbg_trace(struct usbssp_udc *usbssp_data,
		void (*trace)(struct va_format *),
		const char *fmt, ...);

/* USBSSP memory management */
void usbssp_mem_cleanup(struct usbssp_udc *usbssp_data);
int usbssp_mem_init(struct usbssp_udc *usbssp_data, gfp_t flags);
void usbssp_free_priv_device(struct usbssp_udc *usbssp_data);
int usbssp_alloc_priv_device(struct usbssp_udc *usbssp_data, gfp_t flags);

int usbssp_setup_addressable_priv_dev(struct usbssp_udc *usbssp_data);
void usbssp_copy_ep0_dequeue_into_input_ctx(struct usbssp_udc *usbssp_data);
unsigned int usbssp_get_endpoint_index(const struct usb_endpoint_descriptor *desc);
unsigned int usbssp_get_endpoint_address(unsigned int ep_index);
unsigned int usbssp_get_endpoint_flag(const struct usb_endpoint_descriptor *desc);

unsigned int usbssp_last_valid_endpoint(u32 added_ctxs);
void usbssp_endpoint_zero(struct usbssp_udc *usbssp_data,
		struct usbssp_device *dev_priv, struct usbssp_ep *ep);
void usbssp_endpoint_copy(struct usbssp_udc *usbssp_data,
		struct usbssp_container_ctx *in_ctx,
		struct usbssp_container_ctx *out_ctx,
		unsigned int ep_index);
void usbssp_slot_copy(struct usbssp_udc *usbssp_data,
		struct usbssp_container_ctx *in_ctx,
		struct usbssp_container_ctx *out_ctx);
int usbssp_endpoint_init(struct usbssp_udc *usbssp_data,
		struct usbssp_device *dev_priv,
		struct usbssp_ep *dep,
		gfp_t mem_flags);

void usbssp_ring_free(struct usbssp_udc *usbssp_data, struct usbssp_ring *ring);
int usbssp_ring_expansion(struct usbssp_udc *usbssp_data,
		struct usbssp_ring *ring, unsigned int num_trbs, gfp_t flags);
void usbssp_free_endpoint_ring(struct usbssp_udc *usbssp_data,
		struct usbssp_device *dev_priv,
		unsigned int ep_index);
void usbssp_free_stream_info(struct usbssp_udc *usbssp_data,
		struct usbssp_stream_info *stream_info);
struct usbssp_ring *usbssp_dma_to_transfer_ring(
		struct usbssp_ep *ep,
		u64 address);
struct usbssp_ring *usbssp_stream_id_to_ring(
		struct usbssp_device *dev,
		unsigned int ep_index,
		unsigned int stream_id);

struct usbssp_command *usbssp_alloc_command(struct usbssp_udc *usbssp_data,
		bool allocate_completion, gfp_t mem_flags);
struct usbssp_command *usbssp_alloc_command_with_ctx(
		struct usbssp_udc *usbssp_data,
		bool allocate_completion, gfp_t mem_flags);
void usbssp_request_free_priv(struct usbssp_request *req_priv);
void usbssp_free_command(struct usbssp_udc *usbssp_data,
		struct usbssp_command *command);

struct usbssp_container_ctx *usbssp_alloc_container_ctx(
		struct usbssp_udc *usbssp_data,
		int type, gfp_t flags);
void usbssp_free_container_ctx(struct usbssp_udc *usbssp_data,
		struct usbssp_container_ctx *ctx);

/* USBSSP Device controller glue */
void usbssp_bottom_irq(struct work_struct *work);
int usbssp_init(struct usbssp_udc *usbssp_data);
void usbssp_stop(struct usbssp_udc *usbssp_data);
int usbssp_handshake(void __iomem *ptr, u32 mask, u32 done, int usec);
void usbssp_quiesce(struct usbssp_udc *usbssp_data);
int usbssp_halt(struct usbssp_udc *usbssp_data);
int usbssp_start(struct usbssp_udc *usbssp_data);
extern int usbssp_reset(struct usbssp_udc *usbssp_data);
int usbssp_run(struct usbssp_udc *usbssp_data);
int usbssp_gen_setup(struct usbssp_udc *usbssp_data);
int usbssp_disable_slot(struct usbssp_udc *usbssp_data);

int usbssp_suspend(struct usbssp_udc *usbssp_data, bool do_wakeup);
int usbssp_resume(struct usbssp_udc *usbssp_data, bool hibernated);

int usbssp_get_frame(struct usbssp_udc *usbssp_data);
irqreturn_t usbssp_irq(int irq, void *priv);

irqreturn_t usbssp_msi_irq(int irq, void *usbssp_data);

int usbssp_alloc_dev(struct usbssp_udc *usbssp_data);
void usbssp_free_dev(struct usbssp_udc *usbssp_data);

int usbssp_address_device(struct usbssp_udc *usbssp_data);
int usbssp_enable_device(struct usbssp_udc *usbssp_data);

int usbssp_set_usb2_hardware_lpm(struct usbssp_udc *usbsssp_data,
		struct usb_request *req, int enable);

/* USBSSP ring, segment, TRB, and TD functions */
dma_addr_t usbssp_trb_virt_to_dma(struct usbssp_segment *seg,
		union usbssp_trb *trb);
struct usbssp_segment *usbssp_trb_in_td(struct usbssp_udc *usbssp_data,
		struct usbssp_segment *start_seg,
		union usbssp_trb *start_trb, union usbssp_trb *end_trb,
		dma_addr_t suspect_dma, bool debug);

int usbssp_is_vendor_info_code(struct usbssp_udc *usbssp_data,
		unsigned int trb_comp_code);
void usbssp_ring_cmd_db(struct usbssp_udc *usbssp_data);
int usbssp_queue_slot_control(struct usbssp_udc *usbssp_data,
		struct usbssp_command *cmd, u32 trb_type);
int usbssp_queue_address_device(struct usbssp_udc *usbssp_data,
		struct usbssp_command *cmd,
		dma_addr_t in_ctx_ptr, enum usbssp_setup_dev setup);

void usbssp_queue_force_header_erdy(struct usbssp_udc *usbssp_data,
		unsigned int ep_index);

int usbssp_queue_vendor_command(struct usbssp_udc *usbssp_data,
		struct usbssp_command *cmd,
		u32 field1, u32 field2, u32 field3, u32 field4);

int usbssp_queue_stop_endpoint(struct usbssp_udc *usbssp_data,
		struct usbssp_command *cmd,
		unsigned int ep_index, int suspend);
int usbssp_queue_ctrl_tx(struct usbssp_udc *usbssp_data, gfp_t mem_flags,
		 struct usbssp_request *req_priv, unsigned int ep_index);

int usbssp_queue_bulk_tx(struct usbssp_udc *usbssp_data, gfp_t mem_flags,
		struct usbssp_request *req_priv, unsigned int ep_index);
int usbssp_queue_intr_tx(struct usbssp_udc *usbssp_data, gfp_t mem_flags,
		struct usbssp_request *req_priv, unsigned int ep_index);
int usbssp_queue_isoc_tx_prepare(
		struct usbssp_udc *usbssp_data, gfp_t mem_flags,
		struct usbssp_request *req_priv, unsigned int ep_index);
int usbssp_queue_configure_endpoint(struct usbssp_udc *usbssp_data,
		struct usbssp_command *cmd, dma_addr_t in_ctx_ptr,
		bool command_must_succeed);
int usbssp_queue_evaluate_context(struct usbssp_udc *usbssp_data,
		struct usbssp_command *cmd,
		dma_addr_t in_ctx_ptr, bool command_must_succeed);
int usbssp_queue_reset_ep(struct usbssp_udc *usbssp_data,
		struct usbssp_command *cmd,
		unsigned int ep_index, enum usbssp_ep_reset_type reset_type);
int usbssp_queue_nop(struct usbssp_udc *usbssp_data,
		struct usbssp_command *cmd);
void usbssp_cleanup_halted_endpoint(struct usbssp_udc *usbssp_data,
		unsigned int ep_index,
		unsigned int stream_id,
		struct usbssp_td *td,
		enum usbssp_ep_reset_type reset_type);
int usbssp_queue_halt_endpoint(struct usbssp_udc *usbssp_data,
		struct usbssp_command *cmd,
		unsigned int ep_index);
int usbssp_queue_reset_device(struct usbssp_udc *usbssp_data,
		struct usbssp_command *cmd);
void usbssp_find_new_dequeue_state(struct usbssp_udc *usbssp_data,
		unsigned int ep_index,
		unsigned int stream_id, struct usbssp_td *cur_td,
		struct usbssp_dequeue_state *state);
void usbssp_queue_new_dequeue_state(struct  usbssp_udc *usbssp_data,
		unsigned int ep_index,
		struct usbssp_dequeue_state *deq_state);
void usbssp_cleanup_stalled_ring(struct usbssp_udc *usbssp_data,
			unsigned int ep_index, unsigned int stream_id,
			struct usbssp_td *td);
void usbssp_stop_endpoint_command_watchdog(struct timer_list *t);
void usbssp_handle_command_timeout(struct work_struct *work);

void usbssp_ring_ep_doorbell(struct usbssp_udc *usbssp_data,
		unsigned int ep_index, unsigned int stream_id);
void usbssp_cleanup_command_queue(struct usbssp_udc *usbssp_data);
void inc_deq(struct usbssp_udc *usbssp_data, struct usbssp_ring *ring);
unsigned int count_trbs(u64 addr, u64 len);

/* USBSSP port code */
void usbssp_set_link_state(struct usbssp_udc *usbssp_data,
		__le32 __iomem *port_regs, u32 link_state);

void usbssp_test_and_clear_bit(struct usbssp_udc *usbssp_data,
		__le32 __iomem *port_regs, u32 port_bit);

void usbssp_udc_died(struct usbssp_udc *usbssp_data);

#ifdef CONFIG_PM
int usbssp_bus_suspend(struct usbssp_udc *usbssp_data);
int usbssp_bus_resume(struct usbssp_udc *usbssp_data);
#else
#define	usbssp_bus_suspend	NULL
#define	usbssp_bus_resume		NULL
#endif	/* CONFIG_PM */
u32 usbssp_port_state_to_neutral(u32 state);

/* USBSSP DC contexts */
struct usbssp_input_control_ctx *usbssp_get_input_control_ctx(
		struct usbssp_container_ctx *ctx);
struct usbssp_slot_ctx *usbssp_get_slot_ctx(struct usbssp_udc *usbssp_data,
		struct usbssp_container_ctx *ctx);
struct usbssp_ep_ctx *usbssp_get_ep_ctx(struct usbssp_udc *usbssp_data,
		struct usbssp_container_ctx *ctx, unsigned int ep_index);
struct usbssp_ring *usbssp_triad_to_transfer_ring(struct usbssp_udc
		*usbssp_data, unsigned int ep_index, unsigned int stream_id);

/* USBSSP debugging */
void usbssp_print_trb_offsets(struct usbssp_udc *usbssp_data,
		union usbssp_trb *trb);
void usbssp_print_ir_set(struct usbssp_udc *usbssp_data, int set_num);
void usbssp_print_registers(struct usbssp_udc *usbssp_data);
void usbssp_dbg_regs(struct usbssp_udc *usbssp_data);
void usbssp_print_run_regs(struct usbssp_udc *usbssp_data);
void usbssp_debug_trb(struct usbssp_udc *usbssp_data, union usbssp_trb *trb);
void usbssp_debug_segment(struct usbssp_udc *usbssp_data,
		struct usbssp_segment *seg);
void usbssp_debug_ring(struct usbssp_udc *usbssp_data,
		struct usbssp_ring *ring);
void usbssp_dbg_erst(struct usbssp_udc *usbssp_data, struct usbssp_erst *erst);
void usbssp_dbg_cmd_ptrs(struct usbssp_udc *usbssp_data);
void usbssp_dbg_ring_ptrs(struct usbssp_udc *usbssp_data,
		struct usbssp_ring *ring);
void usbssp_dbg_ctx(struct usbssp_udc *usbssp_data,
		struct usbssp_container_ctx *ctx, unsigned int last_ep);

void usbssp_dbg_ep_rings(struct usbssp_udc *usbssp_data,
		unsigned int ep_index, struct usbssp_ep *ep);

void usbssp_dbg_trace(struct usbssp_udc *usbssp_data,
		void (*trace)(struct va_format *),
		const char *fmt, ...);

/* USBSSP gadget interface*/
void usbssp_suspend_gadget(struct usbssp_udc *usbssp_data);
void usbssp_resume_gadget(struct usbssp_udc *usbssp_data);
int usbssp_gadget_init(struct usbssp_udc *usbssp_data);
int  usbssp_gadget_exit(struct usbssp_udc *usbssp_data);
void usbssp_gadget_free_endpoint(struct usbssp_udc *usbssp_data);
int usbssp_gadget_init_endpoint(struct usbssp_udc *usbssp_data);
void usbssp_gadget_giveback(struct usbssp_ep *ep_priv,
		struct usbssp_request *req_priv, int status);
int usbssp_enqueue(struct usbssp_ep *dep, struct usbssp_request *req_priv);
int usbssp_dequeue(struct usbssp_ep *dep, struct usbssp_request *req_priv);
unsigned int usbssp_port_speed(unsigned int port_status);
void usbssp_gadget_reset_interrupt(struct usbssp_udc *usbssp_data);
void usbssp_gadget_disconnect_interrupt(struct usbssp_udc *usbssp_data);
int usbssp_stop_device(struct usbssp_udc *usbssp_data, int suspend);
int usbssp_halt_endpoint(struct usbssp_udc *usbssp_data,
		struct usbssp_ep *dep, int value);
int usbssp_cmd_stop_ep(struct usbssp_udc *usbssp_data, struct usb_gadget *g,
		struct usbssp_ep *ep_priv);
int usbssp_enter_test_mode(struct usbssp_udc *usbssp_data,
		u16 test_mode, unsigned long *flags);
int usbssp_exit_test_mode(struct usbssp_udc *usbssp_data);
int usbssp_setup_analyze(struct usbssp_udc *usbssp_data);
int usbssp_data_complete(struct usbssp_udc *usbssp_data,
		struct usbssp_transfer_event *event);
int usbssp_status_complete(struct usbssp_udc *usbssp_data,
		struct usbssp_transfer_event *event);

int usbssp_status_stage(struct usbssp_udc *usbssp_data);

void usbssp_kill_endpoint_request(struct usbssp_udc *usbssp_data,
		  int ep_index);
int usbssp_add_endpoint(struct usbssp_udc *usbssp_data, struct usbssp_ep *dep);
int usbssp_drop_endpoint(struct usbssp_udc *usbssp_data, struct usb_gadget *g,
		struct usbssp_ep *dep);
int usbssp_reset_device(struct usbssp_udc *usbssp_data);
int usbssp_check_bandwidth(struct usbssp_udc *usbssp_data,
		struct usb_gadget *g);
void usbssp_reset_bandwidth(struct usbssp_udc *usbssp_data,
		struct usb_gadget *g);

static inline struct usbssp_ring *usbssp_request_to_transfer_ring(
		struct usbssp_udc *usbssp_data, struct usbssp_request *req_priv)
{
	return usbssp_triad_to_transfer_ring(usbssp_data,
			usbssp_get_endpoint_index(req_priv->dep->endpoint.desc),
			req_priv->request.stream_id);
}


static inline char *usbssp_slot_state_string(u32 state)
{
	switch (state) {
	case SLOT_STATE_ENABLED:
		return "enabled/disabled";
	case SLOT_STATE_DEFAULT:
		return "default";
	case SLOT_STATE_ADDRESSED:
		return "addressed";
	case SLOT_STATE_CONFIGURED:
		return "configured";
	default:
		return "reserved";
	}
}

static inline const char *usbssp_decode_trb(u32 field0, u32 field1, u32 field2,
		u32 field3)
{
	static char str[256];
	int type = TRB_FIELD_TO_TYPE(field3);

	switch (type) {
	case TRB_LINK:
		sprintf(str,
			"LINK %08x%08x intr %d type '%s' flags %c:%c:%c:%c",
			field1, field0, GET_INTR_TARGET(field2),
			usbssp_trb_type_string(type),
			field3 & TRB_IOC ? 'I' : 'i',
			field3 & TRB_CHAIN ? 'C' : 'c',
			field3 & TRB_TC ? 'T' : 't',
			field3 & TRB_CYCLE ? 'C' : 'c');
		break;
	case TRB_TRANSFER:
	case TRB_COMPLETION:
	case TRB_PORT_STATUS:
	case TRB_DOORBELL:
	case TRB_HC_EVENT:
	case TRB_DEV_NOTE:
	case TRB_MFINDEX_WRAP:
		sprintf(str,
			"TRB %08x%08x status '%s' len %d slot %d ep %d:=:"
			"type '%s' flags %c:%c",
			field1, field0,
			usbssp_trb_comp_code_string(GET_COMP_CODE(field2)),
			EVENT_TRB_LEN(field2), TRB_TO_SLOT_ID(field3),
			/* Macro decrements 1, maybe it shouldn't?!? */
			TRB_TO_EP_INDEX(field3) + 1,
			usbssp_trb_type_string(type),
			field3 & EVENT_DATA ? 'E' : 'e',
			field3 & TRB_CYCLE ? 'C' : 'c');

		break;
	case TRB_SETUP:
		sprintf(str, "bRequestType %02x bRequest %02x wValue %02x%02x "
				"wIndex %02x%02x wLength %d length %d "
				"TD size %d intr %d type '%s' flags %c:%c:%c",
				field0 & 0xff,
				(field0 & 0xff00) >> 8,
				(field0 & 0xff000000) >> 24,
				(field0 & 0xff0000) >> 16,
				(field1 & 0xff00) >> 8,
				field1 & 0xff,
				(field1 & 0xff000000) >> 16 |
				(field1 & 0xff0000) >> 16,
				TRB_LEN(field2), GET_TD_SIZE(field2),
				GET_INTR_TARGET(field2),
				usbssp_trb_type_string(type),
				field3 & TRB_IDT ? 'I' : 'i',
				field3 & TRB_IOC ? 'I' : 'i',
				field3 & TRB_CYCLE ? 'C' : 'c');
		break;
	case TRB_DATA:
		sprintf(str, "Buffer %08x%08x length %d TD size %d intr %d "
				"type '%s' flags %c:%c:%c:%c:%c:%c:%c",
				field1, field0, TRB_LEN(field2),
				GET_TD_SIZE(field2),
				GET_INTR_TARGET(field2),
				usbssp_trb_type_string(type),
				field3 & TRB_IDT ? 'I' : 'i',
				field3 & TRB_IOC ? 'I' : 'i',
				field3 & TRB_CHAIN ? 'C' : 'c',
				field3 & TRB_NO_SNOOP ? 'S' : 's',
				field3 & TRB_ISP ? 'I' : 'i',
				field3 & TRB_ENT ? 'E' : 'e',
				field3 & TRB_CYCLE ? 'C' : 'c');
		break;
	case TRB_STATUS:
		sprintf(str, "Buffer %08x%08x length %d TD size %d intr"
				"%d type '%s' flags %c:%c:%c:%c",
				field1, field0, TRB_LEN(field2),
				GET_TD_SIZE(field2),
				GET_INTR_TARGET(field2),
				usbssp_trb_type_string(type),
				field3 & TRB_IOC ? 'I' : 'i',
				field3 & TRB_CHAIN ? 'C' : 'c',
				field3 & TRB_ENT ? 'E' : 'e',
				field3 & TRB_CYCLE ? 'C' : 'c');
		break;
	case TRB_NORMAL:
	case TRB_ISOC:
	case TRB_EVENT_DATA:
	case TRB_TR_NOOP:
		sprintf(str,
			"Buffer %08x%08x length %d TD size %d intr %d "
			"type '%s' flags %c:%c:%c:%c:%c:%c:%c:%c",
			field1, field0, TRB_LEN(field2), GET_TD_SIZE(field2),
			GET_INTR_TARGET(field2),
			usbssp_trb_type_string(type),
			field3 & TRB_BEI ? 'B' : 'b',
			field3 & TRB_IDT ? 'I' : 'i',
			field3 & TRB_IOC ? 'I' : 'i',
			field3 & TRB_CHAIN ? 'C' : 'c',
			field3 & TRB_NO_SNOOP ? 'S' : 's',
			field3 & TRB_ISP ? 'I' : 'i',
			field3 & TRB_ENT ? 'E' : 'e',
			field3 & TRB_CYCLE ? 'C' : 'c');
		break;

	case TRB_CMD_NOOP:
	case TRB_ENABLE_SLOT:
		sprintf(str,
			"%s: flags %c",
			usbssp_trb_type_string(type),
			field3 & TRB_CYCLE ? 'C' : 'c');
		break;
	case TRB_DISABLE_SLOT:
		sprintf(str,
			"%s: slot %d flags %c",
			usbssp_trb_type_string(type),
			TRB_TO_SLOT_ID(field3),
			field3 & TRB_CYCLE ? 'C' : 'c');
		break;
	case TRB_ADDR_DEV:
		sprintf(str,
			"%s: ctx %08x%08x slot %d flags %c:%c",
			usbssp_trb_type_string(type),
			field1, field0,
			TRB_TO_SLOT_ID(field3),
			field3 & TRB_BSR ? 'B' : 'b',
			field3 & TRB_CYCLE ? 'C' : 'c');
		break;
	case TRB_CONFIG_EP:
		sprintf(str,
			"%s: ctx %08x%08x slot %d flags %c:%c",
			usbssp_trb_type_string(type),
			field1, field0,
			TRB_TO_SLOT_ID(field3),
			field3 & TRB_DC ? 'D' : 'd',
			field3 & TRB_CYCLE ? 'C' : 'c');
		break;
	case TRB_EVAL_CONTEXT:
		sprintf(str,
			"%s: ctx %08x%08x slot %d flags %c",
			usbssp_trb_type_string(type),
			field1, field0,
			TRB_TO_SLOT_ID(field3),
			field3 & TRB_CYCLE ? 'C' : 'c');
		break;
	case TRB_RESET_EP:
		sprintf(str,
			"%s: ctx %08x%08x slot %d ep %d flags %c",
			usbssp_trb_type_string(type),
			field1, field0,
			TRB_TO_SLOT_ID(field3),
			/* Macro decrements 1, maybe it shouldn't?!? */
			TRB_TO_EP_INDEX(field3) + 1,
			field3 & TRB_CYCLE ? 'C' : 'c');
		break;
	case TRB_STOP_RING:
		sprintf(str,
			"%s: slot %d sp %d ep %d flags %c",
			usbssp_trb_type_string(type),
			TRB_TO_SLOT_ID(field3),
			TRB_TO_SUSPEND_PORT(field3),
			/* Macro decrements 1, maybe it shouldn't?!? */
			TRB_TO_EP_INDEX(field3) + 1,
			field3 & TRB_CYCLE ? 'C' : 'c');
		break;
	case TRB_SET_DEQ:
		sprintf(str,
			"%s: deq %08x%08x stream %d slot %d ep %d flags %c",
			usbssp_trb_type_string(type),
			field1, field0,
			TRB_TO_STREAM_ID(field2),
			TRB_TO_SLOT_ID(field3),
			/* Macro decrements 1, maybe it shouldn't?!? */
			TRB_TO_EP_INDEX(field3) + 1,
			field3 & TRB_CYCLE ? 'C' : 'c');
		break;
	case TRB_RESET_DEV:
		sprintf(str,
			"%s: slot %d flags %c",
			usbssp_trb_type_string(type),
			TRB_TO_SLOT_ID(field3),
			field3 & TRB_CYCLE ? 'C' : 'c');
		break;
	case TRB_FORCE_EVENT:
		sprintf(str,
			"%s: event %08x%08x vf intr %d vf id %d flags %c",
			usbssp_trb_type_string(type),
			field1, field0,
			TRB_TO_VF_INTR_TARGET(field2),
			TRB_TO_VF_ID(field3),
			field3 & TRB_CYCLE ? 'C' : 'c');
		break;
	case TRB_SET_LT:
		sprintf(str,
			"%s: belt %d flags %c",
			usbssp_trb_type_string(type),
			TRB_TO_BELT(field3),
			field3 & TRB_CYCLE ? 'C' : 'c');
		break;
	case TRB_FORCE_HEADER:
		sprintf(str,
			"%s: info %08x%08x%08x pkt type %d roothub port %d flags %c",
			usbssp_trb_type_string(type),
			field2, field1, field0 & 0xffffffe0,
			TRB_TO_PACKET_TYPE(field0),
			TRB_TO_DEV_PORT(field3),
			field3 & TRB_CYCLE ? 'C' : 'c');
		break;
	default:
		sprintf(str,
			"type '%s' -> raw %08x %08x %08x %08x",
			usbssp_trb_type_string(type),
			field0, field1, field2, field3);
	}

	return str;
}

static inline const char *usbssp_decode_slot_context(u32 info, u32 info2,
		u32 int_target, u32 state)
{
	static char str[1024];
	u32 speed;
	int ret = 0;

	speed = info & DEV_SPEED;

	ret = sprintf(str, "%s Ctx Entries %d MEL %d us",
			({ char *s;
			switch (speed) {
			case SLOT_SPEED_FS:
				s = "full-speed";
				break;
			case SLOT_SPEED_LS:
				s = "low-speed";
				break;
			case SLOT_SPEED_HS:
				s = "high-speed";
				break;
			case SLOT_SPEED_SS:
				s = "super-speed";
				break;
			case SLOT_SPEED_SSP:
				s = "super-speed plus";
				break;
			default:
				s = "UNKNOWN speed";
			} s; }),
			(info & LAST_CTX_MASK) >> 27,
			info2 & MAX_EXIT);

	ret += sprintf(str + ret, " [Intr %d] Addr %d State %s",
			GET_INTR_TARGET(int_target),
			state & DEV_ADDR_MASK,
			usbssp_slot_state_string(GET_SLOT_STATE(state)));

	return str;
}


static inline const char *usbssp_portsc_link_state_string(u32 portsc)
{
	switch (portsc & PORT_PLS_MASK) {
	case XDEV_U0:
		return "U0";
	case XDEV_U1:
		return "U1";
	case XDEV_U2:
		return "U2";
	case XDEV_U3:
		return "U3";
	case XDEV_DISABLED:
		return "Disabled";
	case XDEV_RXDETECT:
		return "RxDetect";
	case XDEV_INACTIVE:
		return "Inactive";
	case XDEV_POLLING:
		return "Polling";
	case XDEV_RECOVERY:
		return "Recovery";
	case XDEV_HOT_RESET:
		return "Hot Reset";
	case XDEV_COMP_MODE:
		return "Compliance mode";
	case XDEV_TEST_MODE:
		return "Test mode";
	case XDEV_RESUME:
		return "Resume";
	default:
		break;
	}
	return "Unknown";
}

static inline const char *usbssp_decode_portsc(u32 portsc)
{
	static char str[256];
	int ret;

	ret = sprintf(str, "%s %s %s Link:%s PortSpeed:%d ",
			portsc & PORT_POWER	? "Powered" : "Powered-off",
			portsc & PORT_CONNECT	? "Connected" : "Not-connected",
			portsc & PORT_PE	? "Enabled" : "Disabled",
			usbssp_portsc_link_state_string(portsc),
			DEV_PORT_SPEED(portsc));

	if (portsc & PORT_OC)
		ret += sprintf(str + ret, "OverCurrent ");
	if (portsc & PORT_RESET)
		ret += sprintf(str + ret, "In-Reset ");

	ret += sprintf(str + ret, "Change: ");
	if (portsc & PORT_CSC)
		ret += sprintf(str + ret, "CSC ");
	if (portsc & PORT_PEC)
		ret += sprintf(str + ret, "PEC ");
	if (portsc & PORT_WRC)
		ret += sprintf(str + ret, "WRC ");
	if (portsc & PORT_OCC)
		ret += sprintf(str + ret, "OCC ");
	if (portsc & PORT_RC)
		ret += sprintf(str + ret, "PRC ");
	if (portsc & PORT_PLC)
		ret += sprintf(str + ret, "PLC ");
	if (portsc & PORT_CEC)
		ret += sprintf(str + ret, "CEC ");
	ret += sprintf(str + ret, "Wake: ");
	if (portsc & PORT_WKCONN_E)
		ret += sprintf(str + ret, "WCE ");
	if (portsc & PORT_WKDISC_E)
		ret += sprintf(str + ret, "WDE ");
	if (portsc & PORT_WKOC_E)
		ret += sprintf(str + ret, "WOE ");

	return str;
}

static inline const char *usbssp_ep_state_string(u8 state)
{
	switch (state) {
	case EP_STATE_DISABLED:
		return "disabled";
	case EP_STATE_RUNNING:
		return "running";
	case EP_STATE_HALTED:
		return "halted";
	case EP_STATE_STOPPED:
		return "stopped";
	case EP_STATE_ERROR:
		return "error";
	default:
		return "INVALID";
	}
}

static inline const char *usbssp_ep_type_string(u8 type)
{
	switch (type) {
	case ISOC_OUT_EP:
		return "Isoc OUT";
	case BULK_OUT_EP:
		return "Bulk OUT";
	case INT_OUT_EP:
		return "Int OUT";
	case CTRL_EP:
		return "Ctrl";
	case ISOC_IN_EP:
		return "Isoc IN";
	case BULK_IN_EP:
		return "Bulk IN";
	case INT_IN_EP:
		return "Int IN";
	default:
		return "INVALID";
	}
}

static inline const char *usbssp_decode_ep_context(u32 info, u32 info2, u64 deq,
		u32 tx_info)
{
	static char str[1024];
	int ret;

	u32 esit;
	u16 maxp;
	u16 avg;

	u8 max_pstr;
	u8 ep_state;
	u8 interval;
	u8 ep_type;
	u8 burst;
	u8 cerr;
	u8 mult;

	bool lsa;
	bool hid;

	esit = CTX_TO_MAX_ESIT_PAYLOAD_HI(info) << 16 |
		CTX_TO_MAX_ESIT_PAYLOAD(tx_info);

	ep_state = info & EP_STATE_MASK;
	max_pstr = CTX_TO_EP_MAXPSTREAMS(info);
	interval = CTX_TO_EP_INTERVAL(info);
	mult = CTX_TO_EP_MULT(info) + 1;
	lsa = !!(info & EP_HAS_LSA);

	cerr = (info2 & (3 << 1)) >> 1;
	ep_type = CTX_TO_EP_TYPE(info2);
	hid = !!(info2 & (1 << 7));
	burst = CTX_TO_MAX_BURST(info2);
	maxp = MAX_PACKET_DECODED(info2);

	avg = EP_AVG_TRB_LENGTH(tx_info);

	ret = sprintf(str, "State %s mult %d max P. Streams %d %s",
			usbssp_ep_state_string(ep_state), mult,
			max_pstr, lsa ? "LSA " : "");

	ret += sprintf(str + ret, "interval %d us max ESIT payload %d CErr %d ",
			(1 << interval) * 125, esit, cerr);

	ret += sprintf(str + ret, "Type %s %sburst %d maxp %d deq %016llx ",
			usbssp_ep_type_string(ep_type), hid ? "HID" : "",
			burst, maxp, deq);

	ret += sprintf(str + ret, "avg trb len %d", avg);

	return str;
}

/**
 * next_request - gets the next request on the given list
 * @list: the request list to operate on
 *
 * Caller should take care of locking. This function return %NULL or the first
 * request available on @list.
 */
static inline struct usbssp_request *next_request(struct list_head *list)
{
	return list_first_entry_or_null(list, struct usbssp_request, list);
}

struct usbssp_udc;
#define to_usbssp_ep(ep) (container_of(ep, struct usbssp_ep, endpoint))
#define gadget_to_usbssp(g) (container_of(g, struct usbssp_udc, gadget))
#define request_to_usbssp_request(r) (container_of(r, struct usbssp_request, request))

#define to_usbssp_request(r) (container_of(r, struct usbssp_request, request))

__le32 __iomem *usbssp_get_port_io_addr(struct usbssp_udc *usbssp_data);

void usbssp_giveback_request_in_irq(struct usbssp_udc *usbssp_data,
		struct usbssp_td *cur_td, int status);

void usbssp_remove_request(struct usbssp_udc *usbssp_data,
		struct usbssp_request *req_priv, int ep_index);

#endif /* __LINUX_USBSSP_GADGET_H */
