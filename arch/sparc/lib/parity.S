/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 */

#include <linux/linkage.h>

/*
 * This file contains the generic code to calculate the parity
 * of N-bits number, and the popc-patches.
 */

	.text
	.align	32

#define PARITY_MAGIC 0x6996

/*
 * unsigned int __arch_parity8(unsigned int w)
 */
ENTRY(__arch_parity8)
	srl		%o0, 4, %g1
	xor		%o0, %g1, %o0
	and		%o0, 0xf, %o0
	sethi		%hi(PARITY_MAGIC), %g1
	or		%g1, %lo(PARITY_MAGIC), %g1
	srl		%g1, %o0, %o0
	retl
	 and		%o0, 1, %o0
ENDPROC(__arch_parity8)
	.section	.popc_6insn_patch, "ax"
	.word		__arch_parity8
	sllx		%o0, 64-8, %g1
	popc		%g1, %o0
	retl
	 and		%o0, 1, %o0
	nop
	nop
	.previous

/*
 * unsigned int __arch_parity16(unsigned int w)
 */
ENTRY(__arch_parity16)
	srl		%o0, 8, %g1
	xor		%o0, %g1, %o0
	srl		%o0, 4, %g1
	xor		%o0, %g1, %o0
	and		%o0, 0xf, %o0
	sethi		%hi(PARITY_MAGIC), %g1
	or		%g1, %lo(PARITY_MAGIC), %g1
	srl		%g1, %o0, %o0
	retl
	 and		%o0, 1, %o0
ENDPROC(__arch_parity16)
	.section	.popc_6insn_patch, "ax"
	.word		__arch_parity16
	sllx		%o0, 64-16, %g1
	popc		%g1, %o0
	retl
	 and		%o0, 1, %o0
	nop
	nop
	.previous

/*
 * unsigned int __arch_parity32(unsigned int w)
 */
ENTRY(__arch_parity32)
	srl		%o0, 16, %g1
	xor		%o0, %g1, %o0
	srl		%o0, 8, %g1
	xor		%o0, %g1, %o0
	srl		%o0, 4, %g1
	xor		%o0, %g1, %o0
	and		%o0, 0xf, %o0
	sethi		%hi(PARITY_MAGIC), %g1
	or		%g1, %lo(PARITY_MAGIC), %g1
	srl		%g1, %o0, %o0
	retl
	 and		%o0, 1, %o0
ENDPROC(__arch_parity32)
	.section	.popc_6insn_patch, "ax"
	.word		__arch_parity32
	sllx		%o0, 64-32, %g1
	popc		%g1, %o0
	retl
	 and		%o0, 1, %o0
	nop
	nop
	.previous

/*
 * unsigned int __arch_parity64(__u64 w)
 */
ENTRY(__arch_parity64)
	srlx	%o0, 32, %g1
	xor		%o0, %g1, %o0
	srl		%o0, 16, %g1
	xor		%o0, %g1, %o0
	srl		%o0, 8, %g1
	xor		%o0, %g1, %o0
	srl		%o0, 4, %g1
	xor		%o0, %g1, %o0
	and		%o0, 0xf, %o0
	sethi		%hi(PARITY_MAGIC), %g1
	or		%g1, %lo(PARITY_MAGIC), %g1
	srl		%g1, %o0, %o0
	retl
	 and		%o0, 1, %o0
ENDPROC(__arch_parity64)
	.section	.popc_3insn_patch, "ax"
	.word		__arch_parity64
	popc		%o0, %o0
	retl
	 and		%o0, 1, %o0
	.previous
