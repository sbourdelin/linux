/*
 * Cryptographic API.
 *
 * Contextless (de)compression operations.
 *
 * Copyright 2015 LG Electronics Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.
 * If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/crypto.h>
#include <linux/errno.h>
#include <linux/module.h>
#include <linux/seq_file.h>

#include <crypto/compress.h>

#include "internal.h"


static int crypto_ccomp_init(struct crypto_tfm *tfm, u32 type, u32 mask)
{
	return 0;
}

static int crypto_ccomp_init_tfm(struct crypto_tfm *tfm)
{
	return 0;
}

static int crypto_ccomp_report(struct sk_buff *skb, struct crypto_alg *alg)
{
	return -EINVAL;
}

static void crypto_ccomp_show(struct seq_file *m, struct crypto_alg *alg)
	__attribute__ ((unused));
static void crypto_ccomp_show(struct seq_file *m, struct crypto_alg *alg)
{
	seq_puts(m, "type         : ccomp\n");
}

static const struct crypto_type crypto_ccomp_type = {
	.extsize	= crypto_alg_extsize,
	.init		= crypto_ccomp_init,
	.init_tfm	= crypto_ccomp_init_tfm,
#ifdef CONFIG_PROC_FS
	.show		= crypto_ccomp_show,
#endif
	.report		= crypto_ccomp_report,
	.maskclear	= ~CRYPTO_ALG_TYPE_MASK,
	.maskset	= CRYPTO_ALG_TYPE_MASK,
	.type		= CRYPTO_ALG_TYPE_CCOMPRESS,
	.tfmsize	= offsetof(struct crypto_ccomp, base),
};

struct crypto_ccomp *crypto_alloc_ccomp(const char *alg_name, u32 type,
					u32 mask)
{
	return crypto_alloc_tfm(alg_name, &crypto_ccomp_type, type, mask);
}
EXPORT_SYMBOL_GPL(crypto_alloc_ccomp);

int crypto_register_ccomp(struct ccomp_alg *alg)
{
	struct crypto_alg *base = &alg->base;

	base->cra_type = &crypto_ccomp_type;
	base->cra_flags &= ~CRYPTO_ALG_TYPE_MASK;
	base->cra_flags |= CRYPTO_ALG_TYPE_CCOMPRESS;

	return crypto_register_alg(base);
}
EXPORT_SYMBOL_GPL(crypto_register_ccomp);

int crypto_unregister_ccomp(struct ccomp_alg *alg)
{
	return crypto_unregister_alg(&alg->base);
}
EXPORT_SYMBOL_GPL(crypto_unregister_ccomp);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Contextless (de)compression operations");
MODULE_AUTHOR("LG Electronics Inc.");
