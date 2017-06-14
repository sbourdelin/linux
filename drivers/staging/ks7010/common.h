/*
 * Driver for KeyStream wireless LAN cards.
 *
 * Copyright (C) 2005-2008 KeyStream Corp.
 * Copyright (C) 2009 Renesas Technology Corp.
 * Copyright (C) 2017 Tobin C. Harding.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */

#ifndef _KS7010_COMMON_H
#define _KS7010_COMMON_H

struct ks7010;

/* FIXME does the kernel already define these? */
#define MAX_U16_VAL 0xFFFF
#define MAX_U8_VAL 0xFF

#define IE_MAX_SIZE 128

#define KS7010_DEFAULT_BEACON_LOST_COUNT 20
#define KS7010_DEFAULT_RTS_THRESHOLD 2347UL
#define KS7010_DEFAULT_FRAG_THRESHOLD 2346UL

#endif	/* _KS7010_COMMON_H */
