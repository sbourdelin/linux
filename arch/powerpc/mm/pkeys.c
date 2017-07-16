/*
 * PowerPC Memory Protection Keys management
 * Copyright (c) 2015, Intel Corporation.
 * Copyright (c) 2017, IBM Corporation.
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
#include <uapi/asm-generic/mman-common.h>
#include <linux/pkeys.h>                /* PKEY_*                       */

bool pkey_inited;
