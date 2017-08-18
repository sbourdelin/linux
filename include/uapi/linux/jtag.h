/*
 * JTAG class driver
 *
 * Copyright (c) 2017 Mellanox Technologies. All rights reserved.
 * Copyright (c) 2017 Oleksandr Shamray <oleksandrs@mellanox.com>
 *
 * Released under the GPLv2/BSD.
 * SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
 */

#ifndef __UAPI_LINUX_JTAG_H
#define __UAPI_LINUX_JTAG_H

#include <linux/types.h>

/**
 * enum jtag_xfer_mode:
 *
 * @JTAG_XFER_HW_MODE: hardware mode transfer
 * @JTAG_XFER_SW_MODE: software mode transfer
 */
enum jtag_xfer_mode {
	JTAG_XFER_HW_MODE,
	JTAG_XFER_SW_MODE,
};

/**
 * enum jtag_endstate:
 *
 * @JTAG_STATE_IDLE: JTAG state machine IDLE state
 * @JTAG_STATE_PAUSEIR: JTAG state machine PAUSE_IR state
 * @JTAG_STATE_PAUSEDR: JTAG state machine PAUSE_DR state
 */
enum jtag_endstate {
	JTAG_STATE_IDLE,
	JTAG_STATE_PAUSEIR,
	JTAG_STATE_PAUSEDR,
};

/**
 * enum jtag_xfer_type:
 *
 * @JTAG_SIR_XFER: SIR transfer
 * @JTAG_SDR_XFER: SDR transfer
 */
enum jtag_xfer_type {
	JTAG_SIR_XFER,
	JTAG_SDR_XFER,
};

/**
 * enum jtag_xfer_direction:
 *
 * @JTAG_READ_XFER: read transfer
 * @JTAG_WRITE_XFER: write transfer
 */
enum jtag_xfer_direction {
	JTAG_READ_XFER,
	JTAG_WRITE_XFER,
};

/**
 * struct jtag_run_test_idle - forces JTAG state machine to
 * RUN_TEST/IDLE state
 *
 * @mode: access mode
 * @reset: 0 - run IDLE/PAUSE from current state
 *         1 - go through TEST_LOGIC/RESET state before  IDLE/PAUSE
 * @end: completion flag
 * @tck: clock counter
 *
 * Structure represents interface to JTAG device for jtag idle
 * execution.
 */
struct jtag_run_test_idle {
	__u8	mode;
	__u8	reset;
	__u8	endstate;
	__u8	tck;
};

/**
 * struct jtag_xfer - jtag xfer:
 *
 * @mode: access mode
 * @type: transfer type
 * @direction: xfer direction
 * @length: xfer bits len
 * @tdio : xfer data array
 * @endir: xfer end state
 *
 * Structure represents interface to Aspeed JTAG device for jtag sdr xfer
 * execution.
 */
struct jtag_xfer {
	__u8	mode;
	__u8	type;
	__u8	direction;
	__u32	length;
	__u8	*tdio;
	__u8	endstate;
};

#define __JTAG_IOCTL_MAGIC	0xb2

#define JTAG_IOCRUNTEST	_IOW(__JTAG_IOCTL_MAGIC, 0,\
			     struct jtag_run_test_idle)
#define JTAG_SIOCFREQ	_IOW(__JTAG_IOCTL_MAGIC, 1, unsigned int)
#define JTAG_GIOCFREQ	_IOR(__JTAG_IOCTL_MAGIC, 2, unsigned int)
#define JTAG_IOCXFER	_IOWR(__JTAG_IOCTL_MAGIC, 3, struct jtag_xfer)
#define JTAG_GIOCSTATUS _IOWR(__JTAG_IOCTL_MAGIC, 4, enum jtag_endstate)

#endif /* __UAPI_LINUX_JTAG_H */
