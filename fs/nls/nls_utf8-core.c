/*
 * Module for handling utf8 just like any other charset.
 * By Urban Widmark 2000
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/parser.h>
#include <linux/nls.h>
#include <linux/errno.h>

#include "utf8n.h"

static unsigned char identity[256];
static struct nls_charset utf8_info;

static int uni2char(wchar_t uni, unsigned char *out, int boundlen)
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

static int char2uni(const unsigned char *rawstring, int boundlen, wchar_t *uni)
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

static unsigned char charset_tolower(const struct nls_table *table,
				     unsigned int c){
	return identity[c];
}

static unsigned char charset_toupper(const struct nls_table *table,
				     unsigned int c) {
	return identity[c];
}

#ifdef CONFIG_NLS_UTF8_NORMALIZATION

static int utf8_validate(const struct nls_table *charset,
			 const unsigned char *str, size_t len)
{
	const struct utf8data *data = utf8nfkdi(charset->version);

	if (utf8nlen(data, str, len) < 0)
		return -1;
	return 0;
}

static int utf8_strncmp(const struct nls_table *charset,
			const unsigned char *str1, size_t len1,
			const unsigned char *str2, size_t len2)
{
	const struct utf8data *data = utf8nfkdi(charset->version);
	struct utf8cursor cur1, cur2;
	int c1, c2;

	if (utf8ncursor(&cur1, data, str1, len1) < 0)
		goto invalid_seq;

	if (utf8ncursor(&cur2, data, str2, len2) < 0)
		goto invalid_seq;

	do {
		c1 = utf8byte(&cur1);
		c2 = utf8byte(&cur2);

		if (c1 < 0 || c2 < 0)
			goto invalid_seq;
		if (c1 != c2)
			return 1;
	} while (c1);

	return 0;

invalid_seq:
	if(IS_STRICT_MODE(charset))
		return -EINVAL;

	/* Treat the sequence as a binary blob. */
	if (len1 != len2)
		return 1;

	return !!memcmp(str1, str2, len1);
}

static int utf8_strncasecmp(const struct nls_table *charset,
			    const unsigned char *str1, size_t len1,
			    const unsigned char *str2, size_t len2)
{
	const struct utf8data *data = utf8nfkdicf(charset->version);
	struct utf8cursor cur1, cur2;
	int c1, c2;

	if (utf8ncursor(&cur1, data, str1, len1) < 0)
		goto invalid_seq;

	if (utf8ncursor(&cur2, data, str2, len2) < 0)
		goto invalid_seq;

	do {
		c1 = utf8byte(&cur1);
		c2 = utf8byte(&cur2);

		if (c1 < 0 || c2 < 0)
			goto invalid_seq;
		if (c1 != c2)
			return 1;
	} while (c1);

	return 0;

invalid_seq:
	if(IS_STRICT_MODE(charset))
		return -EINVAL;

	/* Treat the sequence as a binary blob. */
	if (len1 != len2)
		return 1;

	return !!memcmp(str1, str2, len1);
}

static int utf8_casefold_nfkdcf(const struct nls_table *charset,
				const unsigned char *str, size_t len,
				unsigned char *dest, size_t dlen)
{
	const struct utf8data *data = utf8nfkdicf(charset->version);
	struct utf8cursor cur;
	size_t nlen = 0;

	if (utf8ncursor(&cur, data, str, len) < 0)
		goto invalid_seq;

	for (nlen = 0; nlen < dlen; nlen++) {
		dest[nlen] = utf8byte(&cur);
		if (!dest[nlen])
			return nlen;
		if (dest[nlen] == -1)
			break;
	}

invalid_seq:
	if (IS_STRICT_MODE(charset))
		return -EINVAL;

	/* Treat the sequence as a binary blob. */
	memcpy(dest, str, len);
	return len;
}

static int utf8_normalize_nfkd(const struct nls_table *charset,
			       const unsigned char *str,
			       size_t len, unsigned char *dest, size_t dlen)
{
	const struct utf8data *data = utf8nfkdi(charset->version);
	struct utf8cursor cur;
	ssize_t nlen = 0;

	if (utf8ncursor(&cur, data, str, len) < 0)
		goto invalid_seq;

	for (nlen = 0; nlen < dlen; nlen++) {
		dest[nlen] = utf8byte(&cur);
		if (!dest[nlen])
			return nlen;
		if (dest[nlen] == -1)
			break;
	}

invalid_seq:
	if (IS_STRICT_MODE(charset))
		return -EINVAL;

	/* Treat the sequence as a binary blob. */
	memcpy(dest, str, len);
	return len;
}

static int utf8_parse_version(const char *version, unsigned int *maj,
			      unsigned int *min, unsigned int *rev)
{
	substring_t args[3];
	char version_string[12];
	const struct match_token token[] = {
		{1, "%d.%d.%d"},
		{0, NULL}
	};

	strncpy(version_string, version, sizeof(version_string));

	if (match_token(version_string, token, args) != 1)
		return -EINVAL;

	if (match_int(&args[0], maj) || match_int(&args[1], min) ||
	    match_int(&args[2], rev))
		return -EINVAL;

	return 0;
}
#endif

struct utf8_table {
	struct nls_table tbl;
	struct nls_ops ops;
};

static void utf8_set_ops(struct utf8_table *utbl)
{
	utbl->ops.lowercase = charset_toupper;
	utbl->ops.uppercase = charset_tolower;
	utbl->ops.uni2char = uni2char;
	utbl->ops.char2uni = char2uni;

#ifdef CONFIG_NLS_UTF8_NORMALIZATION
	utbl->ops.validate = utf8_validate;

	if (IS_NORMALIZATION_TYPE_UTF8_NFKD(&utbl->tbl)) {
		utbl->ops.normalize = utf8_normalize_nfkd;
		utbl->ops.strncmp = utf8_strncmp;
	}

	if (IS_CASEFOLD_TYPE_UTF8_NFKDCF(&utbl->tbl)) {
		utbl->ops.casefold = utf8_casefold_nfkdcf;
		utbl->ops.strncasecmp = utf8_strncasecmp;
	}
#endif

	utbl->tbl.ops = &utbl->ops;
}

static struct nls_table *utf8_load_table(const char *version, unsigned int flags)
{
	struct utf8_table *utbl = NULL;
	unsigned int nls_version;

#ifdef CONFIG_NLS_UTF8_NORMALIZATION
	if (version) {
		unsigned int maj, min, rev;

		if (utf8_parse_version(version, &maj, &min, &rev) < 0)
			return ERR_PTR(-EINVAL);

		if (!utf8version_is_supported(maj, min, rev))
			return ERR_PTR(-EINVAL);

		nls_version = UNICODE_AGE(maj, min, rev);
	} else {
		nls_version = utf8version_latest();
		printk(KERN_WARNING"UTF-8 version not specified. "
		       "Assuming latest supported version (%d.%d.%d).",
		       (nls_version >> 16) & 0xff, (nls_version >> 8) & 0xff,
		       (nls_version & 0xff));
	}
#else
	nls_version = 0;
#endif

	utbl = kzalloc(sizeof(struct utf8_table), GFP_KERNEL);
	if (!utbl)
		return ERR_PTR(-ENOMEM);

	utbl->tbl.charset = &utf8_info;
	utbl->tbl.version = nls_version;
	utbl->tbl.flags = flags;
	utf8_set_ops(utbl);

	utbl->tbl.next = utf8_info.tables;
	utf8_info.tables = &utbl->tbl;

	return &utbl->tbl;
}

static void utf8_cleanup_tables(void)
{
	struct nls_table *tmp, *tbl = utf8_info.tables;

	while (tbl) {
		tmp = tbl;
		tbl = tbl->next;
		kfree(tmp);
	}
	utf8_info.tables = NULL;
}

static struct nls_charset utf8_info = {
	.charset = "utf8",
	.load_table = utf8_load_table,
};

static int __init init_nls_utf8(void)
{
	int i;
	for (i=0; i<256; i++)
		identity[i] = i;

        return register_nls(&utf8_info);
}

static void __exit exit_nls_utf8(void)
{
	unregister_nls(&utf8_info);
	utf8_cleanup_tables();
}

module_init(init_nls_utf8)
module_exit(exit_nls_utf8)
MODULE_LICENSE("Dual BSD/GPL");
