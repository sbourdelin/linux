/*
 * Internal declarations for x86 TLS implementation functions.
 *
 * Copyright (C) 2007 Red Hat, Inc.  All rights reserved.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU General Public License v.2.
 *
 * Red Hat Author: Roland McGrath.
 */

#ifndef _ARCH_X86_KERNEL_TLS_H

#include <linux/regset.h>

#ifdef CONFIG_X86_TLS_AREA
extern user_regset_active_fn regset_tls_active;
extern user_regset_set_fn regset_gdt_set;
#else
#define regset_tls_active NULL
#define regset_gdt_set NULL
#endif

#endif	/* _ARCH_X86_KERNEL_TLS_H */
