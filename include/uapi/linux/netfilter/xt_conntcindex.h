#ifndef _XT_CONNTCINDEX_H
#define _XT_CONNTCINDEX_H

#include <linux/types.h>

/* Copyright (C) 2015 Allied Telesis Labs NZ
 * by Luuk Paulussen <luuk.paulussen@alliedtelesis.co.nz>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

enum {
	XT_CONNTCINDEX_SET = 0,
	XT_CONNTCINDEX_SAVE,
	XT_CONNTCINDEX_RESTORE
};

struct xt_conntcindex_tginfo1 {
	__u16 ctmark, ctmask, nfmask;
	__u8 mode;
};

struct xt_conntcindex_mtinfo1 {
	__u16 mark, mask;
	__u8 invert;
};

#endif /*_XT_CONNTCINDEX_H*/
