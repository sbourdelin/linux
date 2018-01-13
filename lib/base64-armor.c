// SPDX-License-Identifier: GPL-2.0

#include <linux/errno.h>
#include <linux/module.h>
#include <linux/base64-armor.h>

/*
 * base64 encode/decode.
 */

static const char *pem_key =
	"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int encode_bits(int c)
{
	return pem_key[c];
}

static int decode_bits(char c)
{
	if (c >= 'A' && c <= 'Z')
		return c - 'A';
	if (c >= 'a' && c <= 'z')
		return c - 'a' + 26;
	if (c >= '0' && c <= '9')
		return c - '0' + 52;
	if (c == '+')
		return 62;
	if (c == '/')
		return 63;
	if (c == '=')
		return 0; /* just non-negative, please */
	return -EINVAL;
}

int base64_armor(char *dst, int dst_max, const char *src, const char *end)
{
	int olen = 0;
	int line = 0;

	while (src < end) {
		unsigned char a, b, c;

		a = *src++;
		if (dst_max < 4)
			return -ENOSPC;
		*dst++ = encode_bits(a >> 2);
		if (src < end) {
			b = *src++;
			*dst++ = encode_bits(((a & 3) << 4) | (b >> 4));
			if (src < end) {
				c = *src++;
				*dst++ = encode_bits(((b & 15) << 2) |
						     (c >> 6));
				*dst++ = encode_bits(c & 63);
			} else {
				*dst++ = encode_bits((b & 15) << 2);
				*dst++ = '=';
			}
		} else {
			*dst++ = encode_bits(((a & 3) << 4));
			*dst++ = '=';
			*dst++ = '=';
		}
		olen += 4;
		line += 4;
		dst_max -= 4;

		if (line == 64) {
			line = 0;
			if (dst_max < 1)
				return -ENOSPC;
			*(dst++) = '\n';
			olen++;
			dst_max--;
		}
	}
	return olen;
}
EXPORT_SYMBOL(base64_unarmor);

int base64_unarmor(char *dst, int dst_max, const char *src, const char *end)
{
	int olen = 0;

	while (src < end) {
		int a, b, c, d;

		if (src[0] == '\n') {
			src++;
			continue;
		}
		if (src + 4 > end)
			return -EINVAL;
		a = decode_bits(src[0]);
		b = decode_bits(src[1]);
		c = decode_bits(src[2]);
		d = decode_bits(src[3]);
		if (a < 0 || b < 0 || c < 0 || d < 0)
			return -EINVAL;

		if (dst_max < 1)
			return -ENOSPC;
		*dst++ = (a << 2) | (b >> 4);
		dst_max--;
		if (src[2] == '=')
			return olen + 1;
		if (dst_max < 1)
			return -ENOSPC;
		*dst++ = ((b & 15) << 4) | (c >> 2);
		dst_max--;
		if (src[3] == '=')
			return olen + 2;
		if (dst_max < 1)
			return -ENOSPC;
		*dst++ = ((c & 3) << 6) | d;
		dst_max--;
		olen += 3;
		src += 4;
	}
	return olen;
}
EXPORT_SYMBOL(base64_armor);

MODULE_LICENSE("GPL v2");
