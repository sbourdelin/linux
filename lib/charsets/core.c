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

#include <linux/module.h>
#include <linux/string.h>
#include <linux/charsets.h>
#include <linux/parser.h>

#define MAX_ENCODINGS 10

static struct match_token encoding_tokens[MAX_ENCODINGS + 1];
static struct charset_info *charsets[MAX_ENCODINGS];
static int n_encodings;

const struct charset *charset_load(char *charset)
{
	substring_t args[MAX_OPT_ARGS];
	int token;

	args[0].to = args[0].from = NULL;
	token = match_token(charset, encoding_tokens, args);

	if (!encoding_tokens[token].pattern)
		return NULL;

	return charsets[token]->load_charset(args);
}
EXPORT_SYMBOL(charset_load);

int charset_register(struct charset_info *charset)
{
	encoding_tokens[n_encodings].token = n_encodings;
	encoding_tokens[n_encodings].pattern = charset->match_token;

	charsets[n_encodings] = charset;
	n_encodings += 1;
	return 0;
}
EXPORT_SYMBOL(charset_register);

static int __init init_charset(void)
{
	memset(encoding_tokens, 0, sizeof(encoding_tokens));
	n_encodings = 0;

	return 0;
}

static void __exit exit_charset(void)
{
}

module_init(init_charset);
module_exit(exit_charset);

MODULE_AUTHOR("Gabriel Krisman Bertazi");
MODULE_DESCRIPTION("charset abstraction for filesystems");
MODULE_LICENSE("GPL");
