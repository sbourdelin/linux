/*
 * Copyright (C) 2015 Imagination Technologies
 * Author: Paul Burton <paul.burton@imgtec.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef __ASM_MACH_BOSTON_SPACES_H__
#define __ASM_MACH_BOSTON_SPACES_H__

#ifdef CONFIG_64BIT
# define CAC_BASE _AC(0xa800000000000000, UL)
#endif

#include <asm/mach-generic/spaces.h>

#endif /* __ASM_MACH_BOSTON_SPACES_H__ */
