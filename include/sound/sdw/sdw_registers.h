/*
 * sdw_registers.h - SoundWire MIPI spec 1.1 defined SoundWire Slave
 *	register definition file. This defines the register offsets, bit
 *	mask and bit shift in accordance with MIPI spec 1.1.
 *
 * Author: Hardik Shah <hardik.t.shah@intel.com>
 *
 *
 * This header file refers to the MIPI SoundWire 1.1 Spec.
 * [1.1] https://members.mipi.org/wg/All-Members/document (accessible to
 * MIPI members).
 *
 * The comments in the file try to follow the same conventions with a
 * capital letter for all standard definitions such as Master, Slave,
 * Data Port, etc. When possible, the constant numeric values are kept
 * the same as in the MIPI specifications. All of the constants reflect
 * the MIPI SoundWire definitions and are not vendor specific. Some of
 * the constants are for bus driver usage and not MIPI defined. These
 * are specified at appropriate place in this file.
 *
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 *
 * This file is provided under a dual BSD/GPLv2 license.  When using or
 * redistributing this file, you may do so under either license.
 *
 * GPL LICENSE SUMMARY
 *
 * Copyright(c) 2016 Intel Corporation.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 *
 * BSD LICENSE
 *
 * Copyright(c) 2016 Intel Corporation.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in
 *     the documentation and/or other materials provided with the
 *     distribution.
 *   * Neither the name of Intel Corporation nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 *
 */

#ifndef _LINUX_SDW_REG_H
#define _LINUX_SDW_REG_H

#define SDW_SCP_ADDRPAGE1_MASK				0xFF
#define SDW_SCP_ADDRPAGE1_SHIFT				15

#define SDW_SCP_ADDRPAGE2_MASK				0xFF
#define SDW_SCP_ADDRPAGE2_SHIFT				22

#define SDW_REGADDR_SHIFT				0x0
#define SDW_REGADDR_MASK				0xFFFF

#define SDW_MAX_REG_ADDR				65536


#define SDW_NUM_DATA_PORT_REGISTERS			0x100
#define SDW_BANK1_REGISTER_OFFSET			0x10

#define SDW_DP0_INTSTAT					0x0
#define SDW_DP0_INTSTAT_TEST_FAIL_MASK			BIT_MASK(0)
#define SDW_DP0_INTSTAT_PORT_READY_MASK			BIT_MASK(1)
#define SDW_DP0_INTSTAT_BRA_FAILURE_MASK		BIT_MASK(2)
#define SDW_DP0_INTSTAT_IMPDEF1_MASK			BIT_MASK(5)
#define SDW_DP0_INTSTAT_IMPDEF2_MASK			BIT_MASK(6)
#define SDW_DP0_INTSTAT_IMPDEF3_MASK			BIT_MASK(7)

#define SDW_DP0_INTCLEAR				0x0
#define SDW_DP0_INTCLEAR_TEST_FAIL_MASK			BIT_MASK(0)
#define SDW_DP0_INTCLEAR_PORT_READY_MASK		BIT_MASK(1)
#define SDW_DP0_INTCLEAR_BRA_FAILURE_MASK		BIT_MASK(2)
#define SDW_DP0_INTCLEAR_IMPDEF1_MASK			BIT_MASK(5)
#define SDW_DP0_INTCLEAR_IMPDEF2_MASK			BIT_MASK(6)
#define SDW_DP0_INTCLEAR_IMPDEF3_MASK			BIT_MASK(7)

#define SDW_DP0_INTMASK					0x1
#define SDW_DP0_INTMASK_TEST_FAIL_MASK			BIT_MASK(0)
#define SDW_DP0_INTMASK_PORT_READY_MASK			BIT_MASK(1)
#define SDW_DP0_INTMASK_BRA_FAILURE_MASK		BIT_MASK(2)
#define SDW_DP0_INTMASK_IMPDEF1_MASK			BIT_MASK(5)
#define SDW_DP0_INTMASK_IMPDEF2_MASK			BIT_MASK(6)
#define SDW_DP0_INTMASK_IMPDEF3_MASK			BIT_MASK(7)

#define SDW_DP0_PORTCTRL				0x2
#define SDW_DP0_PORTCTRL_PORTDATAMODE_MASK		GENMASK(3, 2)
#define SDW_DP0_PORTCTRL_PORTDATAMODE_SHIFT		2
#define SDW_DP0_PORTCTRL_NEXTINVERTBANK_MASK		BIT_MASK(4)
#define SDW_DP0_PORTCTRL_NEXTINVERTBANK_SHIFT		4
#define SDW_DP0_PORTCTRL_BPT_PAYLD_TYPE_MASK		GENMASK(7, 6)
#define SDW_DP0_PORTCTRL_BPT_PAYLD_TYPE_SHIFT		6


#define SDW_DP0_BLOCKCTRL1				0x3

#define SDW_DP0_PREPARESTATUS				0x4

#define SDW_DP0_PREPARECTRL				0x5

#define SDW_DP0_CHANNELEN				0x20
#define SDW_DP0_SAMPLECTRL1				0x22
#define SDW_DP0_SAMPLECTRL2				0x23
#define SDW_DP0_OFFSETCTRL1				0x24
#define SDW_DP0_OFFSETCTRL2				0x25
#define SDW_DP0_HCTRL					0x26
#define SDW_DP0_LANECTRL				0x28

#define SDW_SCP_INTSTAT1				0x40
#define SDW_SCP_INTSTAT1_PARITY_MASK			BIT_MASK(0)
#define SDW_SCP_INTSTAT1_BUS_CLASH_MASK			BIT_MASK(1)
#define SDW_SCP_INTSTAT1_IMPL_DEF_MASK			BIT_MASK(2)
#define SDW_SCP_INTSTAT1_SCP2_CASCADE_MASK		BIT_MASK(7)

#define SDW_SCP_INTCLEAR1				0x40
#define SDW_SCP_INTCLEAR1_PARITY_MASK			BIT_MASK(0)
#define SDW_SCP_INTCLEAR1_BUS_CLASH_MASK		BIT_MASK(1)
#define SDW_SCP_INTCLEAR1_IMPL_DEF_MASK			BIT_MASK(2)

#define SDW_SCP_INTMASK1				0x41
#define SDW_SCP_INTMASK1_PARITY_MASK			BIT_MASK(0)
#define SDW_SCP_INTMASK1_BUS_CLASH_MASK			BIT_MASK(1)
#define SDW_SCP_INTMASK1_IMPL_DEF_MASK			BIT_MASK(2)


#define SDW_SCP_INTSTAT2				0x42
#define SDW_SCP_INTSTAT2_SCP3_CASCADE_MASK		BIT_MASK(7)


#define SDW_SCP_INTSTAT3				0x43

/* Number of interrupt status registers */
#define SDW_NUM_INT_STAT_REGISTERS			3

/* Number of interrupt clear registers */
#define SDW_NUM_INT_CLEAR_REGISTERS			1

#define SDW_SCP_CTRL					0x44
#define SDW_SCP_CTRL_CLK_STP_NOW_MASK			BIT_MASK(1)
#define SDW_SCP_CTRL_FORCE_RESET_MASK			BIT_MASK(2)
#define SDW_SCP_CTRL_CLK_STP_NOW_SHIFT			0x1

#define SDW_SCP_STAT					0x44
#define SDW_SCP_STAT_CLK_STP_NF_MASK			BIT_MASK(0)

#define SDW_SCP_SYSTEMCTRL				0x45
#define SDW_SCP_SYSTEMCTRL_CLK_STP_PREP_MASK		BIT_MASK(0)
#define SDW_SCP_SYSTEMCTRL_CLK_STP_MODE_MASK		BIT_MASK(2)
#define SDW_SCP_SYSTEMCTRL_WAKE_UP_EN_MASK		BIT_MASK(3)
#define SDW_SCP_SYSTEMCTRL_HIGH_PHY_MASK		BIT_MASK(4)

#define SDW_SCP_SYSTEMCTRL_CLK_STP_PREP_SHIFT		0x0
#define SDW_SCP_SYSTEMCTRL_CLK_STP_MODE_SHIFT		0x2
#define SDW_SCP_SYSTEMCTRL_WAKE_UP_EN_SHIFT		0x3
#define SDW_SCP_SYSTEMCTRL_HIGH_PHY_SHIFT		0x4

#define SDW_SCP_DEVNUMBER				0x46
#define SDW_SCP_HIGH_PHY_CHECK				0x47
#define SDW_SCP_ADDRPAGE1				0x48
#define SDW_SCP_ADDRPAGE2				0x49
#define SDW_SCP_KEEPEREN				0x4A
#define SDW_SCP_BANKDELAY				0x4B
#define SDW_SCP_TESTMODE				0x4F
#define SDW_SCP_DEVID_0					0x50
#define SDW_SCP_DEVID_1					0x51
#define SDW_SCP_DEVID_2					0x52
#define SDW_SCP_DEVID_3					0x53
#define SDW_SCP_DEVID_4					0x54
#define SDW_SCP_DEVID_5					0x55

/* Banked Registers */
#define SDW_SCP_FRAMECTRL				0x60
#define SDW_SCP_NEXTFRAME				0x61

#define SDW_DPN_INTSTAT					0x0
#define SDW_DPN_INTSTAT_TEST_FAIL_MASK			BIT_MASK(0)
#define SDW_DPN_INTSTAT_PORT_READY_MASK			BIT_MASK(1)
#define SDW_DPN_INTSTAT_IMPDEF1_MASK			BIT_MASK(5)
#define SDW_DPN_INTSTAT_IMPDEF2_MASK			BIT_MASK(6)
#define SDW_DPN_INTSTAT_IMPDEF3_MASK			BIT_MASK(7)

#define SDW_DPN_INTCLEAR				0x0
#define SDW_DPN_INTCLEAR_TEST_FAIL_MASK			BIT_MASK(0)
#define SDW_DPN_INTCLEAR_PORT_READY_MASK		BIT_MASK(1)
#define SDW_DPN_INTCLEAR_IMPDEF1_MASK			BIT_MASK(5)
#define SDW_DPN_INTCLEAR_IMPDEF2_MASK			BIT_MASK(6)
#define SDW_DPN_INTCLEAR_IMPDEF3_MASK			BIT_MASK(7)

#define SDW_DPN_INTMASK					0x1
#define SDW_DPN_INTMASK_TEST_FAIL_MASK			BIT_MASK(0)
#define SDW_DPN_INTMASK_PORT_READY_MASK			BIT_MASK(1)
#define SDW_DPN_INTMASK_IMPDEF1_MASK			BIT_MASK(5)
#define SDW_DPN_INTMASK_IMPDEF2_MASK			BIT_MASK(6)
#define SDW_DPN_INTMASK_IMPDEF3_MASK			BIT_MASK(7)

#define SDW_DPN_PORTCTRL				0x2
#define SDW_DPN_PORTCTRL_PORTFLOWMODE_MASK		GENMASK(1, 0)
#define SDW_DPN_PORTCTRL_PORTFLOWMODE_SHIFT		0
#define SDW_DPN_PORTCTRL_PORTDATAMODE_MASK		GENMASK(3, 2)
#define SDW_DPN_PORTCTRL_PORTDATAMODE_SHIFT		2
#define SDW_DPN_PORTCTRL_NEXTINVERTBANK_MASK		BIT_MASK(4)
#define SDW_DPN_PORTCTRL_NEXTINVERTBANK_SHIFT		4

#define SDW_DPN_BLOCKCTRL1				0x3
#define SDW_DPN_BLOCKCTRL1_WORDLENGTH_MASK		GENMASK(5, 0)
#define SDW_DPN_BLOCKCTRL1_WORDLENGTH_SHIFT		0

#define SDW_DPN_PREPARESTATUS				0x4
#define SDW_DPN_PREPARECTRL				0x5
#define SDW_DPN_PREPARECTRL_CH_PREPARE_MASK		GENMASK(7, 0)

#define SDW_DPN_CHANNELEN				0x20
#define SDW_DPN_BLOCKCTRL2				0x21
#define SDW_DPN_SAMPLECTRL1				0x22

#define SDW_DPN_SAMPLECTRL1_LOW_MASK			GENMASK(7, 0)
#define SDW_DPN_SAMPLECTRL2				0x23
#define SDW_DPN_SAMPLECTRL2_SHIFT			8
#define SDW_DPN_SAMPLECTRL2_LOW_MASK			GENMASK(7, 0)
#define SDW_DPN_OFFSETCTRL1				0x24
#define SDW_DPN_OFFSETCTRL2				0x25
#define SDW_DPN_HCTRL					0x26
#define SDW_DPN_HCTRL_HSTART_MASK			GENMASK(7, 4)
#define SDW_DPN_HCTRL_HSTOP_MASK			GENMASK(3, 0)
#define SDW_DPN_HCTRL_HSTART_SHIFT			4
#define SDW_DPN_HCTRL_HSTOP_SHIFT			0
#define SDW_DPN_BLOCKCTRL3				0x27
#define SDW_DPN_LANECTRL				0x28


#define SDW_NUM_CASC_PORT_INTSTAT1			4
#define SDW_CASC_PORT_START_INTSTAT1			0
#define SDW_CASC_PORT_MASK_INTSTAT1			0x8
#define SDW_CASC_PORT_REG_OFFSET_INTSTAT1		0x0

#define SDW_NUM_CASC_PORT_INTSTAT2			7
#define SDW_CASC_PORT_START_INTSTAT2			4
#define SDW_CASC_PORT_MASK_INTSTAT2			1
#define SDW_CASC_PORT_REG_OFFSET_INTSTAT2		1

#define SDW_NUM_CASC_PORT_INTSTAT3			4
#define SDW_CASC_PORT_START_INTSTAT3			11
#define SDW_CASC_PORT_MASK_INTSTAT3			1
#define SDW_CASC_PORT_REG_OFFSET_INTSTAT3		2


#endif
