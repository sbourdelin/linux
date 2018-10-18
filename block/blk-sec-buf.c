/*
 * Sector size level IO buffer allocation helpers for less-than PAGE_SIZE
 * allocation.
 *
 * Controllers may has DMA alignment requirement, meantime filesystem or
 * other upper layer component may allocate IO buffer via slab and submit
 * bio with this buffer directly. Then DMA alignment limit can't be
 * repectected.
 *
 * Create DMA aligned slab, and allocate this less-than PAGE_SIZE IO buffer
 * from the created slab for above users.
 *
 * Copyright (C) 2018 Ming Lei <ming.lei@redhat.com>
 *
 */
#include <linux/kernel.h>
#include <linux/blk-sec-buf.h>

static void __blk_destroy_sec_buf_slabs(struct blk_sec_buf_slabs *slabs)
{
	int i;

	if (!slabs)
		return;

	for (i = 0; i < BLK_NR_SEC_BUF_SLAB; i++)
		kmem_cache_destroy(slabs->slabs[i]);
	kfree(slabs);
}

void blk_destroy_sec_buf_slabs(struct request_queue *q)
{
	mutex_lock(&q->blk_sec_buf_slabs_mutex);
	if (q->sec_buf_slabs && !--q->sec_buf_slabs->ref_cnt) {
		__blk_destroy_sec_buf_slabs(q->sec_buf_slabs);
		q->sec_buf_slabs = NULL;
	}
	mutex_unlock(&q->blk_sec_buf_slabs_mutex);
}
EXPORT_SYMBOL_GPL(blk_destroy_sec_buf_slabs);

int blk_create_sec_buf_slabs(char *name, struct request_queue *q)
{
	struct blk_sec_buf_slabs *slabs;
	char *slab_name;
	int i;
	int nr_slabs = BLK_NR_SEC_BUF_SLAB;
	int ret = -ENOMEM;

	/* No need to create kmem_cache if kmalloc is fine */
	if (!q || queue_dma_alignment(q) < ARCH_KMALLOC_MINALIGN)
		return 0;

	slab_name = kmalloc(strlen(name) + 5, GFP_KERNEL);
	if (!slab_name)
		return ret;

	mutex_lock(&q->blk_sec_buf_slabs_mutex);
	if (q->sec_buf_slabs) {
		q->sec_buf_slabs->ref_cnt++;
		ret = 0;
		goto out;
	}

	slabs = kzalloc(sizeof(*slabs), GFP_KERNEL);
	if (!slabs)
		goto out;

	for (i = 0; i < nr_slabs; i++) {
		int size = (i == nr_slabs - 1) ? PAGE_SIZE - 512 : (i + 1) << 9;

		sprintf(slab_name, "%s-%d", name, i);
		slabs->slabs[i] = kmem_cache_create(slab_name, size,
				queue_dma_alignment(q) + 1,
				SLAB_PANIC, NULL);
		if (!slabs->slabs[i])
			goto fail;
	}

	slabs->ref_cnt = 1;
	q->sec_buf_slabs = slabs;
	ret = 0;
	goto out;

 fail:
	__blk_destroy_sec_buf_slabs(slabs);
 out:
	mutex_unlock(&q->blk_sec_buf_slabs_mutex);
	kfree(slab_name);
	return ret;
}
EXPORT_SYMBOL_GPL(blk_create_sec_buf_slabs);

void *blk_alloc_sec_buf(struct request_queue *q, int size, gfp_t flags)
{
	int i;

	/* We only serve less-than PAGE_SIZE alloction */
	if (size >= PAGE_SIZE)
		return NULL;

	/*
	 * Fallback to kmalloc if no queue is provided, or kmalloc is
	 * enough to respect the queue dma alignment
	 */
	if (!q || queue_dma_alignment(q) < ARCH_KMALLOC_MINALIGN)
		return kmalloc(size, flags);

	if (WARN_ON_ONCE(!q->sec_buf_slabs))
		return NULL;

	i = round_up(size, 512) >> 9;
	i = i < BLK_NR_SEC_BUF_SLAB ? i : BLK_NR_SEC_BUF_SLAB;

	return kmem_cache_alloc(q->sec_buf_slabs->slabs[i - 1], flags);
}
EXPORT_SYMBOL_GPL(blk_alloc_sec_buf);

void blk_free_sec_buf(struct request_queue *q, void *buf, int size)
{
	int i;

	/* We only serve less-than PAGE_SIZE alloction */
	if (size >= PAGE_SIZE)
		return;

	/*
	 * Fallback to kmalloc if no queue is provided, or kmalloc is
	 * enough to respect the queue dma alignment
	 */
	if (!q || queue_dma_alignment(q) < ARCH_KMALLOC_MINALIGN) {
		kfree(buf);
		return;
	}

	if (WARN_ON_ONCE(!q->sec_buf_slabs))
		return;

	i = round_up(size, 512) >> 9;
	i = i < BLK_NR_SEC_BUF_SLAB ? i : BLK_NR_SEC_BUF_SLAB;

	kmem_cache_free(q->sec_buf_slabs->slabs[i - 1], buf);
}
EXPORT_SYMBOL_GPL(blk_free_sec_buf);
