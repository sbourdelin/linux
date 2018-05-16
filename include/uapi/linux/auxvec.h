/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_LINUX_AUXVEC_H
#define _UAPI_LINUX_AUXVEC_H

#include <asm/auxvec.h>

/*
 * Symbolic values for the entries in the auxiliary table
 * put on the initial stack.
 *
 * Values with definitions here are common to all architectures.
 * Individual architectures may define additional values in their
 * <uapi/asm/auxvec.h>.
 *
 * NOTE: Userspace may treat these numbers as a global namespace.
 * Any per-architecture definition must not overlap with these or with
 * any other architecture's definitions, unless it has identical name
 * and number and compatible meaning.
 */
#define AT_NULL   0	/* end of vector */
#define AT_IGNORE 1	/* entry should be ignored */
#define AT_EXECFD 2	/* file descriptor of program */
#define AT_PHDR   3	/* program headers for program */
#define AT_PHENT  4	/* size of program header entry */
#define AT_PHNUM  5	/* number of program headers */
#define AT_PAGESZ 6	/* system page size */
#define AT_BASE   7	/* base address of interpreter */
#define AT_FLAGS  8	/* flags */
#define AT_ENTRY  9	/* entry point of program */
#define AT_NOTELF 10	/* program is not ELF */
#define AT_UID    11	/* real uid */
#define AT_EUID   12	/* effective uid */
#define AT_GID    13	/* real gid */
#define AT_EGID   14	/* effective gid */
#define AT_PLATFORM 15  /* string identifying CPU for optimizations */
#define AT_HWCAP  16    /* arch dependent hints at CPU capabilities */
#define AT_CLKTCK 17	/* frequency at which times() increments */
/* 18 reserved for AT_FPUCW		(sh) */
/* 19 reserved for AT_DCACHEBSIZE	(powerpc) */
/* 20 reserved for AT_ICACHEBSIZE	(powerpc) */
/* 21 reserved for AT_UCACHEBSIZE	(powerpc) */
/* 22 reserved for AT_IGNOREPPC		(powerpc) */
#define AT_SECURE 23   /* secure mode boolean */
#define AT_BASE_PLATFORM 24	/* string identifying real platform, may
				 * differ from AT_PLATFORM. */
#define AT_RANDOM 25	/* address of 16 random bytes */
#define AT_HWCAP2 26	/* extension of AT_HWCAP */

#define AT_EXECFN  31	/* filename of program */

/* 32 reserved for AT_SYSINFO		(alpha ia64 um x86) */
/* 33 reserved for AT_SYSINFO_EHDR	(various architectures) */
/* 34 reserved for AT_L1I_CACHESHAPE	(alpha sh) */
/* 35 reserved for AT_L1D_CACHESHAPE	(alpha sh) */
/* 36 reserved for AT_L2_CACHESHAPE	(alpha sh) */
/* 37 reserved for AT_L3_CACHESHAPE	(alpha) */
/* 38 reserved, do not allocate */
/* 39 reserved, do not allocate */
/* 40 reserved for AT_L1I_CACHESIZE	(powerpc) */
/* 41 reserved for AT_L1I_CACHEGEOMETRY	(powerpc) */
/* 42 reserved for AT_L1D_CACHESIZE	(powerpc) */
/* 43 reserved for AT_L1D_CACHEGEOMETRY	(powerpc) */
/* 44 reserved for AT_L2_CACHESIZE	(powerpc) */
/* 45 reserved for AT_L2_CACHEGEOMETRY	(powerpc) */
/* 46 reserved for AT_L3_CACHESIZE	(powerpc) */
/* 47 reserved for AT_L3_CACHEGEOMETRY	(powerpc) */
/* 48 reserved for AT_ADI_BLKSZ		(sparc) */
/* 49 reserved for AT_ADI_NBITS		(sparc) */
/* 50 reserved for AT_ADI_UEONADI	(sparc) */

#endif /* _UAPI_LINUX_AUXVEC_H */
