#ifndef _HWBM_H
#define _HWBM_H

struct hwbm_pool {
	/* Size of the buffers managed */
	int size;
	/* Number of buffers currently used by this pool */
	int buf_num;
	/* constructor called during alocation */
	int (*construct)(struct hwbm_pool *bm_pool, void *buf);
	/* private data */
	void *priv;
};

void hwbm_buf_free(struct hwbm_pool *bm_pool, void *buf);
int hwbm_pool_refill(struct hwbm_pool *bm_pool);
int hwbm_pool_add(struct hwbm_pool *bm_pool, int buf_num);

#endif /* _HWBM_H */
