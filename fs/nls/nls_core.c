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

static struct nls_table default_table;
static struct nls_table *tables = &default_table;
static DEFINE_SPINLOCK(nls_lock);

int __register_nls(struct nls_table *nls, struct module *owner)
{
	struct nls_table ** tmp = &tables;

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
	nls->next = tables;
	tables = nls;
	spin_unlock(&nls_lock);
	return 0;
}
EXPORT_SYMBOL(__register_nls);

int unregister_nls(struct nls_table * nls)
{
	struct nls_table ** tmp = &tables;

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

static struct nls_table *find_nls(char *charset)
{
	struct nls_table *nls;
	spin_lock(&nls_lock);
	for (nls = tables; nls; nls = nls->next) {
		if (!strcmp(nls_charset_name(nls), charset))
			break;
		if (nls->alias && !strcmp(nls->alias, charset))
			break;
	}
	if (nls && !try_module_get(nls->owner))
		nls = NULL;
	spin_unlock(&nls_lock);
	return nls;
}

struct nls_table *load_nls(char *charset)
{
	return try_then_request_module(find_nls(charset), "nls_%s", charset);
}

void unload_nls(struct nls_table *nls)
{
	if (nls)
		module_put(nls->owner);
}

EXPORT_SYMBOL(unregister_nls);
EXPORT_SYMBOL(unload_nls);
EXPORT_SYMBOL(load_nls);

MODULE_LICENSE("Dual BSD/GPL");
