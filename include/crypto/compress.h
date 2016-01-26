#ifndef _CRYPTO_COMPRESS_H
#define _CRYPTO_COMPRESS_H
#include <linux/crypto.h>

#define CRYPTO_SCOMP_DECOMP_NOCTX CRYPTO_ALG_PRIVATE

struct crypto_scomp {
	struct crypto_tfm base;
};

struct scomp_alg {
	void *(*alloc_ctx)(struct crypto_scomp *tfm);
	void (*free_ctx)(struct crypto_scomp *tfm, void *ctx);
	int (*compress)(struct crypto_scomp *tfm, const u8 *src,
		unsigned int slen, u8 *dst, unsigned int *dlen, void *ctx);
	int (*decompress)(struct crypto_scomp *tfm, const u8 *src,
		unsigned int slen, u8 *dst, unsigned int *dlen, void *ctx);

	struct crypto_alg base;
};

extern struct crypto_scomp *crypto_alloc_scomp(const char *alg_name, u32 type,
					       u32 mask);

static inline struct crypto_tfm *crypto_scomp_tfm(struct crypto_scomp *tfm)
{
	return &tfm->base;
}

static inline struct crypto_scomp *crypto_scomp_cast(struct crypto_tfm *tfm)
{
	return (struct crypto_scomp *)tfm;
}

static inline void crypto_free_scomp(struct crypto_scomp *tfm)
{
	crypto_destroy_tfm(tfm, crypto_scomp_tfm(tfm));
}

static inline int crypto_has_scomp(const char *alg_name, u32 type, u32 mask)
{
	type &= ~CRYPTO_ALG_TYPE_MASK;
	type |= CRYPTO_ALG_TYPE_SCOMPRESS;
	mask |= CRYPTO_ALG_TYPE_MASK;

	return crypto_has_alg(alg_name, type, mask);
}

static inline struct scomp_alg *__crypto_scomp_alg(struct crypto_alg *alg)
{
	return container_of(alg, struct scomp_alg, base);
}

static inline struct scomp_alg *crypto_scomp_alg(struct crypto_scomp *tfm)
{
	return __crypto_scomp_alg(crypto_scomp_tfm(tfm)->__crt_alg);
}

static inline void *crypto_scomp_alloc_ctx(struct crypto_scomp *tfm)
{
	return crypto_scomp_alg(tfm)->alloc_ctx(tfm);
}

static inline void crypto_scomp_free_ctx(struct crypto_scomp *tfm,
						void *ctx)
{
	return crypto_scomp_alg(tfm)->free_ctx(tfm, ctx);
}

static inline int crypto_scomp_compress(struct crypto_scomp *tfm,
				const u8 *src, unsigned int slen,
				u8 *dst, unsigned int *dlen, void *ctx)
{
	return crypto_scomp_alg(tfm)->compress(tfm, src, slen, dst, dlen, ctx);
}

static inline int crypto_scomp_decompress(struct crypto_scomp *tfm,
				const u8 *src, unsigned int slen,
				u8 *dst, unsigned int *dlen, void *ctx)
{
	return crypto_scomp_alg(tfm)->decompress(tfm, src, slen,
						dst, dlen, ctx);
}

static inline bool crypto_scomp_decomp_noctx(struct crypto_scomp *tfm)
{
	return crypto_scomp_tfm(tfm)->__crt_alg->cra_flags &
				CRYPTO_SCOMP_DECOMP_NOCTX;
}

extern int crypto_register_scomp(struct scomp_alg *alg);
extern int crypto_unregister_scomp(struct scomp_alg *alg);
#endif
