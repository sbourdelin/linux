/*
 * linux/fs/nls/nls_core.c
 *
 * Native language support--charsets and unicode translations.
 * By Gordon Chaffee 1996, 1997
 *
 * Unicode based case conversion 1999 by Wolfram Pienkoss
 *
 */

#include <linux/module.h>
#include <linux/string.h>
#include <linux/nls.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/kmod.h>
#include <linux/spinlock.h>

extern struct nls_charset default_charset;
static struct nls_charset *charsets = &default_charset;
static DEFINE_SPINLOCK(nls_lock);

static int nls_validate_flags(struct nls_table *table, unsigned int flags)
{
	if (flags & NLS_STRICT_MODE && !table->ops->validate)
		return -1;

	return 0;
}

static struct nls_table *nls_load_table(struct nls_charset *charset,
					const char *version,
					unsigned int flags)
{
	struct nls_table *tbl;

	/* If there is no load_table hook, only 1 table is supported and
	 * it must have been loaded statically.
	 */
	if (charset->load_table)
		tbl = charset->load_table(version, flags);
	else
		tbl = charset->tables;

	if (IS_ERR(tbl))
		return tbl;

	if (nls_validate_flags(tbl, flags) < 0)
		return ERR_PTR(-EINVAL);

	tbl->flags = flags;
	return tbl;
}

int __register_nls(struct nls_charset *nls, struct module *owner)
{
	struct nls_charset **tmp = &charsets;

	if (nls->next)
		return -EBUSY;

	nls->owner = owner;
	spin_lock(&nls_lock);
	while (*tmp) {
		if (nls == *tmp) {
			spin_unlock(&nls_lock);
			return -EBUSY;
		}
		tmp = &(*tmp)->next;
	}
	nls->next = charsets;
	charsets = nls;
	spin_unlock(&nls_lock);
	return 0;
}
EXPORT_SYMBOL(__register_nls);

int unregister_nls(struct nls_charset * nls)
{
	struct nls_charset **tmp = &charsets;

	spin_lock(&nls_lock);
	while (*tmp) {
		if (nls == *tmp) {
			*tmp = nls->next;
			spin_unlock(&nls_lock);
			return 0;
		}
		tmp = &(*tmp)->next;
	}
	spin_unlock(&nls_lock);
	return -EINVAL;
}

static struct nls_charset *find_nls(const char *charset)
{
	struct nls_charset *nls;
	spin_lock(&nls_lock);
	for (nls = charsets; nls; nls = nls->next) {
		if (!strcmp(nls->charset, charset))
			break;
		if (nls->alias && !strcmp(nls->alias, charset))
			break;
	}

	if (!nls)
		nls = ERR_PTR(-EINVAL);
	else if (!try_module_get(nls->owner))
		nls = ERR_PTR(-EBUSY);

	spin_unlock(&nls_lock);
	return nls;
}

struct nls_table *load_nls_version(const char *charset, const char *version,
				   unsigned int flags)
{
	struct nls_charset *nls_charset;

	nls_charset = try_then_request_module(find_nls(charset),
					      "nls_%s", charset);
	if (IS_ERR(nls_charset))
		return ERR_PTR(-EINVAL);

	return nls_load_table(nls_charset, version, flags);
}
EXPORT_SYMBOL(load_nls_version);

struct nls_table *load_nls(char *charset)
{
	struct nls_table *table = load_nls_version(charset, NULL, 0);

	/* Pre-versioned load_nls() didn't return error pointers. Let's
	 * keep the abi for now to prevent breakage.
	 */
	if (IS_ERR(table))
		return NULL;

	return table;
}

void unload_nls(struct nls_table *nls)
{
	if (!IS_ERR_OR_NULL(nls))
		module_put(nls->charset->owner);
}

EXPORT_SYMBOL(unregister_nls);
EXPORT_SYMBOL(unload_nls);
EXPORT_SYMBOL(load_nls);

MODULE_LICENSE("Dual BSD/GPL");
