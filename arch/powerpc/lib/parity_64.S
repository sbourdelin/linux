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

#include <asm/processor.h>
#include <asm/ppc_asm.h>

/*
 * This file contains the generic code to calculate the parity
 * of N-bits number, and the POPCNT feature sections.
 *
 * Note: This code relies on -mminimal-toc
 */

#define PARITY_MAGIC 0x6996

/*
 * unsigned int __arch_parity8(unsigned int w)
 */
_GLOBAL(__arch_parity8)
BEGIN_FTR_SECTION
	srdi	r4,r3,4
	xor	r3,r3,r4
	clrldi	r3,r3,64-4
	li	r4,PARITY_MAGIC
	srd	r3,r4,r3
	clrldi	r3,r3,64-1
	blr
FTR_SECTION_ELSE
	PPC_POPCNTB(R3,R3)
	clrldi  r3,r3,64-1
	blr
ALT_FTR_SECTION_END_IFCLR(CPU_FTR_POPCNTB)

/*
 * unsigned int __arch_parity16(unsigned int w)
 */
_GLOBAL(__arch_parity16)
BEGIN_FTR_SECTION
	srdi	r4,r3,8
	xor	r3,r3,r4
	srdi	r4,r3,4
	xor	r3,r3,r4
	clrldi	r3,r3,64-4
	li	r4,PARITY_MAGIC
	srd	r3,r4,r3
	clrldi	r3,r3,64-1
	blr
FTR_SECTION_ELSE
  BEGIN_FTR_SECTION_NESTED(50)
	PPC_POPCNTB(R3,R3)
	srdi	r4,r3,8
	add	r3,r4,r3
	clrldi	r3,r3,64-1
	blr
  FTR_SECTION_ELSE_NESTED(50)
	clrlwi  r3,r3,16
	PPC_POPCNTW(R3,R3)
	clrldi	r3,r3,64-1
	blr
  ALT_FTR_SECTION_END_NESTED_IFCLR(CPU_FTR_POPCNTD, 50)
ALT_FTR_SECTION_END_IFCLR(CPU_FTR_POPCNTB)

/*
 * unsigned int __arch_parity32(unsigned int w)
 */
_GLOBAL(__arch_parity32)
BEGIN_FTR_SECTION
	srdi	r4,r3,16
	xor	r3,r3,r4
	srdi	r4,r3,8
	xor	r3,r3,r4
	srdi	r4,r3,4
	xor	r3,r3,r4
	clrldi	r3,r3,64-4
	li	r4,PARITY_MAGIC
	srd	r3,r4,r3
	clrldi	r3,r3,64-1
	blr
FTR_SECTION_ELSE
  BEGIN_FTR_SECTION_NESTED(51)
	PPC_POPCNTB(R3,R3)
	srdi	r4,r3,16
	add	r3,r4,r3
	srdi	r4,r3,8
	add	r3,r4,r3
	clrldi	r3,r3,64-1
	blr
  FTR_SECTION_ELSE_NESTED(51)
	PPC_POPCNTW(R3,R3)
	clrldi	r3,r3,64-1
	blr
  ALT_FTR_SECTION_END_NESTED_IFCLR(CPU_FTR_POPCNTD, 51)
ALT_FTR_SECTION_END_IFCLR(CPU_FTR_POPCNTB)

/*
 * unsigned int __arch_parity64(__u64 w)
 */
_GLOBAL(__arch_parity64)
BEGIN_FTR_SECTION
	srdi	r4,r3,32
	xor	r3,r3,r4
	srdi	r4,r3,16
	xor	r3,r3,r4
	srdi	r4,r3,8
	xor	r3,r3,r4
	srdi	r4,r3,4
	xor	r3,r3,r4
	clrldi	r3,r3,64-4
	li	r4,PARITY_MAGIC
	srd	r3,r4,r3
	clrldi	r3,r3,64-1
	blr
FTR_SECTION_ELSE
  BEGIN_FTR_SECTION_NESTED(52)
	PPC_POPCNTB(R3,R3)
	srdi	r4,r3,32
	add	r3,r4,r3
	srdi	r4,r3,16
	add	r3,r4,r3
	srdi	r4,r3,8
	add	r3,r4,r3
	clrldi	r3,r3,64-1
	blr
  FTR_SECTION_ELSE_NESTED(52)
	PPC_POPCNTD(R3,R3)
	clrldi	r3,r3,64-1
	blr
  ALT_FTR_SECTION_END_NESTED_IFCLR(CPU_FTR_POPCNTD, 52)
ALT_FTR_SECTION_END_IFCLR(CPU_FTR_POPCNTB)
