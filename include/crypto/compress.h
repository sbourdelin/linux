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

/**
 * struct acomp_req - asynchronous compression request
 *
 * @base:	Common attributes for async crypto requests
 * @src:	Pointer to memory containing the input scatterlist buffer
 * @dst:	Pointer to memory containing the output scatterlist buffer
 * @src_len:	Length of input buffer
 * @dst_len:	Length of output buffer
 * @out_len:	Number of bytes produced by (de)compressor
 * @__ctx:	Start of private context data
 */
struct acomp_req {
	struct crypto_async_request base;
	struct scatterlist *src;
	struct scatterlist *dst;
	unsigned int src_len;
	unsigned int dst_len;
	unsigned int out_len;
	void *__ctx[] CRYPTO_MINALIGN_ATTR;
};

/**
 * struct crypto_acomp - user-instantiated objects which encapsulate
 * algorithms and core processing logic
 *
 * @compress:	Function performs a compress operation
 * @decompress:	Function performs a de-compress operation
 * @reqsize:	Request size required by algorithm implementation
 * @base:	Common crypto API algorithm data structure
 */
struct crypto_acomp {
	int (*compress)(struct acomp_req *req);
	int (*decompress)(struct acomp_req *req);
	unsigned int reqsize;
	struct crypto_tfm base;
};

/**
 * struct acomp_alg - async compression algorithm
 *
 * @compress:	Function performs a compress operation
 * @decompress:	Function performs a de-compress operation
 * @init:	Initialize the cryptographic transformation object.
 *		This function is used to initialize the cryptographic
 *		transformation object. This function is called only once at
 *		the instantiation time, right after the transformation context
 *		was allocated. In case the cryptographic hardware has some
 *		special requirements which need to be handled by software, this
 *		function shall check for the precise requirement of the
 *		transformation and put any software fallbacks in place.
 * @exit:	Deinitialize the cryptographic transformation object. This is a
 *		counterpart to @init, used to remove various changes set in
 *		@init.
 *
 * @base:	Common crypto API algorithm data structure
 */
struct acomp_alg {
	int (*compress)(struct acomp_req *req);
	int (*decompress)(struct acomp_req *req);
	int (*init)(struct crypto_acomp *tfm);
	void (*exit)(struct crypto_acomp *tfm);
	struct crypto_alg base;
};

/**
 * DOC: Asynchronous Compression API
 *
 * The Asynchronous Compression API is used with the algorithms of type
 * CRYPTO_ALG_TYPE_ACOMPRESS (listed as type "acomp" in /proc/crypto)
 */

/**
 * crypto_alloc_acompress() -- allocate ACOMPRESS tfm handle
 * @alg_name: is the cra_name / name or cra_driver_name / driver name of the
 *	      compression algorithm e.g. "deflate"
 * @type: specifies the type of the algorithm
 * @mask: specifies the mask for the algorithm
 *
 * Allocate a handle for compression algorithm. The returned struct
 * crypto_acomp is the handle that is required for any subsequent
 * API invocation for the compression operations.
 *
 * Return: allocated handle in case of success; IS_ERR() is true in case
 *	   of an error, PTR_ERR() returns the error code.
 */
struct crypto_acomp *crypto_alloc_acomp(const char *alg_name, u32 type,
					u32 mask);

static inline struct crypto_tfm *crypto_acomp_tfm(struct crypto_acomp *tfm)
{
	return &tfm->base;
}

static inline struct crypto_acomp *crypto_acomp_cast(struct crypto_tfm *tfm)
{
	return (struct crypto_acomp *)tfm;
}

static inline void *crypto_acomp_ctx(struct crypto_acomp *tfm)
{
	return crypto_tfm_ctx(crypto_acomp_tfm(tfm));
}

static inline struct acomp_alg *__crypto_acomp_alg(struct crypto_alg *alg)
{
	return container_of(alg, struct acomp_alg, base);
}

static inline struct crypto_acomp *__crypto_acomp_tfm(
	struct crypto_tfm *tfm)
{
	return container_of(tfm, struct crypto_acomp, base);
}

static inline struct acomp_alg *crypto_acomp_alg(
	struct crypto_acomp *tfm)
{
	return __crypto_acomp_alg(crypto_acomp_tfm(tfm)->__crt_alg);
}

static inline unsigned int crypto_acomp_reqsize(struct crypto_acomp *tfm)
{
	return tfm->reqsize;
}

static inline void acomp_request_set_tfm(struct acomp_req *req,
					 struct crypto_acomp *tfm)
{
	req->base.tfm = crypto_acomp_tfm(tfm);
}

static inline struct crypto_acomp *crypto_acomp_reqtfm(
				struct acomp_req *req)
{
	return __crypto_acomp_tfm(req->base.tfm);
}

/**
 * crypto_free_acomp() -- free ACOMPRESS tfm handle
 *
 * @tfm: ACOMPRESS tfm handle allocated with crypto_alloc_acompr()
 */
static inline void crypto_free_acomp(struct crypto_acomp *tfm)
{
	crypto_destroy_tfm(tfm, crypto_acomp_tfm(tfm));
}

static inline int crypto_has_acomp(const char *alg_name, u32 type, u32 mask)
{
	type &= ~CRYPTO_ALG_TYPE_MASK;
	type |= CRYPTO_ALG_TYPE_ACOMPRESS;
	mask |= CRYPTO_ALG_TYPE_MASK;

	return crypto_has_alg(alg_name, type, mask);
}

/**
 * acomp_request_alloc() -- allocates async compress request
 *
 * @tfm:	ACOMPRESS tfm handle allocated with crypto_alloc_acomp()
 * @gfp:	allocation flags
 *
 * Return: allocated handle in case of success or NULL in case of an error.
 */
struct acomp_req *acomp_request_alloc(struct crypto_acomp *acomp,
						gfp_t gfp);

/**
 * acomp_request_free() -- zeroize and free async compress request
 *
 * @req:	request to free
 */
void acomp_request_free(struct acomp_req *acomp);

/**
 * acomp_request_set_callback() -- Sets an asynchronous callback.
 *
 * Callback will be called when an asynchronous operation on a given
 * request is finished.
 *
 * @req:	request that the callback will be set for
 * @flgs:	specify for instance if the operation may backlog
 * @cmlp:	callback which will be called
 * @data:	private data used by the caller
 */
static inline void acomp_request_set_callback(struct acomp_req *req, u32 flgs,
					crypto_completion_t cmpl, void *data)
{
	req->base.complete = cmpl;
	req->base.data = data;
	req->base.flags = flgs;
}

/**
 * acomp_request_set_comp() -- Sets reqest parameters
 *
 * Sets parameters required by acomp operation
 *
 * @req:	async compress request
 * @src:	ptr to input buffer list
 * @dst:	ptr to output buffer list
 * @src_len:	size of the input buffer
 * @dst_len:	size of the output buffer
 * @result:	(de)compression result returned by compressor
 */
static inline void acomp_request_set_comp(struct acomp_req *req,
					  struct scatterlist *src,
					  struct scatterlist *dst,
					  unsigned int src_len,
					  unsigned int dst_len)
{
	req->src = src;
	req->dst = dst;
	req->src_len = src_len;
	req->dst_len = dst_len;
	req->out_len = 0;
}

/**
 * crypto_acomp_compress() -- Invoke async compress operation
 *
 * Function invokes the async compress operation
 *
 * @req:	async compress request
 *
 * Return: zero on success; error code in case of error
 */
static inline int crypto_acomp_compress(struct acomp_req *req)
{
	struct crypto_acomp *tfm = crypto_acomp_reqtfm(req);

	return tfm->compress(req);
}

/**
 * crypto_acomp_decompress() -- Invoke async decompress operation
 *
 * Function invokes the async decompress operation
 *
 * @req:	async compress request
 *
 * Return: zero on success; error code in case of error
 */
static inline int crypto_acomp_decompress(struct acomp_req *req)
{
	struct crypto_acomp *tfm = crypto_acomp_reqtfm(req);

	return tfm->decompress(req);
}

extern int crypto_register_acomp(struct acomp_alg *alg);
extern int crypto_unregister_acomp(struct acomp_alg *alg);
#endif
