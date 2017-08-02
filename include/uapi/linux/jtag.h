/*
 * Copyright (c) 2017 Mellanox Technologies. All rights reserved.
 * Copyright (c) 2017 Oleksandr Shamray <oleksandrs@mellanox.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the names of the copyright holders nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 * Alternatively, this software may be distributed under the terms of the
 * GNU General Public License ("GPL") version 2 as published by the Free
 * Software Foundation.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef __UAPI_LINUX_JTAG_H
#define __UAPI_LINUX_JTAG_H

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
 * struct jtag_run_test_idle - forces JTAG sm to
 * RUN_TEST/IDLE state *
 * @mode: access mode
 * @reset: 0 - run IDEL/PAUSE from current state
 *         1 - go trough TEST_LOGIC/RESET state before  IDEL/PAUSE
 * @end: completion flag
 * @tck: clock counter
 *
 * Structure represents interface to JTAG device for jtag idle
 * execution.
 */
struct jtag_run_test_idle {
	enum jtag_xfer_mode	mode;
	unsigned char		reset;
	enum jtag_endstate	endstate;
	unsigned char		tck;
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
	enum jtag_xfer_mode	mode;
	enum jtag_xfer_type	type;
	enum jtag_xfer_direction direction;
	unsigned short		length;
	char			*tdio;
	enum jtag_endstate	endstate;
};

#define __JTAG_IOCTL_MAGIC	0xb2

#define JTAG_IOCRUNTEST	_IOW(__JTAG_IOCTL_MAGIC, 0,\
			     struct jtag_run_test_idle)
#define JTAG_SIOCFREQ	_IOW(__JTAG_IOCTL_MAGIC, 1, unsigned int)
#define JTAG_GIOCFREQ	_IOR(__JTAG_IOCTL_MAGIC, 2, unsigned int)
#define JTAG_IOCXFER	_IOWR(__JTAG_IOCTL_MAGIC, 3, struct jtag_xfer)
#define JTAG_GIOCSTATUS _IOWR(__JTAG_IOCTL_MAGIC, 4, enum jtag_endstate)

#endif /* __UAPI_LINUX_JTAG_H */
