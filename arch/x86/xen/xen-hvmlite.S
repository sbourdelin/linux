/*
 * Copyright C 2016, Oracle and/or its affiliates. All rights reserved.
 *
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
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

	.code32
	.text
#define _pa(x)          ((x) - __START_KERNEL_map)

#include <linux/elfnote.h>
#include <linux/init.h>
#include <linux/linkage.h>
#include <asm/segment.h>
#include <asm/asm.h>
#include <asm/boot.h>
#include <asm/processor-flags.h>
#include <asm/msr.h>
#include <xen/interface/elfnote.h>

	__HEAD
	.code32

/* Entry point for HVMlite guests */
ENTRY(hvmlite_start_xen)
	cli
	cld

	mov $_pa(gdt), %eax
	lgdt (%eax)

	movl $(__BOOT_DS),%eax
	movl %eax,%ds
	movl %eax,%es
	movl %eax,%ss

	/* Stash hvm_start_info */
	mov $_pa(hvmlite_start_info), %edi
	mov %ebx, %esi
	mov $_pa(hvmlite_start_info_sz), %ecx
	mov (%ecx), %ecx
	rep
	movsb

	movl $_pa(early_stack_end), %eax
	movl %eax, %esp

	/* Enable PAE mode */
	movl %cr4, %eax
	orl $X86_CR4_PAE, %eax
	movl %eax, %cr4

#ifdef CONFIG_X86_64
	/* Enable Long mode */
	movl $MSR_EFER, %ecx
	rdmsr
	btsl $_EFER_LME, %eax
	wrmsr

	/* Enable pre-constructed page tables */
	mov $_pa(init_level4_pgt), %eax
	movl %eax, %cr3
	movl $(X86_CR0_PG | X86_CR0_PE), %eax
	movl %eax, %cr0

	/* Jump to 64-bit mode. */
	pushl $__KERNEL_CS
	leal _pa(1f), %eax
	pushl %eax
	lret

	/* 64-bit entry point */
	.code64
1:
	call xen_prepare_hvmlite

	/* startup_64 expects boot_params in %rsi */
	mov $_pa(xen_hvmlite_boot_params), %rsi
	movq $_pa(startup_64), %rax
	jmp *%rax

#else /* CONFIG_X86_64 */

	/* Clear boot page tables */
	movl $_pa(early_pgtable), %edi
	xorl %eax, %eax
	movl $((PAGE_SIZE*5)/4), %ecx
	rep stosl

	/* Level 3 */
	movl $_pa(early_pgtable), %edi
	leal (PAGE_SIZE +_PAGE_PRESENT)(%edi), %eax
	movl $4, %ecx
1:
	movl %eax, 0x00(%edi)
	addl $8, %edi
	decl %ecx
	jnz 1b

	/* Level 2 (2M entries) */
	movl $(_pa(early_pgtable) + PAGE_SIZE), %edi
	movl $(_PAGE_PSE | _PAGE_RW | _PAGE_PRESENT), %eax
	movl $2048, %ecx
2:
	movl %eax, 0(%edi)
	addl $0x00200000, %eax
	addl $8, %edi
	decl %ecx
	jnz 2b

	/* Enable the boot paging */
	movl $_pa(early_pgtable), %eax
	movl %eax, %cr3
	movl %cr0, %eax
	orl $(X86_CR0_PG | X86_CR0_PE), %eax
	movl %eax, %cr0

	ljmp $__BOOT_CS,$3f
3:
	call xen_prepare_hvmlite
	mov $_pa(xen_hvmlite_boot_params), %esi

	/* startup_32 doesn't expect paging and PAE to be on */
	ljmp $__BOOT_CS,$_pa(4f)
4:
	movl %cr0, %eax
	andl $~X86_CR0_PG, %eax
	movl %eax, %cr0
	movl %cr4, %eax
	andl $~X86_CR4_PAE, %eax
	movl %eax, %cr4

	ljmp    $0x10, $_pa(startup_32)
#endif

	.data
gdt:
	.word	gdt_end - gdt
	.long	_pa(gdt)
	.word	0
	.quad	0x0000000000000000 /* NULL descriptor */
#ifdef CONFIG_X86_64
	.quad	0x00af9a000000ffff /* __KERNEL_CS */
#else
	.quad	0x00cf9a000000ffff /* __KERNEL_CS */
#endif
	.quad	0x00cf92000000ffff /* __KERNEL_DS */
gdt_end:

	.bss
	.balign 4
early_stack:
	.fill 16, 1, 0
early_stack_end:

#ifdef CONFIG_X86_32
	.section ".pgtable","a",@nobits
	.balign 4096
early_pgtable:
	.fill 5*4096, 1, 0
#endif

	ELFNOTE(Xen, XEN_ELFNOTE_PHYS32_ENTRY,
	             _ASM_PTR (hvmlite_start_xen - __START_KERNEL_map))
