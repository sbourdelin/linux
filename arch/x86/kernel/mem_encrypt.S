/*
 * AMD Memory Encryption Support
 *
 * Copyright (C) 2016 Advanced Micro Devices, Inc.
 *
 * Author: Tom Lendacky <thomas.lendacky@amd.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/linkage.h>

	.text
	.code64
ENTRY(sme_get_me_loss)
	xor	%rax, %rax
	mov	sme_me_loss(%rip), %al
	ret
ENDPROC(sme_get_me_loss)

	.data
	.align 16
ENTRY(sme_me_mask)
	.quad	0x0000000000000000
sme_me_loss:
	.byte	0x00
	.align	8
