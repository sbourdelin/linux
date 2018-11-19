/*
 * Accelerated CRC32(C) using AArch64 CRC instructions
 *
 * Copyright (C) 2016 - 2018 Linaro Ltd <ard.biesheuvel@linaro.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/linkage.h>
#include <asm/alternative.h>
#include <asm/assembler.h>

	.cpu		generic+crc

	.macro		__crc32, c

64: cmp     x2, #64
    b.lt    32f

    adds    x11, x1, #16
    adds    x12, x1, #32
    adds    x13, x1, #48

0 : subs    x2, x2, #64
    b.mi    32f

    ldp     x3, x4, [x1], #64
    ldp     x5, x6, [x11], #64
    ldp     x7, x8, [x12], #64
    ldp     x9, x10,[x13], #64

 CPU_BE( rev     x3, x3      )
 CPU_BE( rev     x4, x4      )
 CPU_BE( rev     x5, x5      )
 CPU_BE( rev     x6, x6      )
 CPU_BE( rev     x7, x7      )
 CPU_BE( rev     x8, x8      )
 CPU_BE( rev     x9, x9      )
 CPU_BE( rev     x10,x10     )

    crc32\c\()x w0, w0, x3
    crc32\c\()x w0, w0, x4
    crc32\c\()x w0, w0, x5
    crc32\c\()x w0, w0, x6
    crc32\c\()x w0, w0, x7
    crc32\c\()x w0, w0, x8
    crc32\c\()x w0, w0, x9
    crc32\c\()x w0, w0, x10

    b.ne       0b
    ret

32: tbz     x2, #5, 16f
    ldp     x3, x4, [x1], #16
    ldp     x5, x6, [x1], #16
CPU_BE( rev     x3, x3      )
CPU_BE( rev     x4, x4      )
CPU_BE( rev     x5, x5      )
CPU_BE( rev     x6, x6      )
    crc32\c\()x w0, w0, x3
    crc32\c\()x w0, w0, x4
    crc32\c\()x w0, w0, x5
    crc32\c\()x w0, w0, x6

16: tbz     x2, #4, 8f
	ldp		x3, x4, [x1], #16
CPU_BE(	rev		x3, x3		)
CPU_BE(	rev		x4, x4		)
	crc32\c\()x	w0, w0, x3
	crc32\c\()x	w0, w0, x4

8:	tbz		x2, #3, 4f
	ldr		x3, [x1], #8
CPU_BE(	rev		x3, x3		)
	crc32\c\()x	w0, w0, x3
4:	tbz		x2, #2, 2f
	ldr		w3, [x1], #4
CPU_BE(	rev		w3, w3		)
	crc32\c\()w	w0, w0, w3
2:	tbz		x2, #1, 1f
	ldrh		w3, [x1], #2
CPU_BE(	rev16		w3, w3		)
	crc32\c\()h	w0, w0, w3
1:	tbz		x2, #0, 0f
	ldrb		w3, [x1]
	crc32\c\()b	w0, w0, w3
0:	ret
	.endm

	.align		5
ENTRY(crc32_le)
alternative_if_not ARM64_HAS_CRC32
	b		crc32_le_base
alternative_else_nop_endif
	__crc32
ENDPROC(crc32_le)

	.align		5
ENTRY(__crc32c_le)
alternative_if_not ARM64_HAS_CRC32
	b		__crc32c_le_base
alternative_else_nop_endif
	__crc32		c
ENDPROC(__crc32c_le)
