/*
 *  TW5864 driver - Exp-Golomb code functions
 *
 *  Copyright (C) 2015 Bluecherry, LLC <maintainers@bluecherrydvr.com>
 *  Copyright (C) 2015 Andrey Utkin <andrey.utkin@corp.bluecherry.net>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 */

struct bs {
	u8 *p_start;
	u8 *p;
	u8 *p_end;
	int i_left; /* number of available bits */
};

static inline void bs_init(struct bs *s, void *p_data, int i_data)
{
	s->p_start = p_data;
	s->p = p_data;
	s->p_end = s->p + i_data;
	s->i_left = 8;
}

static inline int bs_pos(struct bs *s)
{
	return (8 * (s->p - s->p_start) + 8 - s->i_left);
}

static inline int bs_eof(struct bs *s)
{
	return (s->p >= s->p_end ? 1 : 0);
}

static inline int bs_len(struct bs *s)
{
	return (s->p - s->p_start);
}

static inline int bs_left(struct bs *s)
{
	return (8 - s->i_left);
}

static inline void bs_direct_write(struct bs *s, u8 value)
{
	*s->p = value;
	s->p++;
	s->i_left = 8;
}

static inline void bs_write(struct bs *s, int i_count, u32 i_bits)
{
	if (s->p >= s->p_end - 4)
		return;
	while (i_count > 0) {
		if (i_count < 32)
			i_bits &= (1 << i_count) - 1;
		if (i_count < s->i_left) {
			*s->p = (*s->p << i_count) | i_bits;
			s->i_left -= i_count;
			break;
		}
		*s->p = (*s->p << s->i_left) | (i_bits >> (i_count -
							   s->i_left));
		i_count -= s->i_left;
		s->p++;
		s->i_left = 8;
	}
}

static inline void bs_write1(struct bs *s, u32 i_bit)
{
	if (s->p < s->p_end) {
		*s->p <<= 1;
		*s->p |= i_bit;
		s->i_left--;
		if (s->i_left == 0) {
			s->p++;
			s->i_left = 8;
		}
	}
}

static inline void bs_align_0(struct bs *s)
{
	if (s->i_left != 8) {
		*s->p <<= s->i_left;
		s->i_left = 8;
		s->p++;
	}
}

static inline void bs_sh_align(struct bs *s)
{
	if (s->i_left != 8) {
		*s->p <<= s->i_left;
		s->i_left = 8;
	}
}

static inline void bs_align_1(struct bs *s)
{
	if (s->i_left != 8) {
		*s->p <<= s->i_left;
		*s->p |= (1 << s->i_left) - 1;
		s->i_left = 8;
		s->p++;
	}
}

static inline void bs_align(struct bs *s)
{
	bs_align_0(s);
}

/* golomb functions */
static inline void bs_write_ue(struct bs *s, u32 val)
{
	int i_size = 0;
	static const int i_size0_255[256] = {
		1, 1, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 4, 4, 4, 4, 5, 5, 5,
		5, 5,
		5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5,
		6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6,
		6, 6,
		6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6,
		7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
		7, 7,
		7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
		7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
		7, 7,
		7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
		8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
		8, 8,
		8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
		8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
		8, 8,
		8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
		8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
		8, 8,
		8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
		8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
		8, 8,
		8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8
	};

	if (val == 0) {
		bs_write1(s, 1);
	} else {
		unsigned int tmp = ++val;

		if (tmp >= 0x00010000) {
			i_size += 16;
			tmp >>= 16;
		}
		if (tmp >= 0x100) {
			i_size += 8;
			tmp >>= 8;
		}
		i_size += i_size0_255[tmp];

		bs_write(s, 2 * i_size - 1, val);
	}
}

static inline void bs_write_se(struct bs *s, int val)
{
	bs_write_ue(s, val <= 0 ? -val * 2 : val * 2 - 1);
}

static inline void bs_write_te(struct bs *s, int x, int val)
{
	if (x == 1)
		bs_write1(s, 1 & ~val);
	else if (x > 1)
		bs_write_ue(s, val);
}

static inline void bs_rbsp_trailing(struct bs *s)
{
	bs_write1(s, 1);
	if (s->i_left != 8)
		bs_write(s, s->i_left, 0x00);
}

static inline int bs_size_ue(unsigned int val)
{
	int i_size = 0;
	static const int i_size0_254[255] = {
		1, 3, 3, 5, 5, 5, 5, 7, 7, 7, 7, 7, 7, 7, 7,
		9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9,
		11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11,
		11, 11,
		11, 11, 11, 11, 11, 11, 11,
		11, 11, 11, 11, 11, 11, 11, 11, 11, 13, 13, 13, 13, 13,
		13, 13,
		13, 13, 13, 13, 13, 13, 13,
		13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13,
		13, 13,
		13, 13, 13, 13, 13, 13, 13,
		13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13,
		13, 13,
		13, 13, 13, 13, 13, 13, 13,
		13, 13, 13, 13, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15,
		15, 15,
		15, 15, 15, 15, 15, 15, 15,
		15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15,
		15, 15,
		15, 15, 15, 15, 15, 15, 15,
		15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15,
		15, 15,
		15, 15, 15, 15, 15, 15, 15,
		15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15,
		15, 15,
		15, 15, 15, 15, 15, 15, 15,
		15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15,
		15, 15,
		15, 15, 15, 15, 15, 15, 15,
		15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15,
		15, 15,
		15
	};

	if (val < 255)
		return i_size0_254[val];

	val++;

	if (val >= 0x10000) {
		i_size += 32;
		val = (val >> 16) - 1;
	}
	if (val >= 0x100) {
		i_size += 16;
		val = (val >> 8) - 1;
	}
	return i_size0_254[val] + i_size;
}

static inline int bs_size_se(int val)
{
	return bs_size_ue(val <= 0 ? -val * 2 : val * 2 - 1);
}

static inline int bs_size_te(int x, int val)
{
	if (x == 1)
		return 1;
	if (x > 1)
		return bs_size_ue(val);
	return 0;
}
