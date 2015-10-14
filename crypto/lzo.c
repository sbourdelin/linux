/*
 * Cryptographic API.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 51
 * Franklin St, Fifth Floor, Boston, MA 02110-1301 USA
 *
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/crypto.h>
#include <linux/vmalloc.h>
#include <linux/mm.h>
#include <linux/lzo.h>
#include <crypto/compress.h>

struct lzo_ctx {
	void *lzo_comp_mem;
};

static void *lzo_alloc_context(struct crypto_ccomp *tfm)
{
	void *ctx;

	ctx = kmalloc(LZO1X_MEM_COMPRESS,
		    GFP_KERNEL | __GFP_NOWARN | __GFP_REPEAT);
	if (!ctx)
		ctx = vmalloc(LZO1X_MEM_COMPRESS);
	if (!ctx)
		return ERR_PTR(-ENOMEM);

	return ctx;
}

static int lzo_init(struct crypto_tfm *tfm)
{
	struct lzo_ctx *ctx = crypto_tfm_ctx(tfm);

	ctx->lzo_comp_mem = lzo_alloc_context(NULL);
	if (IS_ERR(ctx->lzo_comp_mem))
		return -ENOMEM;

	return 0;
}

static void lzo_free_context(struct crypto_ccomp *tfm, void *ctx)
{
	kvfree(ctx);
}

static void lzo_exit(struct crypto_tfm *tfm)
{
	struct lzo_ctx *ctx = crypto_tfm_ctx(tfm);

	lzo_free_context(NULL, ctx->lzo_comp_mem);
}

static int __lzo_compress(const u8 *src, unsigned int slen,
			u8 *dst, unsigned int *dlen, void *ctx)
{
	size_t tmp_len = *dlen; /* size_t(ulong) <-> uint on 64 bit */
	int err;

	err = lzo1x_1_compress(src, slen, dst, &tmp_len, ctx);

	if (err != LZO_E_OK)
		return -EINVAL;

	*dlen = tmp_len;
	return 0;
}

static int lzo_compress(struct crypto_tfm *tfm, const u8 *src,
			    unsigned int slen, u8 *dst, unsigned int *dlen)
{
	struct lzo_ctx *ctx = crypto_tfm_ctx(tfm);

	return __lzo_compress(src, slen, dst, dlen, ctx->lzo_comp_mem);
}

static int __lzo_decompress(const u8 *src, unsigned int slen,
				u8 *dst, unsigned int *dlen, void *ctx)
{
	int err;
	size_t tmp_len = *dlen; /* size_t(ulong) <-> uint on 64 bit */

	err = lzo1x_decompress_safe(src, slen, dst, &tmp_len);

	if (err != LZO_E_OK)
		return -EINVAL;

	*dlen = tmp_len;
	return 0;
}

static int lzo_decompress(struct crypto_tfm *tfm, const u8 *src,
			      unsigned int slen, u8 *dst, unsigned int *dlen)
{
	return __lzo_decompress(src, slen, dst, dlen, NULL);
}

static struct crypto_alg alg = {
	.cra_name		= "lzo",
	.cra_flags		= CRYPTO_ALG_TYPE_COMPRESS,
	.cra_ctxsize		= sizeof(struct lzo_ctx),
	.cra_module		= THIS_MODULE,
	.cra_init		= lzo_init,
	.cra_exit		= lzo_exit,
	.cra_u			= { .compress = {
	.coa_compress 		= lzo_compress,
	.coa_decompress  	= lzo_decompress } }
};

static struct ccomp_alg ccomp = {
	.alloc_context		= lzo_alloc_context,
	.free_context		= lzo_free_context,
	.compress		= __lzo_compress,
	.decompress		= __lzo_decompress,
	.flags			= CCOMP_TYPE_DECOMP_NOCTX,
	.base			= {
		.cra_name	= "lzo",
		.cra_flags	= CRYPTO_ALG_TYPE_CCOMPRESS,
		.cra_module	= THIS_MODULE,
	}
};

static int __init lzo_mod_init(void)
{
	int ret;

	ret = crypto_register_alg(&alg);
	if (ret)
		return ret;

	ret = crypto_register_ccomp(&ccomp);
	if (ret) {
		crypto_unregister_alg(&alg);
		return ret;
	}

	return ret;
}

static void __exit lzo_mod_fini(void)
{
	crypto_unregister_alg(&alg);
	crypto_unregister_ccomp(&ccomp);
}

module_init(lzo_mod_init);
module_exit(lzo_mod_fini);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("LZO Compression Algorithm");
MODULE_ALIAS_CRYPTO("lzo");
