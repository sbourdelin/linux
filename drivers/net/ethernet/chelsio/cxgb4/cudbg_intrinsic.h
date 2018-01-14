/*
 *  Copyright (C) 2018 Chelsio Communications.  All rights reserved.
 *
 *  This program is free software; you can redistribute it and/or modify it
 *  under the terms and conditions of the GNU General Public License,
 *  version 2, as published by the Free Software Foundation.
 *
 *  This program is distributed in the hope it will be useful, but WITHOUT
 *  ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 *  FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 *  more details.
 *
 *  The full GNU General Public License is included in this distribution in
 *  the file called "COPYING".
 *
 */

#ifndef __CUDBG_INTRINSIC_H__
#define __CUDBG_INTRINSIC_H__

unsigned int cudbg_mem_read_def(struct cudbg_init *pdbg_init,
				u32 start, u32 offset, u32 size,
				u32 mem_aperture, u8 *outbuf);
void cudbg_set_intrinsic_callback(struct cudbg_init *pdbg_init);
#endif /* __CUDBG_INTRINSIC_H__ */
