/*
 * S.A.R.A. Linux Security Module
 *
 * Copyright (C) 2017 Salvatore Mesoraca <s.mesoraca16@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2, as
 * published by the Free Software Foundation.
 *
 * Assembly sequences used here were copied from
 * PaX patch by PaX Team <pageexec@freemail.hu>
 * Being just hexadecimal constants, they are not subject to
 * any copyright.
 *
 */

#ifndef __SARA_TRAMPOLINES_H
#define __SARA_TRAMPOLINES_H
#ifdef CONFIG_SECURITY_SARA_WXPROT_EMUTRAMP


/* x86_32 */


struct libffi_trampoline_x86_32 {
	unsigned char mov;
	unsigned int addr1;
	unsigned char jmp;
	unsigned int addr2;
} __packed;

struct gcc_trampoline_x86_32_type1 {
	unsigned char mov1;
	unsigned int addr1;
	unsigned char mov2;
	unsigned int addr2;
	unsigned short jmp;
} __packed;

struct gcc_trampoline_x86_32_type2 {
	unsigned char mov;
	unsigned int addr1;
	unsigned char jmp;
	unsigned int addr2;
} __packed;

union trampolines_x86_32 {
	struct libffi_trampoline_x86_32 lf;
	struct gcc_trampoline_x86_32_type1 g1;
	struct gcc_trampoline_x86_32_type2 g2;
};

#define is_valid_libffi_trampoline_x86_32(UNION)	\
	(UNION.lf.mov == 0xB8 &&			\
	UNION.lf.jmp == 0xE9)

#define emulate_libffi_trampoline_x86_32(UNION, REGS) do {	\
	(REGS)->ax = UNION.lf.addr1;				\
	(REGS)->ip = (unsigned int) ((REGS)->ip +		\
				     UNION.lf.addr2 +		\
				     sizeof(UNION.lf));		\
} while (0)

#define is_valid_gcc_trampoline_x86_32_type1(UNION, REGS)	\
	(UNION.g1.mov1 == 0xB9 &&				\
	UNION.g1.mov2 == 0xB8 &&				\
	UNION.g1.jmp == 0xE0FF &&				\
	REGS->ip > REGS->sp)

#define emulate_gcc_trampoline_x86_32_type1(UNION, REGS) do {	\
	(REGS)->cx = UNION.g1.addr1;				\
	(REGS)->ax = UNION.g1.addr2;				\
	(REGS)->ip = UNION.g1.addr2;				\
} while (0)

#define is_valid_gcc_trampoline_x86_32_type2(UNION, REGS)	\
	(UNION.g2.mov == 0xB9 &&				\
	UNION.g2.jmp == 0xE9 &&					\
	REGS->ip > REGS->sp)

#define emulate_gcc_trampoline_x86_32_type2(UNION, REGS) do {	\
	(REGS)->cx = UNION.g2.addr1;				\
	(REGS)->ip = (unsigned int) ((REGS)->ip +		\
				     UNION.g2.addr2 +		\
				     sizeof(UNION.g2));		\
} while (0)



#ifdef CONFIG_X86_64

struct libffi_trampoline_x86_64 {
	unsigned short mov1;
	unsigned long addr1;
	unsigned short mov2;
	unsigned long addr2;
	unsigned char stcclc;
	unsigned short jmp1;
	unsigned char jmp2;
} __packed;

struct gcc_trampoline_x86_64_type1 {
	unsigned short mov1;
	unsigned long addr1;
	unsigned short mov2;
	unsigned long addr2;
	unsigned short jmp1;
	unsigned char jmp2;
} __packed;

struct gcc_trampoline_x86_64_type2 {
	unsigned short mov1;
	unsigned int addr1;
	unsigned short mov2;
	unsigned long addr2;
	unsigned short jmp1;
	unsigned char jmp2;
} __packed;

union trampolines_x86_64 {
	struct libffi_trampoline_x86_64 lf;
	struct gcc_trampoline_x86_64_type1 g1;
	struct gcc_trampoline_x86_64_type2 g2;
};

#define is_valid_libffi_trampoline_x86_64(UNION)	\
	(UNION.lf.mov1 == 0xBB49 &&			\
	UNION.lf.mov2 == 0xBA49 &&			\
	(UNION.lf.stcclc == 0xF8 ||			\
	 UNION.lf.stcclc == 0xF9) &&			\
	UNION.lf.jmp1 == 0xFF49 &&			\
	UNION.lf.jmp2 == 0xE3)

#define emulate_libffi_trampoline_x86_64(UNION, REGS) do {	\
	(REGS)->r11 = UNION.lf.addr1;				\
	(REGS)->r10 = UNION.lf.addr2;				\
	(REGS)->ip = UNION.lf.addr1;				\
	if (UNION.lf.stcclc == 0xF8)				\
		(REGS)->flags &= ~X86_EFLAGS_CF;		\
	else							\
		(REGS)->flags |= X86_EFLAGS_CF;			\
} while (0)

#define is_valid_gcc_trampoline_x86_64_type1(UNION, REGS)	\
	(UNION.g1.mov1 == 0xBB49 &&				\
	UNION.g1.mov2 == 0xBA49 &&				\
	UNION.g1.jmp1 == 0xFF49 &&				\
	UNION.g1.jmp2 == 0xE3 &&				\
	REGS->ip > REGS->sp)

#define emulate_gcc_trampoline_x86_64_type1(UNION, REGS) do {	\
	(REGS)->r11 = UNION.g1.addr1;				\
	(REGS)->r10 = UNION.g1.addr2;				\
	(REGS)->ip = UNION.g1.addr1;				\
} while (0)

#define is_valid_gcc_trampoline_x86_64_type2(UNION, REGS)	\
	(UNION.g2.mov1 == 0xBB41 &&				\
	UNION.g2.mov2 == 0xBA49 &&				\
	UNION.g2.jmp1 == 0xFF49 &&				\
	UNION.g2.jmp2 == 0xE3 &&				\
	REGS->ip > REGS->sp)

#define emulate_gcc_trampoline_x86_64_type2(UNION, REGS) do {	\
	(REGS)->r11 = UNION.g2.addr1;				\
	(REGS)->r10 = UNION.g2.addr2;				\
	(REGS)->ip = UNION.g2.addr1;				\
} while (0)

#endif /* CONFIG_X86_64 */

#endif /* CONFIG_SECURITY_SARA_WXPROT_EMUTRAMP */
#endif /* __SARA_TRAMPOLINES_H */
