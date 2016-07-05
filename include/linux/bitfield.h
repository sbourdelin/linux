/*
 * Copyright (C) 2014 Felix Fietkau <nbd@nbd.name>
 * Copyright (C) 2004 - 2009 Ivo van Doorn <IvDoorn@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2
 * as published by the Free Software Foundation
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#ifndef _LINUX_BITFIELD_H
#define _LINUX_BITFIELD_H

#include <asm/types.h>
#include <linux/bug.h>

#define _bf_shf(x) (__builtin_ffsll(x) - 1)

#define _BF_FIELD_CHECK(_mask, _val)					\
	({								\
		BUILD_BUG_ON(!(_mask));					\
		BUILD_BUG_ON(__builtin_constant_p(_val) ?		\
			     ~((_mask) >> _bf_shf(_mask)) & (_val) :	\
			     0);					\
		__BUILD_BUG_ON_NOT_POWER_OF_2((_mask) +			\
					      (1ULL << _bf_shf(_mask))); \
	})

/*
 * Bitfield access macros
 *
 * This file contains macros which take as input shifted mask
 * from which they extract the base mask and shift amount at
 * compilation time.  There are two separate sets of the macros
 * one for 32bit registers and one for 64bit ones.
 *
 * Fields can be defined using GENMASK (which is usually
 * less error-prone and easier to match with datasheets).
 *
 * FIELD_{GET,PUT} macros are designed to be used with masks which
 * are compilation time constants.
 *
 * Example:
 *
 *  #define REG_FIELD_A  GENMASK(6, 0)
 *  #define REG_FIELD_B  BIT(7)
 *  #define REG_FIELD_C  GENMASK(15, 8)
 *  #define REG_FIELD_D  GENMASK(31, 16)
 *
 * Get:
 *  a = FIELD_GET(REG_FIELD_A, reg);
 *  b = FIELD_GET(REG_FIELD_B, reg);
 *
 * Set:
 *  reg = FIELD_PUT(REG_FIELD_A, 1) |
 *	  FIELD_PUT(REG_FIELD_B, 0) |
 *	  FIELD_PUT(REG_FIELD_C, c) |
 *	  FIELD_PUT(REG_FIELD_D, 0x40);
 *
 * Modify:
 *  reg &= ~REG_FIELD_C;
 *  reg |= FIELD_PUT(REG_FIELD_C, c);
 */

/**
 * FIELD_PUT() - construct a bitfield element
 * @_mask: shifted mask defining the field's length and position
 * @_val:  value to put in the field
 *
 * FIELD_PUT() masks and shifts up the value.  The result should
 * be combined with other fields of the bitfield using logical OR.
 */
#define FIELD_PUT(_mask, _val)					\
	({							\
		_BF_FIELD_CHECK(_mask, _val);			\
		((u32)(_val) << _bf_shf(_mask)) & (_mask);	\
	})

/**
 * FIELD_GET() - extract a bitfield element
 * @_mask: shifted mask defining the field's length and position
 * @_val:  32bit value of entire bitfield
 *
 * FIELD_GET() extracts the field specified by @_mask from the
 * bitfield passed in as @_val.
 */
#define FIELD_GET(_mask, _val)					\
	({							\
		_BF_FIELD_CHECK(_mask, 0);			\
		(u32)(((_val) & (_mask)) >> _bf_shf(_mask));	\
	})

#define FIELD_PUT64(_mask, _val)				\
	({							\
		_BF_FIELD_CHECK(_mask, _val);			\
		((u64)(_val) << _bf_shf(_mask)) & (_mask);	\
	})

#define FIELD_GET64(_mask, _val)				\
	({							\
		_BF_FIELD_CHECK(_mask, 0);			\
		(u64)(((_val) & (_mask)) >> _bf_shf(_mask));	\
	})

#endif
