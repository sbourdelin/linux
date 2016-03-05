#ifndef _HWBM_H
#define _HWBM_H

struct hwbm_pool {
	/* Size of the buffers managed */
	int size;
	/* Number of buffers currently used by this pool */
	int buf_num;
	/* constructor called during alocation */
	int (*construct)(struct hwbm_pool *bm_pool, void *buf);
	/* protect acces to the buffer counter*/
	spinlock_t lock;
	/* private data */
	void *priv;
};

void hwbm_buf_free(struct hwbm_pool *bm_pool, void *buf);
int hwbm_pool_refill(struct hwbm_pool *bm_pool, gfp_t gfp);
int hwbm_pool_add(struct hwbm_pool *bm_pool, unsigned int buf_num, gfp_t gfp);

#endif /* _HWBM_H */
