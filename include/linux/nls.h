/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_NLS_H
#define _LINUX_NLS_H

#include <linux/init.h>
#include <linux/string.h>

/* Unicode has changed over the years.  Unicode code points no longer
 * fit into 16 bits; as of Unicode 5 valid code points range from 0
 * to 0x10ffff (17 planes, where each plane holds 65536 code points).
 *
 * The original decision to represent Unicode characters as 16-bit
 * wchar_t values is now outdated.  But plane 0 still includes the
 * most commonly used characters, so we will retain it.  The newer
 * 32-bit unicode_t type can be used when it is necessary to
 * represent the full Unicode character set.
 */

/* Plane-0 Unicode character */
typedef u16 wchar_t;
#define MAX_WCHAR_T	0xffff

/* Arbitrary Unicode character */
typedef u32 unicode_t;
struct nls_table;

struct nls_ops {
	int (*uni2char) (wchar_t uni, unsigned char *out, int boundlen);
	int (*char2uni) (const unsigned char *rawstring, int boundlen,
			 wchar_t *uni);
	int (*strncmp)(const struct nls_table *charset,
		       const unsigned char *str1, size_t len1,
		       const unsigned char *str2, size_t len2);
	int (*strncasecmp)(const struct nls_table *charset,
			   const unsigned char *str1, size_t len1,
			   const unsigned char *str2, size_t len2);
	unsigned char (*lowercase)(const struct nls_table *charset,
				   unsigned int c);
	unsigned char (*uppercase)(const struct nls_table *charset,
				   unsigned int c);

};

struct nls_table {
	const struct nls_charset *charset;
	unsigned int version;

	const struct nls_ops *ops;
	struct nls_table *next;

};

struct nls_charset {
	const char *charset;
	const char *alias;
	struct module *owner;
	struct nls_table *tables;
	struct nls_charset *next;
	struct nls_table *(*load_table)(const char *version);
};

/* this value hold the maximum octet of charset */
#define NLS_MAX_CHARSET_SIZE 6 /* for UTF-8 */

/* Byte order for UTF-16 strings */
enum utf16_endian {
	UTF16_HOST_ENDIAN,
	UTF16_LITTLE_ENDIAN,
	UTF16_BIG_ENDIAN
};

/* nls_base.c */
extern int __register_nls(struct nls_charset *, struct module *);
extern int unregister_nls(struct nls_charset *);
extern struct nls_table *load_nls(char *);
extern struct nls_table *load_nls_version(const char *charset,
					  const char *version);
extern void unload_nls(struct nls_table *);
extern struct nls_table *load_nls_default(void);
#define register_nls(nls) __register_nls((nls), THIS_MODULE)

extern int utf8_to_utf32(const u8 *s, int len, unicode_t *pu);
extern int utf32_to_utf8(unicode_t u, u8 *s, int maxlen);
extern int utf8s_to_utf16s(const u8 *s, int len,
		enum utf16_endian endian, wchar_t *pwcs, int maxlen);
extern int utf16s_to_utf8s(const wchar_t *pwcs, int len,
		enum utf16_endian endian, u8 *s, int maxlen);

static inline int nls_uni2char(const struct nls_table *table, wchar_t uni,
			       unsigned char *out, int boundlen)
{
	return table->ops->uni2char(uni, out, boundlen);
}

static inline int nls_char2uni(const struct nls_table *table,
			       const unsigned char *rawstring,
			       int boundlen, wchar_t *uni)
{
	return table->ops->char2uni(rawstring, boundlen, uni);
}

static inline const char *nls_charset_name(const struct nls_table *table)
{
	return table->charset->charset;
}

static inline unsigned char nls_tolower(struct nls_table *t, unsigned char c)
{
	unsigned char nc = t->ops->lowercase(t, c);

	return nc ? nc : c;
}

static inline unsigned char nls_toupper(struct nls_table *t, unsigned char c)
{
	unsigned char nc = t->ops->uppercase(t, c);

	return nc ? nc : c;
}

static inline int nls_strncasecmp(struct nls_table *t,
				  const unsigned char *s1, size_t len1,
				  const unsigned char *s2, size_t len2)
{
	if (t->ops->strncasecmp)
		return t->ops->strncasecmp(t, s1, len1, s2, len2);

	if (len1 != len2)
		return 1;

	while (len1--) {
		if (nls_tolower(t, *s1++) != nls_tolower(t, *s2++))
			return 1;
	}

	return 0;
}

static inline int nls_strncmp(struct nls_table *t,
			      const unsigned char *s1, size_t len1,
			      const unsigned char *s2, size_t len2)
{
	if (t->ops->strncmp)
		return t->ops->strncmp(t, s1, len1, s2, len2);

	if (len1 != len2)
		return 1;

	/* strnicmp did not return negative values. So let's keep the
	 * abi for now */
	return !!memcmp(s1, s2, len1);
}

static inline int nls_strnicmp(struct nls_table *t, const unsigned char *s1,
		const unsigned char *s2, int len)
{
	return nls_strncasecmp(t, s1, len, s2, len);
}

/*
 * nls_nullsize - return length of null character for codepage
 * @codepage - codepage for which to return length of NULL terminator
 *
 * Since we can't guarantee that the null terminator will be a particular
 * length, we have to check against the codepage. If there's a problem
 * determining it, assume a single-byte NULL terminator.
 */
static inline int
nls_nullsize(const struct nls_table *codepage)
{
	int charlen;
	char tmp[NLS_MAX_CHARSET_SIZE];

	charlen = codepage->ops->uni2char(0, tmp, NLS_MAX_CHARSET_SIZE);

	return charlen > 0 ? charlen : 1;
}

#define MODULE_ALIAS_NLS(name)	MODULE_ALIAS("nls_" __stringify(name))

#endif /* _LINUX_NLS_H */

