/*
 * This file defines the fixed addresses where userspace programs
 * can find atomic code sequences.
 *
 * Copyright 2007-2008 Analog Devices Inc.
 *
 * Licensed under the GPL-2 or later.
 */

#ifndef _UAPI__BFIN_ASM_FIXED_CODE_H__
#define _UAPI__BFIN_ASM_FIXED_CODE_H__

#ifndef PHY_RAM_BASE_ADDRESS
#ifdef __KERNEL__
#error "Don't include <uapi/asm/fixed_code.h>, include <asm/fixed_code.h>"
#else
#define PHY_RAM_BASE_ADDRESS	0x0
#endif
#endif

#define FIXED_CODE_START	(PHY_RAM_BASE_ADDRESS + 0x400)

#define SIGRETURN_STUB		(PHY_RAM_BASE_ADDRESS + 0x400)

#define ATOMIC_SEQS_START	(PHY_RAM_BASE_ADDRESS + 0x410)

#define ATOMIC_XCHG32		(PHY_RAM_BASE_ADDRESS + 0x410)
#define ATOMIC_CAS32		(PHY_RAM_BASE_ADDRESS + 0x420)
#define ATOMIC_ADD32		(PHY_RAM_BASE_ADDRESS + 0x430)
#define ATOMIC_SUB32		(PHY_RAM_BASE_ADDRESS + 0x440)
#define ATOMIC_IOR32		(PHY_RAM_BASE_ADDRESS + 0x450)
#define ATOMIC_AND32		(PHY_RAM_BASE_ADDRESS + 0x460)
#define ATOMIC_XOR32		(PHY_RAM_BASE_ADDRESS + 0x470)

#define ATOMIC_SEQS_END		(PHY_RAM_BASE_ADDRESS + 0x480)

#define SAFE_USER_INSTRUCTION   (PHY_RAM_BASE_ADDRESS + 0x480)

#define FIXED_CODE_END		(PHY_RAM_BASE_ADDRESS + 0x490)

#endif /* _UAPI__BFIN_ASM_FIXED_CODE_H__ */
