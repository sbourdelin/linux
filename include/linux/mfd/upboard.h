// SPDX-License-Identifier: GPL-2.0
//
// UP Board MFD driver interface
//
// Copyright (c) 2018, Emutex Ltd.
//
// Author: Javier Arteaga <javier@emutex.com>
//

#ifndef __LINUX_MFD_UPBOARD_H
#define __LINUX_MFD_UPBOARD_H

#define UPBOARD_REGISTER_SIZE 16

/**
 * enum upboard_reg - addresses for 16-bit controller registers
 *
 * @UPBOARD_REG_PLATFORM_ID:    [RO] BOARD_ID | MANUFACTURER_ID
 * @UPBOARD_REG_FIRMWARE_ID:    [RO] BUILD | MAJOR | MINOR | PATCH
 * @UPBOARD_REG_FUNC_EN0:       [RW] Toggles for board functions (bank 0)
 * @UPBOARD_REG_FUNC_EN1:       [RW] Toggles for board functions (bank 1)
 * @UPBOARD_REG_GPIO_EN0:       [RW] Hi-Z (0) / enabled (1) GPIO (bank 0)
 * @UPBOARD_REG_GPIO_EN1:       [RW] Hi-Z (0) / enabled (1) GPIO (bank 1)
 * @UPBOARD_REG_GPIO_EN2:       [RW] Hi-Z (0) / enabled (1) GPIO (bank 2)
 * @UPBOARD_REG_GPIO_DIR0:      [RW] SoC- (0) / FPGA- (1) driven GPIO (bank 0)
 * @UPBOARD_REG_GPIO_DIR1:      [RW] SoC- (0) / FPGA- (1) driven GPIO (bank 1)
 * @UPBOARD_REG_GPIO_DIR2:      [RW] SoC- (0) / FPGA- (1) driven GPIO (bank 2)
 * @UPBOARD_REG_MAX: one past the last valid address
 */
enum upboard_reg {
	UPBOARD_REG_PLATFORM_ID   = 0x10,
	UPBOARD_REG_FIRMWARE_ID   = 0x11,
	UPBOARD_REG_FUNC_EN0      = 0x20,
	UPBOARD_REG_FUNC_EN1      = 0x21,
	UPBOARD_REG_GPIO_EN0      = 0x30,
	UPBOARD_REG_GPIO_EN1      = 0x31,
	UPBOARD_REG_GPIO_EN2      = 0x32,
	UPBOARD_REG_GPIO_DIR0     = 0x40,
	UPBOARD_REG_GPIO_DIR1     = 0x41,
	UPBOARD_REG_GPIO_DIR2     = 0x42,
	UPBOARD_REG_MAX,
};

#endif /*  __LINUX_MFD_UPBOARD_H */
