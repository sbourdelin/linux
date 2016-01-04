extern int crypto_init_scomp_ops_async(struct crypto_tfm *tfm);
extern struct acomp_req *
crypto_scomp_acomp_request_alloc(struct crypto_acomp *tfm, gfp_t gfp);
extern void crypto_scomp_acomp_request_free(struct acomp_req *req);
