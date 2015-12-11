/*
 * unicode.c
 *
 * PURPOSE
 *	Routines for converting between UTF-8 and OSTA Compressed Unicode.
 *      Also handles filename mangling
 *
 * DESCRIPTION
 *	OSTA Compressed Unicode is explained in the OSTA UDF specification.
 *		http://www.osta.org/
 *	UTF-8 is explained in the IETF RFC XXXX.
 *		ftp://ftp.internic.net/rfc/rfcxxxx.txt
 *
 * COPYRIGHT
 *	This file is distributed under the terms of the GNU General Public
 *	License (GPL). Copies of the GPL can be obtained from:
 *		ftp://prep.ai.mit.edu/pub/gnu/GPL
 *	Each contributing author retains all rights to their own work.
 */

#include "udfdecl.h"

#include <linux/kernel.h>
#include <linux/string.h>	/* for memset */
#include <linux/nls.h>
#include <linux/crc-itu-t.h>
#include <linux/slab.h>

#include "udf_sb.h"

static int udf_uni2char_utf8(wchar_t uni,
			     unsigned char *out,
			     int boundlen)
{
	int len = 0;

	if (boundlen <= 0)
		return -ENAMETOOLONG;

	if (uni < 0x80)
		out[len++] = (unsigned char)uni;
	else if (uni < 0x800) {
		if (boundlen < 2)
			return -ENAMETOOLONG;
		out[len++] = (unsigned char)(0xc0 | (uni >> 6));
		out[len++] = (unsigned char)(0x80 | (uni & 0x3f));
	} else {
		if (boundlen < 3)
			return -ENAMETOOLONG;
		out[len++] = (unsigned char)(0xe0 | (uni >> 12));
		out[len++] = (unsigned char)(0x80 | ((uni >> 6) & 0x3f));
		out[len++] = (unsigned char)(0x80 | (uni & 0x3f));
	}
	return len;
}

static int udf_char2uni_utf8(const unsigned char *in,
			     int boundlen,
			     wchar_t *uni)
{
	unsigned int utf_char;
	unsigned char c;
	int len, utf_cnt;

	utf_char = 0;
	utf_cnt = 0;
	for (len = 0; len < boundlen;) {

		c = in[len++];

		/* Complete a multi-byte UTF-8 character */
		if (utf_cnt) {
			utf_char = (utf_char << 6) | (c & 0x3f);
			if (--utf_cnt)
				continue;
		} else {
			/* Check for a multi-byte UTF-8 character */
			if (c & 0x80) {
				/* Start a multi-byte UTF-8 character */
				if ((c & 0xe0) == 0xc0) {
					utf_char = c & 0x1f;
					utf_cnt = 1;
				} else if ((c & 0xf0) == 0xe0) {
					utf_char = c & 0x0f;
					utf_cnt = 2;
				} else if ((c & 0xf8) == 0xf0) {
					utf_char = c & 0x07;
					utf_cnt = 3;
				} else if ((c & 0xfc) == 0xf8) {
					utf_char = c & 0x03;
					utf_cnt = 4;
				} else if ((c & 0xfe) == 0xfc) {
					utf_char = c & 0x01;
					utf_cnt = 5;
				} else {
					utf_cnt = -1;
					break;
				}
				continue;
			} else {
				/* Single byte UTF-8 character (most common) */
				utf_char = c;
			}
		}
		*uni = utf_char;
		break;
	}
	if (utf_cnt) {
		*uni = '?';
		return -EINVAL;
	}
	return len;
}

#define ILLEGAL_CHAR_MARK	'_'
#define EXT_MARK		'.'
#define CRC_MARK		'#'
#define EXT_SIZE		5
/* Number of chars we need to store generated CRC to make filename unique */
#define CRC_LEN			5

static int udf_name_from_CS0(uint8_t *str_o, int str_max_len,
			     const uint8_t *ocu_i, int ocu_len,
			     int (*conv_f)(wchar_t, unsigned char *, int),
			     int translate)
{
	const uint8_t *ocu;
	uint32_t c;
	uint8_t cmp_id, mb;
	int i, ic, is, len, ocu_len_r;
	int firstDots = 0, needsCRC = 0, illChar = 0;
	uint8_t ext[EXT_SIZE * NLS_MAX_CHARSET_SIZE + 1];
	uint8_t crc[CRC_LEN];
	int ext_len, ext_max_len = sizeof(ext);
	int str_o_len = 0;	/* Length of resulting output */
	int ext_o_len = 0;	/* Length of extension in output buffer */
	int i_ext = -1;	/* Extension position in input buffer */
	int o_crc = 0;	/* Rightmost possible output position for CRC+ext */
	int ext_crc_len = 0;	/* Ext output length if used with CRC */
	unsigned short valueCRC;

	if (str_max_len <= 0)
		return 0;

	if (ocu_len == 0) {
		memset(str_o, 0, str_max_len);
		return 0;
	}

	cmp_id = ocu_i[0];
	if (cmp_id != 8 && cmp_id != 16) {
		memset(str_o, 0, str_max_len);
		pr_err("unknown compression code (%d)\n", cmp_id);
		return -EINVAL;
	}
	mb = cmp_id >> 4;

	ocu = &ocu_i[1];
	ocu_len--;
	ocu_len_r = ocu_len & ((-1) << mb);

	if (translate) {

		/* Look for extension */
		for (i = ocu_len_r - mb - 1, ext_len = 0;
		     (i >= 0) && (ext_len < EXT_SIZE);
		     i -= (mb + 1), ext_len++) {

			c = ocu[i];
			if (mb)
				c = (c << 8) | ocu[i+1];

			if (c == EXT_MARK) {
				if (ext_len)
					i_ext = i;
				break;
			}
		}
		if (i_ext >= 0) {
			/* Convert extension */
			if (ext_max_len > str_max_len)
				ext_max_len = str_max_len;

			ext[ext_o_len++] = EXT_MARK;
			for (i = i_ext + mb + 1; i < ocu_len_r;) {

				c = ocu[i++];
				if (mb)
					c = (c << 8) | ocu[i++];

				if (c == '/' || c == 0) {
					if (illChar)
						continue;
					illChar = 1;
					needsCRC = 1;
					c = ILLEGAL_CHAR_MARK;
				} else
					illChar = 0;

				len = conv_f(c, &ext[ext_o_len],
					     ext_max_len - ext_o_len);
				/* Valid character? */
				if (len >= 0)
					ext_o_len += len;
				else {
					ext[ext_o_len++] = '?';
					needsCRC = 1;
				}
				if ((ext_o_len + CRC_LEN) < str_max_len)
					ext_crc_len = ext_o_len;
			}
		}
	}

	for (i = 0, ic = 0; i < ocu_len_r; ic++) {

		is = i;

		/* Expand OSTA compressed Unicode to Unicode */
		c = ocu[i++];
		if (mb)
			c = (c << 8) | ocu[i++];

		if (translate) {
			if (is == i_ext) {
				if (str_o_len > (str_max_len - ext_o_len))
					needsCRC = 1;
				break;
			}
			if ((c == '.') && (ic == 0))
				firstDots = 1;
			if ((c != '.') || (ic > 1))
				firstDots = 0;

			if (c == '/' || c == 0) {
				if (illChar)
					continue;
				illChar = 1;
				needsCRC = 1;
				c = ILLEGAL_CHAR_MARK;
			} else
				illChar = 0;
		}

		if (str_o_len < str_max_len) {
			/* Compress Unicode to UTF-8 or NLS */
			len = conv_f(c, &str_o[str_o_len],
				     str_max_len - str_o_len);
			/* Valid character? */
			if (len >= 0)
				str_o_len += len;
			else {
				str_o[str_o_len++] = '?';
				needsCRC = 1;
			}
			if (str_o_len <= (str_max_len - ext_o_len - CRC_LEN))
				o_crc = str_o_len;
		} else
			needsCRC = 1;
	}

	if (translate) {
		if (firstDots || needsCRC) {
			str_o_len = o_crc;
			if (str_o_len < str_max_len) {
				valueCRC = crc_itu_t(0, ocu, ocu_len);
				crc[0] = CRC_MARK;
				crc[1] = hex_asc_upper_hi(valueCRC >> 8);
				crc[2] = hex_asc_upper_lo(valueCRC >> 8);
				crc[3] = hex_asc_upper_hi(valueCRC);
				crc[4] = hex_asc_upper_lo(valueCRC);
				len = CRC_LEN;
				if (len > (str_max_len - str_o_len))
					len = str_max_len - str_o_len;
				memcpy(&str_o[str_o_len], crc, len);
				str_o_len += len;
			}
			ext_o_len = ext_crc_len;
		}
		if (ext_o_len > 0) {
			memcpy(&str_o[str_o_len], ext, ext_o_len);
			str_o_len += ext_o_len;
		}
	}

	return str_o_len;
}

static int udf_name_to_CS0(uint8_t *ocu_o, int ocu_max_len,
			   const uint8_t *str_i, int str_len,
			   int (*conv_f)(const unsigned char *, int, wchar_t *))
{
	int i, len;
	unsigned int max_val;
	wchar_t uni_char;
	int ocu_len, ocu_ch;

	memset(ocu_o, 0, sizeof(uint8_t) * ocu_max_len);
	ocu_o[0] = 8;
	max_val = 0xff;
	ocu_ch = 1;

try_again:
	ocu_len = 1;
	for (i = 0; (i < str_len) && ((ocu_len + ocu_ch) <= ocu_max_len); i++) {
		len = conv_f(&str_i[i], str_len, &uni_char);
		if (!len)
			continue;
		/* Invalid character, deal with it */
		if (len < 0) {
			len = 1;
			uni_char = '?';
		}

		if (uni_char > max_val) {
			ocu_o[0] = 0x10;
			max_val = 0xffff;
			ocu_ch = 2;
			goto try_again;
		}

		if (max_val == 0xffff)
			ocu_o[ocu_len++] = (uint8_t)(uni_char >> 8);
		ocu_o[ocu_len++] = (uint8_t)(uni_char & 0xff);
		i += len - 1;
	}

	return ocu_len;
}

int udf_CS0toUTF8(uint8_t *outstr, int outlen, const uint8_t *instr, int inlen)
{
	return udf_name_from_CS0(outstr, outlen, instr, inlen,
				 udf_uni2char_utf8, 0);
}

int udf_get_filename(struct super_block *sb, const uint8_t *sname, int slen,
		     uint8_t *dname, int dlen)
{
	int (*conv_f)(wchar_t, unsigned char *, int);
	int ret;

	if (!slen)
		return -EIO;

	if (UDF_QUERY_FLAG(sb, UDF_FLAG_UTF8)) {
		conv_f = udf_uni2char_utf8;
	} else if (UDF_QUERY_FLAG(sb, UDF_FLAG_NLS_MAP)) {
		conv_f = UDF_SB(sb)->s_nls_map->uni2char;
	} else
		BUG();

	ret = udf_name_from_CS0(dname, dlen, sname, slen, conv_f, 1);
	/* Zero length filename isn't valid... */
	if (ret == 0)
		ret = -EINVAL;
	return ret;
}

int udf_put_filename(struct super_block *sb, const uint8_t *sname, int slen,
		     uint8_t *dname, int dlen)
{
	int (*conv_f)(const unsigned char *, int, wchar_t *);

	if (UDF_QUERY_FLAG(sb, UDF_FLAG_UTF8)) {
		conv_f = udf_char2uni_utf8;
	} else if (UDF_QUERY_FLAG(sb, UDF_FLAG_NLS_MAP)) {
		conv_f = UDF_SB(sb)->s_nls_map->char2uni;
	} else
		BUG();

	return udf_name_to_CS0(dname, dlen, sname, slen, conv_f);
}

