/*
 * vpd_decode.c
 *
 * Google VPD decoding routines.
 *
 * Copyright 2017 Google Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License v2.0 as published by
 * the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/export.h>

#include "vpd_decode.h"

static int decode_len(const int32_t max_len, const uint8_t *in,
		      int32_t *length, int32_t *decoded_len)
{
	uint8_t more;
	int i = 0;

	if (!length || !decoded_len)
		return VPD_FAIL;

	*length = 0;
	do {
		if (i >= max_len)
			return VPD_FAIL;

		more = in[i] & 0x80;
		*length <<= 7;
		*length |= in[i] & 0x7f;
		++i;
	} while (more);

	*decoded_len = i;

	return VPD_OK;
}

int decode_vpd_string(const int32_t max_len, const uint8_t *input_buf,
		      int32_t *consumed, vpd_decode_callback callback,
		      void *callback_arg)
{
	int type;
	int res;
	int32_t key_len, value_len;
	int32_t decoded_len;
	const uint8_t *key, *value;

	/* type */
	if (*consumed >= max_len)
		return VPD_FAIL;

	type = input_buf[*consumed];

	switch (type) {
	case VPD_TYPE_INFO:
	case VPD_TYPE_STRING:
		(*consumed)++;

		/* key */
		res = decode_len(max_len - *consumed, &input_buf[*consumed],
				 &key_len, &decoded_len);
		if (res != VPD_OK || *consumed + decoded_len >= max_len)
			return VPD_FAIL;

		*consumed += decoded_len;
		key = &input_buf[*consumed];
		*consumed += key_len;

		/* value */
		res = decode_len(max_len - *consumed, &input_buf[*consumed],
				 &value_len, &decoded_len);
		if (res != VPD_OK || *consumed + decoded_len > max_len)
			return VPD_FAIL;

		*consumed += decoded_len;
		value = &input_buf[*consumed];
		*consumed += value_len;

		if (type == VPD_TYPE_STRING)
			return callback(key, key_len, value, value_len,
					callback_arg);
		break;

	default:
		return VPD_FAIL;
	}

	return VPD_OK;
}
EXPORT_SYMBOL(decode_vpd_string);
