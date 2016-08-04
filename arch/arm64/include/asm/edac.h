/*
 * Copyright 2011 Calxeda, Inc.
 * Based on PPC version Copyright 2007 MontaVista Software, Inc.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#ifndef ASM_EDAC_H
#define ASM_EDAC_H
/*
 * ECC atomic, DMA, SMP and interrupt safe scrub function.
 * Implements the per arch atomic_scrub() that EDAC use for software
 * ECC scrubbing.  It reads memory and then writes back the original
 * value, allowing the hardware to detect and correct memory errors.
 */
static inline void atomic_scrub(void *va, u32 size)
{
	unsigned long *virt_addr = va;
	unsigned long temp;
	unsigned int temp2;
	unsigned int i;

	for (i = 0; i < size / sizeof(*virt_addr); i++, virt_addr++) {
		/* Very carefully read and write to memory atomically
		 * so we are interrupt, DMA and SMP safe.
		 */
		__asm__ __volatile__("\n"
			"1:	ldxr	%0, [%2]\n"
			"	stxr	%w1, %0, [%2]\n"
			"	cbnz	%w1, 1b\n"
			: "=&r"(temp), "=&r"(temp2)
			: "r"(virt_addr)
			: "memory");
	}
}

#endif
