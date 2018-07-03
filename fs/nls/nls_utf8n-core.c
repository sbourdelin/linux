/*
 * Copyright (c) 2017 Collabora Ltd.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it would be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/nls.h>
#include <linux/slab.h>
#include <linux/parser.h>
#include <linux/string.h>

#include "utf8n.h"

static struct nls_charset utf8norm_info;

static int utf8_strncmp(const struct nls_table *charset,
			const unsigned char *str1, size_t len1,
			const unsigned char *str2, size_t len2)
{
	const struct utf8data *data = utf8nfkdi(charset->version);
	struct utf8cursor cur1, cur2;
	int c1, c2;
	int r;

	r = utf8ncursor(&cur1, data, str1, len1);
	if (r < 0)
		return -EINVAL;
	r = utf8ncursor(&cur2, data, str2, len2);
	if (r < 0)
		return -EINVAL;

	do {
		c1 = utf8byte(&cur1);
		c2 = utf8byte(&cur2);

		if (c1 < 0 || c2 < 0)
			return -EINVAL;
		if (c1 != c2)
			return 1;
	} while (c1);

	return 0;
}

static int utf8_strncasecmp(const struct nls_table *charset,
			    const unsigned char *str1, size_t len1,
			    const unsigned char *str2, size_t len2)
{
	const struct utf8data *data = utf8nfkdicf(charset->version);
	struct utf8cursor cur1, cur2;
	int c1, c2;
	int r;

	r = utf8ncursor(&cur1, data, str1, len1);
	if (r < 0)
		return -EINVAL;

	r = utf8ncursor(&cur2, data, str2, len2);
	if (r < 0)
		return -EINVAL;

	do {
		c1 = utf8byte(&cur1);
		c2 = utf8byte(&cur2);

		if (c1 < 0 || c2 < 0)
			return -EINVAL;
		if (c1 != c2)
			return 1;
	} while (c1);

	return 0;
}

static int utf8_casefold(const struct nls_table *charset,
			 const unsigned char *str, size_t len,
			 unsigned char *dest, size_t dlen)
{
	const struct utf8data *data = utf8nfkdicf(charset->version);
	struct utf8cursor cur;
	size_t nlen = 0;

	utf8ncursor(&cur, data, str, len);
	for (nlen = 0; nlen < dlen; nlen++) {
		dest[nlen] = utf8byte(&cur);
		if (!dest[nlen])
			return nlen;
	}

	return -EINVAL;
}

static int utf8_normalize(const struct nls_table *charset,
			  const unsigned char *str,
			  size_t len, unsigned char *dest, size_t dlen)
{
	const struct utf8data *data = utf8nfkdi(charset->version);
	struct utf8cursor cur;
	ssize_t nlen = 0;

	utf8ncursor(&cur, data, str, len);

	for (nlen = 0; nlen < dlen; nlen++) {
		dest[nlen] = utf8byte(&cur);
		if (!dest[nlen])
			return nlen;
	}

	return -EINVAL;
}

static int utf8_uni2char(wchar_t uni, unsigned char *out, int boundlen)
{
	int n;

	if (boundlen <= 0)
		return -ENAMETOOLONG;

	n = utf32_to_utf8(uni, out, boundlen);
	if (n < 0) {
		*out = '?';
		return -EINVAL;
	}
	return n;
}

static int utf8_char2uni(const unsigned char *rawstring, int boundlen,
			 wchar_t *uni)
{
	int n;
	unicode_t u;

	n = utf8_to_utf32(rawstring, boundlen, &u);
	if (n < 0 || u > MAX_WCHAR_T) {
		*uni = 0x003f;	/* ? */
		return -EINVAL;
	}
	*uni = (wchar_t) u;
	return n;
}

static unsigned char utf8_tolower(const struct nls_table *table,
				  unsigned int c)
{
	return c; /* Identity */
}

static unsigned char utf8_toupper(const struct nls_table *table,
				  unsigned int c)
{
	return c; /* Identity */
}

static const struct nls_ops utf8_ops = {
	.strncmp = utf8_strncmp,
	.strncasecmp = utf8_strncasecmp,
	.casefold = utf8_casefold,
	.normalize = utf8_normalize,
	.lowercase = utf8_tolower,
	.uppercase = utf8_toupper,
	.uni2char = utf8_uni2char,
	.char2uni = utf8_char2uni,
};

static int utf8_parse_version(const char *version, unsigned int *maj,
			      unsigned int *min, unsigned int *rev)
{
	substring_t args[3];
	char *tmp;
	const struct match_token token[] = {
		{1, "%d.%d.%d"},
		{0, NULL}
	};
	int ret = 0;

	tmp = kstrdup(version, GFP_KERNEL);
	if (match_token(tmp, token, args) != 1) {
		ret = -EINVAL;
		goto out;
	}

	if (match_int(&args[0], maj) || match_int(&args[1], min) ||
	    match_int(&args[2], rev)) {
		ret = -EINVAL;
		goto out;
	}
out:
	kfree(tmp);
	return ret;
}

static struct nls_table *utf8_load_charset(const char *version)
{
	struct nls_table *tbl = NULL;
	unsigned int nls_version;

	if (version) {
		unsigned int maj, min, rev;

		if (utf8_parse_version(version, &maj, &min, &rev) < 0)
			return ERR_PTR(-EINVAL);

		if (!utf8version_is_supported(maj, min, rev))
			return ERR_PTR(-EINVAL);

		nls_version = UNICODE_AGE(maj, min, rev);
	} else {
		nls_version = utf8version_latest();
		printk(KERN_WARNING"utf8norm version not specified. "
		       "Assuming latest supported version (%d.%d.%d).",
		       (nls_version >> 16) & 0xff, (nls_version >> 8) & 0xff,
		       (nls_version & 0xff));
	}

	 /* Try an already loaded table first. */
	for (tbl = utf8norm_info.tables; tbl; tbl = tbl->next) {
		if (tbl->version == nls_version)
			return tbl;
	}

	tbl = kmalloc(sizeof(struct nls_table), GFP_KERNEL);
	if (!tbl)
		return ERR_PTR(-ENOMEM);

	tbl->charset = &utf8norm_info;
	tbl->version = nls_version;
	tbl->ops = &utf8_ops;

	tbl->next = utf8norm_info.tables;
	utf8norm_info.tables = tbl;

	return tbl;
}

static void utf8_cleanup_tables(void)
{
	struct nls_table *tmp, *tbl = utf8norm_info.tables;

	while (tbl) {
		tmp = tbl;
		tbl = tbl->next;
		kfree(tmp);
	}
	utf8norm_info.tables = NULL;
}

static struct nls_charset utf8norm_info = {
	.charset = "utf8n",
	.load_table = utf8_load_charset,
};

static int __init init_utf8(void)
{
	register_nls(&utf8norm_info);
	return 0;
}

static void __exit exit_utf8(void)
{
	unregister_nls(&utf8norm_info);
	utf8_cleanup_tables();
}

module_init(init_utf8);
module_exit(exit_utf8);
MODULE_AUTHOR("SGI, Gabriel Krisman Bertazi");
MODULE_DESCRIPTION("UTF-8 charset operations for filesystems");
MODULE_LICENSE("GPL");
