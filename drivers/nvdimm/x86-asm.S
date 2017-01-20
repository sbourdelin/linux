/*
 * Copyright (c) 2017, Intel Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 */
#include <linux/linkage.h>

/*
 * __arch_memcpy_to_pmem - non-temporal + unordered memory copy
 *
 * 8-byte alignment for destination, source, and len. The results of
 * this transfer are not persistent or globally visible until a
 * sub-sequent sfence (REQ_FLUSH) to the pmem driver.
 *
 * Derived from __copy_user_nocache.
 */
ENTRY(__arch_memcpy_to_pmem)
	/* Set 4x8-byte copy count and remainder */
	movl %edx,%ecx
	andl $63,%edx
	shrl $6,%ecx
	jz .L_8b_pmem_copy_entry	/* jump if count is 0 */

	/* Perform 4x8-byte pmem loop-copy */
.L_4x8b_pmem_copy_loop:
	movq (%rsi),%r8
	movq 1*8(%rsi),%r9
	movq 2*8(%rsi),%r10
	movq 3*8(%rsi),%r11
	movnti %r8,(%rdi)
	movnti %r9,1*8(%rdi)
	movnti %r10,2*8(%rdi)
	movnti %r11,3*8(%rdi)
	movq 4*8(%rsi),%r8
	movq 5*8(%rsi),%r9
	movq 6*8(%rsi),%r10
	movq 7*8(%rsi),%r11
	movnti %r8,4*8(%rdi)
	movnti %r9,5*8(%rdi)
	movnti %r10,6*8(%rdi)
	movnti %r11,7*8(%rdi)
	leaq 64(%rsi),%rsi
	leaq 64(%rdi),%rdi
	decl %ecx
	jnz .L_4x8b_pmem_copy_loop

	/* Set 8-byte copy count and remainder */
.L_8b_pmem_copy_entry:
	movl %edx,%ecx
	andl $7,%edx
	shrl $3,%ecx
	jnz .L_8b_pmem_copy_loop /* continue if count non-zero */
	ret

	/* Perform 8-byte pmem loop-copy */
.L_8b_pmem_copy_loop:
	movq (%rsi),%r8
	movnti %r8,(%rdi)
	leaq 8(%rsi),%rsi
	leaq 8(%rdi),%rdi
	decl %ecx
	jnz .L_8b_pmem_copy_loop
	ret
ENDPROC(__arch_memcpy_to_pmem)
