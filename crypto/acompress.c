/*
 * Asynchronous compression operations
 *
 * Copyright (c) 2015, Intel Corporation
 * Authors: Weigang Li <weigang.li@intel.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 */
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/crypto.h>

#include <crypto/algapi.h>
#include <linux/cryptouser.h>
#include <net/netlink.h>
#include <crypto/compress.h>
#include <crypto/internal/compress.h>
#include "internal.h"

const struct crypto_type crypto_acomp_type;

#ifdef CONFIG_NET
static int crypto_acomp_report(struct sk_buff *skb, struct crypto_alg *alg)
{
	struct crypto_report_comp racomp;

	strncpy(racomp.type, "acomp", sizeof(racomp.type));

	if (nla_put(skb, CRYPTOCFGA_REPORT_COMPRESS,
		    sizeof(struct crypto_report_comp), &racomp))
		goto nla_put_failure;
	return 0;

nla_put_failure:
	return -EMSGSIZE;
}
#else
static int crypto_acomp_report(struct sk_buff *skb, struct crypto_alg *alg)
{
	return -ENOSYS;
}
#endif

static void crypto_acomp_show(struct seq_file *m, struct crypto_alg *alg)
	__attribute__ ((unused));

static void crypto_acomp_show(struct seq_file *m, struct crypto_alg *alg)
{
	seq_puts(m, "type         : acomp\n");
}

static void crypto_acomp_exit_tfm(struct crypto_tfm *tfm)
{
	struct crypto_acomp *acomp = __crypto_acomp_tfm(tfm);
	struct acomp_alg *alg = crypto_acomp_alg(acomp);

	alg->exit(acomp);
}

static int crypto_acomp_init_tfm(struct crypto_tfm *tfm)
{
	struct crypto_acomp *acomp = __crypto_acomp_tfm(tfm);
	struct acomp_alg *alg = crypto_acomp_alg(acomp);

	if (tfm->__crt_alg->cra_type != &crypto_acomp_type)
		return crypto_init_scomp_ops_async(tfm);

	acomp->compress = alg->compress;
	acomp->decompress = alg->decompress;

	if (alg->exit)
		acomp->base.exit = crypto_acomp_exit_tfm;

	if (alg->init)
		return alg->init(acomp);

	return 0;
}

static unsigned int crypto_acomp_extsize(struct crypto_alg *alg)
{
	if (alg->cra_type == &crypto_acomp_type)
		return alg->cra_ctxsize;

	return sizeof(void *);
}

const struct crypto_type crypto_acomp_type = {
	.extsize = crypto_acomp_extsize,
	.init_tfm = crypto_acomp_init_tfm,
#ifdef CONFIG_PROC_FS
	.show = crypto_acomp_show,
#endif
	.report = crypto_acomp_report,
	.maskclear = ~CRYPTO_ALG_TYPE_MASK,
	.maskset = CRYPTO_ALG_TYPE_ACOMPRESS_MASK,
	.type = CRYPTO_ALG_TYPE_ACOMPRESS,
	.tfmsize = offsetof(struct crypto_acomp, base),
};
EXPORT_SYMBOL_GPL(crypto_acomp_type);

struct crypto_acomp *crypto_alloc_acomp(const char *alg_name, u32 type,
					u32 mask)
{
	return crypto_alloc_tfm(alg_name, &crypto_acomp_type, type, mask);
}
EXPORT_SYMBOL_GPL(crypto_alloc_acomp);

struct acomp_req *acomp_request_alloc(struct crypto_acomp *acomp,
						gfp_t gfp)
{
	struct crypto_tfm *tfm = crypto_acomp_tfm(acomp);
	struct acomp_req *req;

	if (tfm->__crt_alg->cra_type != &crypto_acomp_type)
		return crypto_scomp_acomp_request_alloc(acomp, gfp);

	req = kzalloc(sizeof(*req) + crypto_acomp_reqsize(acomp), gfp);
	if (likely(req))
		acomp_request_set_tfm(req, acomp);

	return req;
}
EXPORT_SYMBOL_GPL(acomp_request_alloc);

void acomp_request_free(struct acomp_req *req)
{
	struct crypto_acomp *acomp = crypto_acomp_reqtfm(req);
	struct crypto_tfm *tfm = crypto_acomp_tfm(acomp);

	if (tfm->__crt_alg->cra_type != &crypto_acomp_type)
		return crypto_scomp_acomp_request_free(req);

	kfree(req);
}
EXPORT_SYMBOL_GPL(acomp_request_free);

int crypto_register_acomp(struct acomp_alg *alg)
{
	struct crypto_alg *base = &alg->base;

	base->cra_type = &crypto_acomp_type;
	base->cra_flags &= ~CRYPTO_ALG_TYPE_MASK;
	base->cra_flags |= CRYPTO_ALG_TYPE_ACOMPRESS;
	return crypto_register_alg(base);
}
EXPORT_SYMBOL_GPL(crypto_register_acomp);

int crypto_unregister_acomp(struct acomp_alg *alg)
{
	return crypto_unregister_alg(&alg->base);
}
EXPORT_SYMBOL_GPL(crypto_unregister_acomp);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Asynchronous compression type");
