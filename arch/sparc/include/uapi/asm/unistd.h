/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * System calls under the Sparc.
 *
 * Don't be scared by the ugly clobbers, it is the only way I can
 * think of right now to force the arguments into fixed registers
 * before the trap into the system call with gcc 'asm' statements.
 *
 * Copyright (C) 1995, 2007 David S. Miller (davem@davemloft.net)
 *
 * SunOS compatibility based upon preliminary work which is:
 *
 * Copyright (C) 1995 Adrian M. Rodriguez (adrian@remus.rutgers.edu)
 */
#ifndef _UAPI_SPARC_UNISTD_H
#define _UAPI_SPARC_UNISTD_H

#ifdef CONFIG_64
#include <asm/unistd_64.h>
#else
#include <asm/unistd_32.h>
#endif

#define __NR_syscalls 361

#endif /* _UAPI_SPARC_UNISTD_H */
