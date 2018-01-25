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

#ifndef _CHARSET_H
#define _CHARSET_H

#include <linux/types.h>

struct charset_info;
struct charset;

struct charset_ops {
	int (*strncmp)(const struct charset *charset, const char *str1,
		       size_t len1, const char *str2, size_t len2);
	int (*strncasecmp)(const struct charset *charset, const char *str1,
			   size_t len1, const char *str2, size_t len2);
	int (*casefold)(const struct charset *charset, const char *str,
			int len, char **folded);
	int (*normalize)(const struct charset *charset, const char *str,
			 int len, char **normalization);
};

struct charset {
	const struct charset_info *info;
	unsigned int version;
	const struct charset_ops *ops;
};

struct charset_info {
	char *name;
	char *match_token;
	struct charset* (*load_charset)(void *args);
};

static inline int charset_strncmp(const struct charset *charset,
				  const char *str1, size_t len1,
				  const char *str2, size_t len2)
{
	return charset->ops->strncmp(charset, str1, len1, str2, len2);
}

static inline int charset_strncasecmp(const struct charset *charset,
				      const char *str1, size_t len1,
				      const char *str2, size_t len2)
{
	return charset->ops->strncasecmp(charset, str1, len1, str2, len2);
}

static inline int charset_casefold(const struct charset *charset,
				   const char *str, int len, char **folded)
{
	return charset->ops->casefold(charset, str, len, folded);
}

static inline int charset_normalize(const struct charset *charset,
				    const char *str, int len,
				    char **normalization)
{
	return charset->ops->normalize(charset, str, len, normalization);
}

int charset_register(struct charset_info *charset);
const struct charset *charset_load(char *charset);
#endif
