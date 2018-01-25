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

#include <linux/charsets.h>
#include <linux/utf8norm.h>
#include <linux/slab.h>
#include <linux/parser.h>
#include <linux/string.h>

static int utf8_strncmp(const struct charset *charset, const char *str1,
			size_t len1, const char *str2, size_t len2)
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

static int utf8_strncasecmp(const struct charset *charset, const char *str1,
			    size_t len1, const char *str2, size_t len2)
{
	const struct utf8data *data = utf8nfkdicf(charset->version);
	struct utf8cursor cur1, cur2;
	unsigned char c1, c2;
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

int utf8_casefold(const struct charset *charset, const char *str, int len,
		  char **folded)
{
	const struct utf8data *data = utf8nfkdicf(charset->version);
	struct utf8cursor cur;
	char *s;
	ssize_t nlen;

	nlen = utf8nlen(data, str, len);
	if (nlen < 0)
		return -EINVAL;

	s = kmalloc(nlen + 1, GFP_NOFS);
	if (!s)
		return -ENOMEM;
	*folded = s;

	utf8ncursor(&cur, data, str, len);
	do {
		*s = utf8byte(&cur);
	} while (*s++);

	return nlen;
}

int utf8_normalize(const struct charset *charset, const char *str, int len,
		   char **normalization)
{
	const struct utf8data *data = utf8nfkdi(charset->version);
	struct utf8cursor cur;
	char *s;
	ssize_t nlen;

	nlen = utf8nlen(data, str, len);
	if (nlen < 0)
		return -EINVAL;

	s = kmalloc(nlen + 1, GFP_NOFS);
	if (!s)
		return -ENOMEM;
	*normalization = s;

	utf8ncursor(&cur, data, str, len);
	do {
		*s = utf8byte(&cur);
	} while (*s++);

	return nlen;
}

static const struct charset_ops utf8_ops = {
	.strncmp = utf8_strncmp,
	.strncasecmp = utf8_strncasecmp,
	.casefold = utf8_casefold,
	.normalize = utf8_normalize,
};

static struct charset *utf8_load_charset(void *pargs)
{
	int maj, min, rev;
	struct charset *charset;
	substring_t *args = pargs;

	if (match_int(&args[0], &maj) || match_int(&args[1], &min) ||
	    match_int(&args[2], &rev))
		return NULL;

	if (!utf8version_is_supported(maj, min, rev))
		return NULL;

	charset = kmalloc(sizeof(struct charset), GFP_KERNEL);
	if (!charset)
		return NULL;

	charset->info = NULL;
	charset->version = UNICODE_AGE(maj, min, rev);
	charset->ops = &utf8_ops;

	return charset;
}

static struct charset_info utf8_info = {
	.name = "utf8",
	.match_token = "utf8-%d.%d.%d",
	.load_charset = utf8_load_charset,
};

static int __init init_utf8(void)
{
	charset_register(&utf8_info);
	return 0;
}

static void __exit exit_utf8(void)
{
}

module_init(init_utf8);
module_exit(exit_utf8);
MODULE_AUTHOR("Gabriel Krisman Bertazi");
MODULE_DESCRIPTION("UTF-8 charset operations for filesystems");
MODULE_LICENSE("GPL");

