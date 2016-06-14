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
#include <linux/log2.h>

#define _bf_shf(x) (__builtin_ffsll(x) - 1)

#define _BF_FIELD_CHECK(_mask, _val)					\
	({								\
		const u64 hi = (_mask) + (1ULL << _bf_shf(_mask));	\
									\
		BUILD_BUG_ON(!(_mask) || (hi && !is_power_of_2_u64(hi))); \
		BUILD_BUG_ON(__builtin_constant_p(_val) ?		\
			     ~((_mask) >> _bf_shf(_mask)) & (_val) :	\
			     0);					\
	})

#define FIELD_PUT(_mask, _val)					\
	({							\
		_BF_FIELD_CHECK(_mask, _val);			\
		((u32)(_val) << _bf_shf(_mask)) & (_mask);	\
	})

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
