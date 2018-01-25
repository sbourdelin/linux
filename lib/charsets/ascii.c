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
#include <linux/types.h>
#include <linux/module.h>
#include <linux/charsets.h>
#include <linux/string.h>
#include <linux/ctype.h>
#include <linux/parser.h>

static struct charset_info ascii_info;

int ascii_strncmp(const struct charset *charset, const char *str1,
		  size_t len1, const char *str2, size_t len2)
{
	size_t len = (len1 < len2)? len1:len2;

	return strncmp(str1, str2, len);
}

int ascii_strncasecmp(const struct charset *charset, const char *str1,
		      size_t len1, const char *str2, size_t len2)
{
	size_t len = (len1 < len2)? len1:len2;

	return strncasecmp(str1, str2, len);
}

int ascii_normalize(const struct charset *charset, const char *str,
		    int len, char **normalization)
{
	*normalization = kstrdup(str, GFP_NOFS);
	return (*normalization) ? len : -ENOMEM;
}

int ascii_casefold(const struct charset *charset, const char *str,
		   int len, char **folded_str)
{
	int i;
	char *fold;

	fold = kstrdup(str, GFP_NOFS);
	if (!fold)
		return -ENOMEM;

	for (i = 0; i < len; i++)
		fold[i] = tolower(fold[i]);

	*folded_str = fold;
	return len;
}

static const struct charset_ops ascii_ops = {
	.strncmp = ascii_strncmp,
	.strncasecmp = ascii_strncasecmp,
	.casefold = ascii_casefold,
	.normalize = ascii_normalize,
};

static struct charset ascii_charset = {
	.version = 0,
	.info = &ascii_info,
	.ops = &ascii_ops
};

static struct charset *ascii_load_charset(void *pargs)
{
	return &ascii_charset;
}

static struct charset_info ascii_info = {
	.name = "ascii",
	.match_token = "ascii",
	.load_charset = ascii_load_charset,
};

static int __init init_ascii(void)
{
	charset_register(&ascii_info);
	return 0;
}

static void __exit exit_ascii(void)
{
}

module_init(init_ascii);
module_exit(exit_ascii);
MODULE_AUTHOR("Gabriel Krisman Bertazi");
MODULE_DESCRIPTION("ASCII charset for filesystems");
MODULE_LICENSE("GPL");

