/*
 * mlxcpld.h - Mellanox I2C multiplexer support in CPLD
 *
 * Copyright (c) 2016 Mellanox Technologies. All rights reserved.
 * Copyright (c) 2016 Michael Shych <michaels@mellanox.com>
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

#ifndef _LINUX_I2C_MLXCPLD_H
#define _LINUX_I2C_MLXCPLD_H

/* Platform data for the CPLD I2C multiplexers */

/* mlxcpld_mux_platform_mode - per channel initialisation data:
 * @adap_id: bus number for the adapter. 0 = don't care
 * @deselect_on_exit: set this entry to 1, if your H/W needs deselection
 *		      of this channel after transaction.
 */
struct mlxcpld_mux_platform_mode {
	int adap_id;
	unsigned int deselect_on_exit;
};

/* mlxcpld_mux_platform_data - per mux data, used with i2c_register_board_info
 * @modes - mux configuration model
 * @num_modes - number of adapters
 * @sel_reg_addr - mux select register offset in CPLD space
 * @first_channel - first channel to start virtual busses vector
 * @addr - address of mux device - set to mux select register offset on LPC
 *	   connected CPLDs or to I2C address on I2C conncted CPLDs
 */
struct mlxcpld_mux_platform_data {
	struct mlxcpld_mux_platform_mode *modes;
	int num_modes;
	int sel_reg_addr;
	int first_channel;
	unsigned short addr;
};

#endif /* _LINUX_I2C_MLXCPLD_H */
