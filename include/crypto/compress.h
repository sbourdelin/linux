/*
 * Compress: Compression algorithms under the cryptographic API.
 *
 * Copyright 2008 Sony Corporation
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

#ifndef _CRYPTO_COMPRESS_H
#define _CRYPTO_COMPRESS_H

#include <linux/crypto.h>

#define CCOMP_TYPE_DECOMP_NOCTX 0x000000001

struct crypto_ccomp {
	struct crypto_tfm base;
};

struct ccomp_alg {
	void *(*alloc_context)(struct crypto_ccomp *tfm);
	void (*free_context)(struct crypto_ccomp *tfm, void *ctx);
	int (*compress)(const u8 *src, unsigned int slen, u8 *dst,
			unsigned int *dlen, void *ctx);
	int (*decompress)(const u8 *src, unsigned int slen, u8 *dst,
			unsigned int *dlen, void *ctx);

	unsigned long flags;
	struct crypto_alg base;
};

extern struct crypto_ccomp *crypto_alloc_ccomp(const char *alg_name, u32 type,
					       u32 mask);

static inline struct crypto_tfm *crypto_ccomp_tfm(struct crypto_ccomp *tfm)
{
	return &tfm->base;
}

static inline void crypto_free_ccomp(struct crypto_ccomp *tfm)
{
	crypto_destroy_tfm(tfm, crypto_ccomp_tfm(tfm));
}

static inline struct ccomp_alg *__crypto_ccomp_alg(struct crypto_alg *alg)
{
	return container_of(alg, struct ccomp_alg, base);
}

static inline struct ccomp_alg *crypto_ccomp_alg(struct crypto_ccomp *tfm)
{
	return __crypto_ccomp_alg(crypto_ccomp_tfm(tfm)->__crt_alg);
}

static inline void *crypto_ccomp_alloc_context(struct crypto_ccomp *tfm)
{
	return crypto_ccomp_alg(tfm)->alloc_context(tfm);
}

static inline void crypto_ccomp_free_context(struct crypto_ccomp *tfm,
						void *ctx)
{
	return crypto_ccomp_alg(tfm)->free_context(tfm, ctx);
}

static inline int crypto_ccomp_compress(struct crypto_ccomp *tfm,
				const u8 *src, unsigned int slen,
				u8 *dst, unsigned int *dlen, void *ctx)
{
	return crypto_ccomp_alg(tfm)->compress(src, slen, dst, dlen, ctx);
}

static inline int crypto_ccomp_decompress(struct crypto_ccomp *tfm,
				const u8 *src, unsigned int slen,
				u8 *dst, unsigned int *dlen, void *ctx)
{
	return crypto_ccomp_alg(tfm)->decompress(src, slen, dst, dlen, ctx);
}

static inline bool crypto_ccomp_decomp_noctx(struct crypto_ccomp *tfm)
{
	return crypto_ccomp_alg(tfm)->flags & CCOMP_TYPE_DECOMP_NOCTX;
}

extern int crypto_register_ccomp(struct ccomp_alg *alg);
extern int crypto_unregister_ccomp(struct ccomp_alg *alg);
#endif	/* _CRYPTO_COMPRESS_H */
