/*
 * Cryptographic API.
 *
 * Synchronous compression operations.
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
#include <linux/cryptouser.h>
#include <linux/vmalloc.h>
#include <linux/highmem.h>
#include <linux/gfp.h>

#include <crypto/compress.h>
#include <crypto/scatterwalk.h>
#include <net/netlink.h>

#include "internal.h"


static int crypto_scomp_init(struct crypto_tfm *tfm, u32 type, u32 mask)
{
	return 0;
}

static int crypto_scomp_init_tfm(struct crypto_tfm *tfm)
{
	return 0;
}

#ifdef CONFIG_NET
static int crypto_scomp_report(struct sk_buff *skb, struct crypto_alg *alg)
{
	struct crypto_report_comp rcomp;

	strncpy(rcomp.type, "scomp", sizeof(rcomp.type));
	if (nla_put(skb, CRYPTOCFGA_REPORT_COMPRESS,
		    sizeof(struct crypto_report_comp), &rcomp))
		goto nla_put_failure;
	return 0;

nla_put_failure:
	return -EMSGSIZE;
}
#else
static int crypto_scomp_report(struct sk_buff *skb, struct crypto_alg *alg)
{
	return -ENOSYS;
}
#endif

static void crypto_scomp_show(struct seq_file *m, struct crypto_alg *alg)
	__attribute__ ((unused));
static void crypto_scomp_show(struct seq_file *m, struct crypto_alg *alg)
{
	seq_puts(m, "type         : scomp\n");
}

static const struct crypto_type crypto_scomp_type = {
	.extsize	= crypto_alg_extsize,
	.init		= crypto_scomp_init,
	.init_tfm	= crypto_scomp_init_tfm,
#ifdef CONFIG_PROC_FS
	.show		= crypto_scomp_show,
#endif
	.report		= crypto_scomp_report,
	.maskclear	= ~CRYPTO_ALG_TYPE_MASK,
	.maskset	= CRYPTO_ALG_TYPE_MASK,
	.type		= CRYPTO_ALG_TYPE_SCOMPRESS,
	.tfmsize	= offsetof(struct crypto_scomp, base),
};

struct crypto_scomp *crypto_alloc_scomp(const char *alg_name, u32 type,
					u32 mask)
{
	return crypto_alloc_tfm(alg_name, &crypto_scomp_type, type, mask);
}
EXPORT_SYMBOL_GPL(crypto_alloc_scomp);

static void *scomp_map(struct scatterlist *sg, unsigned int len)
{
	gfp_t gfp_flags;
	void *buf;

	if (sg_is_last(sg))
		return kmap_atomic(sg_page(sg)) + sg->offset;

	if (in_atomic() || irqs_disabled())
		gfp_flags = GFP_ATOMIC;
	else
		gfp_flags = GFP_KERNEL;

	buf = kmalloc(len, gfp_flags);
	if (!buf)
		return NULL;

	scatterwalk_map_and_copy(buf, sg, 0, len, 0);

	return buf;
}

static void scomp_unmap(struct scatterlist *sg, void *buf, unsigned int len)
{
	if (!buf)
		return;

	if (sg_is_last(sg)) {
		kunmap_atomic(buf);
		return;
	}

	scatterwalk_map_and_copy(buf, sg, 0, len, 1);
	kfree(buf);
}

static int scomp_acomp_compress(struct acomp_req *req,
			 struct crypto_acomp *tfm)
{
	int ret;
	void **tfm_ctx = crypto_acomp_ctx(tfm);
	struct crypto_scomp *scomp = (struct crypto_scomp *)*tfm_ctx;
	void *ctx = *(req->__ctx);
	char *src = scomp_map(req->src, req->src_len);
	char *dst = scomp_map(req->dst, req->dst_len);

	if (!src || !dst) {
		ret = -ENOMEM;
		goto out;
	}

	req->out_len = req->dst_len;
	ret = crypto_scomp_compress(scomp, src, req->src_len,
				dst, &req->out_len, ctx);

out:
	scomp_unmap(req->src, src, 0);
	scomp_unmap(req->dst, dst, ret ? 0 : req->out_len);

	return ret;
}

static int scomp_async_compress(struct acomp_req *req)
{
	return scomp_acomp_compress(req, crypto_acomp_reqtfm(req));
}

static int scomp_acomp_decompress(struct acomp_req *req,
			   struct crypto_acomp *tfm)
{
	int ret;
	void **tfm_ctx = crypto_acomp_ctx(tfm);
	struct crypto_scomp *scomp = (struct crypto_scomp *)*tfm_ctx;
	void *ctx = *(req->__ctx);
	char *src = scomp_map(req->src, req->src_len);
	char *dst = scomp_map(req->dst, req->dst_len);

	if (!src || !dst) {
		ret = -ENOMEM;
		goto out;
	}

	req->out_len = req->dst_len;
	ret = crypto_scomp_decompress(scomp, src, req->src_len,
				dst, &req->out_len, ctx);

out:
	scomp_unmap(req->src, src, 0);
	scomp_unmap(req->dst, dst, ret ? 0 : req->out_len);

	return ret;
}

static int scomp_async_decompress(struct acomp_req *req)
{
	return scomp_acomp_decompress(req, crypto_acomp_reqtfm(req));
}

static void crypto_exit_scomp_ops_async(struct crypto_tfm *tfm)
{
	struct crypto_scomp **ctx = crypto_tfm_ctx(tfm);

	crypto_free_scomp(*ctx);
}

int crypto_init_scomp_ops_async(struct crypto_tfm *tfm)
{
	struct crypto_alg *calg = tfm->__crt_alg;
	struct crypto_acomp *acomp = __crypto_acomp_tfm(tfm);
	struct crypto_scomp *scomp;
	void **ctx = crypto_tfm_ctx(tfm);

	if (!crypto_mod_get(calg))
		return -EAGAIN;

	scomp = crypto_create_tfm(calg, &crypto_scomp_type);
	if (IS_ERR(scomp)) {
		crypto_mod_put(calg);
		return PTR_ERR(scomp);
	}

	*ctx = scomp;
	tfm->exit = crypto_exit_scomp_ops_async;

	acomp->compress = scomp_async_compress;
	acomp->decompress = scomp_async_decompress;
	acomp->reqsize = sizeof(void *);

	return 0;
}

struct acomp_req *crypto_scomp_acomp_request_alloc(struct crypto_acomp *tfm,
							gfp_t gfp)
{
	void **tfm_ctx = crypto_acomp_ctx(tfm);
	struct crypto_scomp *scomp = (struct crypto_scomp *)*tfm_ctx;
	struct acomp_req *req;
	void *ctx;

	req = kzalloc(sizeof(*req) + crypto_acomp_reqsize(tfm), gfp);
	if (!req)
		return NULL;

	ctx = crypto_scomp_alloc_ctx(scomp);
	if (IS_ERR(ctx)) {
		kfree(req);
		return NULL;
	}

	*(req->__ctx) = ctx;
	acomp_request_set_tfm(req, tfm);

	return req;
}

void crypto_scomp_acomp_request_free(struct acomp_req *req)
{
	struct crypto_acomp *tfm = crypto_acomp_reqtfm(req);
	void **tfm_ctx = crypto_acomp_ctx(tfm);
	struct crypto_scomp *scomp = (struct crypto_scomp *)*tfm_ctx;
	void *ctx = *(req->__ctx);

	crypto_scomp_free_ctx(scomp, ctx);
	kfree(req);
}

int crypto_register_scomp(struct scomp_alg *alg)
{
	struct crypto_alg *base = &alg->base;

	base->cra_type = &crypto_scomp_type;
	base->cra_flags &= ~CRYPTO_ALG_TYPE_MASK;
	base->cra_flags |= CRYPTO_ALG_TYPE_SCOMPRESS;

	return crypto_register_alg(base);
}
EXPORT_SYMBOL_GPL(crypto_register_scomp);

int crypto_unregister_scomp(struct scomp_alg *alg)
{
	return crypto_unregister_alg(&alg->base);
}
EXPORT_SYMBOL_GPL(crypto_unregister_scomp);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Synchronous compression operations");
MODULE_AUTHOR("LG Electronics Inc.");

