/*
 * Copyright (C) 2015 ARM Limited
 *
 * Author: Vladimir Murzin <vladimir.murzin@arm.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

	.macro	addruart, rp, tmp1, tmp2
	ldr	\rp, =CONFIG_DEBUG_UART_PHYS
	.endm

	.macro	senduart, rd, rx
	strb	\rd, [\rx]		@ Data Register
	.endm

	.macro	busyuart, rd, rx
1001:	ldrb	\rd, [\rx, #0x4]	@ State Register
	tst	\rd, #1			@ busy
	bne	1001b			@ wait until transmit done
	.endm

	.macro	waituart,rd,rx
	.endm
