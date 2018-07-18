/*
 * Copyright (C) 2003 Jana Saout <jana@saout.de>
 * Copyright (C) 2004 Clemens Fruhwirth <clemens@endorphin.org>
 * Copyright (C) 2006-2017 Red Hat, Inc. All rights reserved.
 * Copyright (C) 2013-2017 Milan Broz <gmazyland@gmail.com>
 *
 * This file is released under the GPL.
 */

#include <linux/completion.h>
#include <linux/err.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/key.h>
#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/mempool.h>
#include <linux/slab.h>
#include <linux/crypto.h>
#include <linux/workqueue.h>
#include <linux/kthread.h>
#include <linux/backing-dev.h>
#include <linux/atomic.h>
#include <linux/scatterlist.h>
#include <linux/rbtree.h>
#include <linux/ctype.h>
#include <asm/page.h>
#include <asm/unaligned.h>
#include <crypto/hash.h>
#include <crypto/md5.h>
#include <crypto/algapi.h>
#include <crypto/skcipher.h>
#include <crypto/aead.h>
#include <crypto/authenc.h>
#include <crypto/geniv.h>
#include <crypto/internal/aead.h>
#include <crypto/internal/skcipher.h>
#include <linux/rtnetlink.h> /* for struct rtattr and RTA macros only */
#include <keys/user-type.h>
#include <linux/device-mapper.h>
#include <linux/backing-dev.h>
#include <linux/log2.h>

#define DM_MSG_PREFIX "crypt"

struct geniv_ctx;
struct geniv_req_ctx;

/*
 * context holding the current state of a multi-part conversion
 */
struct convert_context {
	struct completion restart;
	struct bio *bio_in;
	struct bio *bio_out;
	struct bvec_iter iter_in;
	struct bvec_iter iter_out;
	sector_t cc_sector;
	atomic_t cc_pending;
	union {
		struct skcipher_request *req;
		struct aead_request *req_aead;
	} r;
};

/*
 * per bio private data
 */
struct dm_crypt_io {
	struct crypt_config *cc;
	struct bio *base_bio;
	u8 *integrity_metadata;
	bool integrity_metadata_from_pool;
	struct work_struct work;

	struct convert_context ctx;

	atomic_t io_pending;
	blk_status_t error;
	sector_t sector;

	struct rb_node rb_node;
} CRYPTO_MINALIGN_ATTR;

struct dm_crypt_request {
	struct convert_context *ctx;
	struct scatterlist *sg_in;
	struct scatterlist *sg_out;
	sector_t iv_sector;
};

struct crypt_config;

/*
 * Crypt: maps a linear range of a block device
 * and encrypts / decrypts at the same time.
 */
enum flags { DM_CRYPT_SUSPENDED, DM_CRYPT_KEY_VALID,
	     DM_CRYPT_SAME_CPU, DM_CRYPT_NO_OFFLOAD };

/*
 * The fields in here must be read only after initialization.
 */
struct crypt_config {
	struct dm_dev *dev;
	sector_t start;

	struct percpu_counter n_allocated_pages;

	struct workqueue_struct *io_queue;
	struct workqueue_struct *crypt_queue;

	wait_queue_head_t write_thread_wait;
	struct task_struct *write_thread;
	struct rb_root write_tree;

	char *cipher_string;
	char *cipher_auth;
	char *key_string;

	sector_t iv_offset;
	unsigned int iv_size;
	unsigned short int sector_size;
	unsigned char sector_shift;

	/* ESSIV: struct crypto_cipher *essiv_tfm */
	void *iv_private;
	union {
		struct crypto_skcipher *tfm;
		struct crypto_aead *tfm_aead;
	} cipher_tfm;
	unsigned int tfms_count;
	unsigned long cipher_flags;

	/*
	 * Layout of each crypto request:
	 *
	 *   struct skcipher_request
	 *      context
	 *      padding
	 *   struct dm_crypt_request
	 *      padding
	 *   IV
	 *
	 * The padding is added so that dm_crypt_request and the IV are
	 * correctly aligned.
	 */
	unsigned int dmreq_start;

	unsigned int per_bio_data_size;

	unsigned long flags;
	unsigned int key_size;
	unsigned int key_parts;      /* independent parts in key buffer */
	unsigned int key_extra_size; /* additional keys length */
	unsigned int key_mac_size;   /* MAC key size for authenc(...) */

	unsigned int integrity_tag_size;
	unsigned int integrity_iv_size;
	unsigned int on_disk_tag_size;

	/*
	 * pool for per bio private data, crypto requests,
	 * encryption requeusts/buffer pages and integrity tags
	 */
	unsigned tag_pool_max_sectors;
	mempool_t tag_pool;
	mempool_t req_pool;
	mempool_t page_pool;

	struct bio_set bs;
	struct mutex bio_alloc_lock;

	u8 key[0];
};

#define MAX_SG_LIST     (BIO_MAX_PAGES * 8)
#define MIN_IOS         64
#define MAX_TAG_SIZE    480
#define POOL_ENTRY_SIZE 512

static void clone_init(struct dm_crypt_io *, struct bio *);
static void kcryptd_queue_crypt(struct dm_crypt_io *io);

static DEFINE_SPINLOCK(dm_crypt_clients_lock);
static unsigned dm_crypt_clients_n = 0;
static volatile unsigned long dm_crypt_pages_per_client;
#define DM_CRYPT_MEMORY_PERCENT			2
#define DM_CRYPT_MIN_PAGES_PER_CLIENT		(BIO_MAX_PAGES * 16)

static void clone_init(struct dm_crypt_io *, struct bio *);
static void kcryptd_queue_crypt(struct dm_crypt_io *io);

/*
 * Use this to access cipher attributes that are independent of the key.
 */
static struct crypto_skcipher *any_tfm(struct crypt_config *cc)
{
	return cc->cipher_tfm.tfm;
}

static struct crypto_aead *any_tfm_aead(struct crypt_config *cc)
{
	return cc->cipher_tfm.tfm_aead;
}

/*
 * Integrity extensions
 */
static bool crypt_integrity_aead(struct crypt_config *cc)
{
	return test_bit(CRYPT_MODE_INTEGRITY_AEAD, &cc->cipher_flags);
}

static int dm_crypt_integrity_io_alloc(struct dm_crypt_io *io, struct bio *bio)
{
	struct bio_integrity_payload *bip;
	unsigned int tag_len;
	int ret;

	if (!bio_sectors(bio) || !io->cc->on_disk_tag_size)
		return 0;

	bip = bio_integrity_alloc(bio, GFP_NOIO, 1);
	if (IS_ERR(bip))
		return PTR_ERR(bip);

	tag_len = io->cc->on_disk_tag_size * bio_sectors(bio);

	bip->bip_iter.bi_size = tag_len;
	bip->bip_iter.bi_sector = io->cc->start + io->sector;

	ret = bio_integrity_add_page(bio, virt_to_page(io->integrity_metadata),
				     tag_len, offset_in_page(io->integrity_metadata));
	if (unlikely(ret != tag_len))
		return -ENOMEM;

	return 0;
}

static int crypt_integrity_ctr(struct crypt_config *cc, struct dm_target *ti)
{
#ifdef CONFIG_BLK_DEV_INTEGRITY
	struct blk_integrity *bi = blk_get_integrity(cc->dev->bdev->bd_disk);

	/* From now we require underlying device with our integrity profile */
	if (!bi || strcasecmp(bi->profile->name, "DM-DIF-EXT-TAG")) {
		ti->error = "Integrity profile not supported.";
		return -EINVAL;
	}

	if (bi->tag_size != cc->on_disk_tag_size ||
	    bi->tuple_size != cc->on_disk_tag_size) {
		ti->error = "Integrity profile tag size mismatch.";
		return -EINVAL;
	}
	if (1 << bi->interval_exp != cc->sector_size) {
		ti->error = "Integrity profile sector size mismatch.";
		return -EINVAL;
	}

	if (crypt_integrity_aead(cc)) {
		cc->integrity_tag_size = cc->on_disk_tag_size - cc->integrity_iv_size;
		DMINFO("Integrity AEAD, tag size %u, IV size %u.",
		       cc->integrity_tag_size, cc->integrity_iv_size);

		if (crypto_aead_setauthsize(any_tfm_aead(cc), cc->integrity_tag_size)) {
			ti->error = "Integrity AEAD auth tag size is not supported.";
			return -EINVAL;
		}
	} else if (cc->integrity_iv_size)
		DMINFO("Additional per-sector space %u bytes for IV.",
		       cc->integrity_iv_size);

	if ((cc->integrity_tag_size + cc->integrity_iv_size) != bi->tag_size) {
		ti->error = "Not enough space for integrity tag in the profile.";
		return -EINVAL;
	}

	return 0;
#else
	ti->error = "Integrity profile not supported.";
	return -EINVAL;
#endif
}

static void crypt_convert_init(struct crypt_config *cc,
			       struct convert_context *ctx,
			       struct bio *bio_out, struct bio *bio_in,
			       sector_t sector)
{
	ctx->bio_in = bio_in;
	ctx->bio_out = bio_out;
	if (bio_in)
		ctx->iter_in = bio_in->bi_iter;
	if (bio_out)
		ctx->iter_out = bio_out->bi_iter;
	ctx->cc_sector = sector + cc->iv_offset;
	init_completion(&ctx->restart);
}

static struct dm_crypt_request *dmreq_of_req(struct crypt_config *cc,
					     void *req)
{
	return (struct dm_crypt_request *)((char *)req + cc->dmreq_start);
}

static void *req_of_dmreq(struct crypt_config *cc, struct dm_crypt_request *dmreq)
{
	return (void *)((char *)dmreq - cc->dmreq_start);
}


static void kcryptd_async_done(struct crypto_async_request *async_req,
			       int error);

static void crypt_alloc_req_skcipher(struct crypt_config *cc,
				     struct convert_context *ctx)
{
	if (!ctx->r.req)
		ctx->r.req = mempool_alloc(&cc->req_pool, GFP_NOIO);

	skcipher_request_set_tfm(ctx->r.req, cc->cipher_tfm.tfm);

	/*
	 * Use REQ_MAY_BACKLOG so a cipher driver internally backlogs
	 * requests if driver request queue is full.
	 */
	skcipher_request_set_callback(ctx->r.req,
	    CRYPTO_TFM_REQ_MAY_BACKLOG | CRYPTO_TFM_REQ_MAY_SLEEP,
	    kcryptd_async_done, dmreq_of_req(cc, ctx->r.req));
}

static void crypt_alloc_req_aead(struct crypt_config *cc,
				 struct convert_context *ctx)
{
	if (!ctx->r.req_aead)
		ctx->r.req_aead = mempool_alloc(&cc->req_pool, GFP_NOIO);

	aead_request_set_tfm(ctx->r.req_aead, cc->cipher_tfm.tfm_aead);

	/*
	 * Use REQ_MAY_BACKLOG so a cipher driver internally backlogs
	 * requests if driver request queue is full.
	 */
	aead_request_set_callback(ctx->r.req_aead,
	    CRYPTO_TFM_REQ_MAY_BACKLOG | CRYPTO_TFM_REQ_MAY_SLEEP,
	    kcryptd_async_done, dmreq_of_req(cc, ctx->r.req_aead));
}

static void crypt_alloc_req(struct crypt_config *cc,
			    struct convert_context *ctx)
{
	if (crypt_integrity_aead(cc))
		crypt_alloc_req_aead(cc, ctx);
	else
		crypt_alloc_req_skcipher(cc, ctx);
}

static void crypt_free_req_skcipher(struct crypt_config *cc,
				    struct skcipher_request *req, struct bio *base_bio)
{
	struct dm_crypt_io *io = dm_per_bio_data(base_bio, cc->per_bio_data_size);

	if ((struct skcipher_request *)(io + 1) != req)
		mempool_free(req, &cc->req_pool);
}

static void crypt_free_req_aead(struct crypt_config *cc,
				struct aead_request *req, struct bio *base_bio)
{
	struct dm_crypt_io *io = dm_per_bio_data(base_bio, cc->per_bio_data_size);

	if ((struct aead_request *)(io + 1) != req)
		mempool_free(req, &cc->req_pool);
}

static void crypt_free_req(struct crypt_config *cc, void *req, struct bio *base_bio)
{
	if (crypt_integrity_aead(cc))
		crypt_free_req_aead(cc, req, base_bio);
	else
		crypt_free_req_skcipher(cc, req, base_bio);
}

/*
 * Encrypt / decrypt data from one bio to another one (can be the same one)
 */
static blk_status_t crypt_convert_bio(struct crypt_config *cc,
					struct convert_context *ctx)
{
	unsigned int cryptlen, n1, n2, nents, i = 0, bytes = 0;
	struct skcipher_request *req = NULL;
	struct aead_request *req_aead = NULL;
	struct dm_crypt_request *dmreq;
	struct dm_crypt_io *io = container_of(ctx, struct dm_crypt_io, ctx);
	struct geniv_req_info rinfo;
	struct bio_vec bv_in, bv_out;
	int r;

	atomic_set(&ctx->cc_pending, 1);
	crypt_alloc_req(cc, ctx);

	if (crypt_integrity_aead(cc)) {
		req_aead = ctx->r.req_aead;
		dmreq = dmreq_of_req(cc, req_aead);
	} else {
		req = ctx->r.req;
		dmreq = dmreq_of_req(cc, req);
	}

	n1 = bio_segments(ctx->bio_in);
	n2 = bio_segments(ctx->bio_out);
	nents = n1 > n2 ? n1 : n2;
	nents = nents > MAX_SG_LIST ? MAX_SG_LIST : nents;
	cryptlen = ctx->iter_in.bi_size;

	DMDEBUG("dm-crypt:%s: segments:[in=%u, out=%u] bi_size=%u\n",
		bio_data_dir(ctx->bio_in) == WRITE ? "write" : "read",
		n1, n2, cryptlen);

	dmreq->sg_in = kcalloc(nents, sizeof(struct scatterlist), GFP_KERNEL);
	dmreq->sg_out = kcalloc(nents, sizeof(struct scatterlist), GFP_KERNEL);
	if (!dmreq->sg_in || !dmreq->sg_out) {
		DMERR("dm-crypt: Failed to allocate scatterlist\n");
		r = -ENOMEM;
		goto end;
	}
	dmreq->ctx = ctx;

	sg_init_table(dmreq->sg_in, nents);
	sg_init_table(dmreq->sg_out, nents);

	while (ctx->iter_in.bi_size && ctx->iter_out.bi_size && i < nents) {
		bv_in = bio_iter_iovec(ctx->bio_in, ctx->iter_in);
		bv_out = bio_iter_iovec(ctx->bio_out, ctx->iter_out);

		sg_set_page(&dmreq->sg_in[i], bv_in.bv_page, bv_in.bv_len,
				bv_in.bv_offset);
		sg_set_page(&dmreq->sg_out[i], bv_out.bv_page, bv_out.bv_len,
				bv_out.bv_offset);

		bio_advance_iter(ctx->bio_in, &ctx->iter_in, bv_in.bv_len);
		bio_advance_iter(ctx->bio_out, &ctx->iter_out, bv_out.bv_len);

		bytes += bv_in.bv_len;
		i++;
	}

	DMDEBUG("dm-crypt: Processed %u of %u bytes\n", bytes, cryptlen);

	rinfo.cc_sector = ctx->cc_sector;
	rinfo.nents = nents;
	rinfo.integrity_metadata = io->integrity_metadata;

	atomic_inc(&ctx->cc_pending);
	if (crypt_integrity_aead(cc)) {
		aead_request_set_crypt(req_aead, dmreq->sg_in, dmreq->sg_out,
					bytes, (u8 *)&rinfo);
		if (bio_data_dir(ctx->bio_in) == WRITE)
			r = crypto_aead_encrypt(req_aead);
		else
			r = crypto_aead_decrypt(req_aead);
	} else {
		skcipher_request_set_crypt(req, dmreq->sg_in, dmreq->sg_out,
					bytes, (u8 *)&rinfo);
		if (bio_data_dir(ctx->bio_in) == WRITE)
			r = crypto_skcipher_encrypt(req);
		else
			r = crypto_skcipher_decrypt(req);
	}

	switch (r) {
	/* The request was queued so wait. */
	case -EBUSY:
		wait_for_completion(&ctx->restart);
		reinit_completion(&ctx->restart);
		/* fall through */
	/*
	 * The request is queued and processed asynchronously,
	 * completion function kcryptd_async_done() is called.
	 */
	case -EINPROGRESS:
		ctx->r.req = NULL;
		cond_resched();
		break;
	/*
	 * The requeest was already processed (synchronously).
	 */
	case 0:
		atomic_dec(&ctx->cc_pending);
		break;
	/*
	 * There was a data integrity error.
	 */
	case -EBADMSG:
		atomic_dec(&ctx->cc_pending);
		return BLK_STS_PROTECTION;
	/*
	 * There was an error while processing the request.
	 */
	default:
		atomic_dec(&ctx->cc_pending);
		return BLK_STS_IOERR;
	}
end:
	return 0;
}

static void crypt_free_buffer_pages(struct crypt_config *cc, struct bio *clone);

/*
 * Generate a new unfragmented bio with the given size
 * This should never violate the device limitations (but only because
 * max_segment_size is being constrained to PAGE_SIZE).
 *
 * This function may be called concurrently. If we allocate from the mempool
 * concurrently, there is a possibility of deadlock. For example, if we have
 * mempool of 256 pages, two processes, each wanting 256, pages allocate from
 * the mempool concurrently, it may deadlock in a situation where both processes
 * have allocated 128 pages and the mempool is exhausted.
 *
 * In order to avoid this scenario we allocate the pages under a mutex.
 *
 * In order to not degrade performance with excessive locking, we try
 * non-blocking allocations without a mutex first but on failure we fallback
 * to blocking allocations with a mutex.
 */
static struct bio *crypt_alloc_buffer(struct dm_crypt_io *io, unsigned size)
{
	struct crypt_config *cc = io->cc;
	struct bio *clone;
	unsigned int nr_iovecs = (size + PAGE_SIZE - 1) >> PAGE_SHIFT;
	gfp_t gfp_mask = GFP_NOWAIT | __GFP_HIGHMEM;
	unsigned i, len, remaining_size;
	struct page *page;

retry:
	if (unlikely(gfp_mask & __GFP_DIRECT_RECLAIM))
		mutex_lock(&cc->bio_alloc_lock);

	clone = bio_alloc_bioset(GFP_NOIO, nr_iovecs, &cc->bs);
	if (!clone)
		goto out;

	clone_init(io, clone);

	remaining_size = size;

	for (i = 0; i < nr_iovecs; i++) {
		page = mempool_alloc(&cc->page_pool, gfp_mask);
		if (!page) {
			crypt_free_buffer_pages(cc, clone);
			bio_put(clone);
			gfp_mask |= __GFP_DIRECT_RECLAIM;
			goto retry;
		}

		len = (remaining_size > PAGE_SIZE) ? PAGE_SIZE : remaining_size;

		bio_add_page(clone, page, len, 0);

		remaining_size -= len;
	}

	/* Allocate space for integrity tags */
	if (dm_crypt_integrity_io_alloc(io, clone)) {
		crypt_free_buffer_pages(cc, clone);
		bio_put(clone);
		clone = NULL;
	}
out:
	if (unlikely(gfp_mask & __GFP_DIRECT_RECLAIM))
		mutex_unlock(&cc->bio_alloc_lock);

	return clone;
}

static void crypt_free_buffer_pages(struct crypt_config *cc, struct bio *clone)
{
	unsigned int i;
	struct bio_vec *bv;

	bio_for_each_segment_all(bv, clone, i) {
		BUG_ON(!bv->bv_page);
		mempool_free(bv->bv_page, &cc->page_pool);
	}
}

static void crypt_io_init(struct dm_crypt_io *io, struct crypt_config *cc,
			  struct bio *bio, sector_t sector)
{
	io->cc = cc;
	io->base_bio = bio;
	io->sector = sector;
	io->error = 0;
	io->ctx.r.req = NULL;
	io->integrity_metadata = NULL;
	io->integrity_metadata_from_pool = false;
	atomic_set(&io->io_pending, 0);
}

static void crypt_inc_pending(struct dm_crypt_io *io)
{
	atomic_inc(&io->io_pending);
}

/*
 * One of the bios was finished. Check for completion of
 * the whole request and correctly clean up the buffer.
 */
static void crypt_dec_pending(struct dm_crypt_io *io)
{
	struct crypt_config *cc = io->cc;
	struct bio *base_bio = io->base_bio;
	struct dm_crypt_request *dmreq;
	blk_status_t error = io->error;

	if (!atomic_dec_and_test(&io->io_pending))
		return;

	if (io->ctx.r.req) {
		crypt_free_req(cc, io->ctx.r.req, base_bio);

		if (crypt_integrity_aead(cc))
			dmreq = dmreq_of_req(cc, io->ctx.r.req_aead);
		else
			dmreq = dmreq_of_req(cc, io->ctx.r.req);
		DMDEBUG("dm-crypt: Freeing scatterlists [sync]\n");
		kfree(dmreq->sg_in);
		kfree(dmreq->sg_out);
	}

	if (unlikely(io->integrity_metadata_from_pool))
		mempool_free(io->integrity_metadata, &io->cc->tag_pool);
	else
		kfree(io->integrity_metadata);

	base_bio->bi_status = error;
	bio_endio(base_bio);
}

/*
 * kcryptd/kcryptd_io:
 *
 * Needed because it would be very unwise to do decryption in an
 * interrupt context.
 *
 * kcryptd performs the actual encryption or decryption.
 *
 * kcryptd_io performs the IO submission.
 *
 * They must be separated as otherwise the final stages could be
 * starved by new requests which can block in the first stages due
 * to memory allocation.
 *
 * The work is done per CPU global for all dm-crypt instances.
 * They should not depend on each other and do not block.
 */
static void crypt_endio(struct bio *clone)
{
	struct dm_crypt_io *io = clone->bi_private;
	struct crypt_config *cc = io->cc;
	unsigned rw = bio_data_dir(clone);
	blk_status_t error;

	/*
	 * free the processed pages
	 */
	if (rw == WRITE)
		crypt_free_buffer_pages(cc, clone);

	error = clone->bi_status;
	bio_put(clone);

	if (rw == READ && !error) {
		kcryptd_queue_crypt(io);
		return;
	}

	if (unlikely(error))
		io->error = error;

	crypt_dec_pending(io);
}

static void clone_init(struct dm_crypt_io *io, struct bio *clone)
{
	struct crypt_config *cc = io->cc;

	clone->bi_private = io;
	clone->bi_end_io  = crypt_endio;
	bio_set_dev(clone, cc->dev->bdev);
	clone->bi_opf	  = io->base_bio->bi_opf;
}

static int kcryptd_io_read(struct dm_crypt_io *io, gfp_t gfp)
{
	struct crypt_config *cc = io->cc;
	struct bio *clone;

	/*
	 * We need the original biovec array in order to decrypt
	 * the whole bio data *afterwards* -- thanks to immutable
	 * biovecs we don't need to worry about the block layer
	 * modifying the biovec array; so leverage bio_clone_fast().
	 */
	clone = bio_clone_fast(io->base_bio, gfp, &cc->bs);
	if (!clone)
		return 1;

	crypt_inc_pending(io);

	clone_init(io, clone);
	clone->bi_iter.bi_sector = cc->start + io->sector;

	if (dm_crypt_integrity_io_alloc(io, clone)) {
		crypt_dec_pending(io);
		bio_put(clone);
		return 1;
	}

	generic_make_request(clone);
	return 0;
}

static void kcryptd_io_read_work(struct work_struct *work)
{
	struct dm_crypt_io *io = container_of(work, struct dm_crypt_io, work);

	crypt_inc_pending(io);
	if (kcryptd_io_read(io, GFP_NOIO))
		io->error = BLK_STS_RESOURCE;
	crypt_dec_pending(io);
}

static void kcryptd_queue_read(struct dm_crypt_io *io)
{
	struct crypt_config *cc = io->cc;

	INIT_WORK(&io->work, kcryptd_io_read_work);
	queue_work(cc->io_queue, &io->work);
}

static void kcryptd_io_write(struct dm_crypt_io *io)
{
	struct bio *clone = io->ctx.bio_out;

	generic_make_request(clone);
}

#define crypt_io_from_node(node) rb_entry((node), struct dm_crypt_io, rb_node)

static int dmcrypt_write(void *data)
{
	struct crypt_config *cc = data;
	struct dm_crypt_io *io;

	while (1) {
		struct rb_root write_tree;
		struct blk_plug plug;

		DECLARE_WAITQUEUE(wait, current);

		spin_lock_irq(&cc->write_thread_wait.lock);
continue_locked:

		if (!RB_EMPTY_ROOT(&cc->write_tree))
			goto pop_from_list;

		set_current_state(TASK_INTERRUPTIBLE);
		__add_wait_queue(&cc->write_thread_wait, &wait);

		spin_unlock_irq(&cc->write_thread_wait.lock);

		if (unlikely(kthread_should_stop())) {
			set_current_state(TASK_RUNNING);
			remove_wait_queue(&cc->write_thread_wait, &wait);
			break;
		}

		schedule();

		set_current_state(TASK_RUNNING);
		spin_lock_irq(&cc->write_thread_wait.lock);
		__remove_wait_queue(&cc->write_thread_wait, &wait);
		goto continue_locked;

pop_from_list:
		write_tree = cc->write_tree;
		cc->write_tree = RB_ROOT;
		spin_unlock_irq(&cc->write_thread_wait.lock);

		BUG_ON(rb_parent(write_tree.rb_node));

		/*
		 * Note: we cannot walk the tree here with rb_next because
		 * the structures may be freed when kcryptd_io_write is called.
		 */
		blk_start_plug(&plug);
		do {
			io = crypt_io_from_node(rb_first(&write_tree));
			rb_erase(&io->rb_node, &write_tree);
			kcryptd_io_write(io);
		} while (!RB_EMPTY_ROOT(&write_tree));
		blk_finish_plug(&plug);
	}
	return 0;
}

static void kcryptd_crypt_write_io_submit(struct dm_crypt_io *io, int async)
{
	struct bio *clone = io->ctx.bio_out;
	struct crypt_config *cc = io->cc;
	unsigned long flags;
	sector_t sector;
	struct rb_node **rbp, *parent;

	if (unlikely(io->error)) {
		crypt_free_buffer_pages(cc, clone);
		bio_put(clone);
		crypt_dec_pending(io);
		return;
	}

	/* crypt_convert should have filled the clone bio */
	BUG_ON(io->ctx.iter_out.bi_size);

	clone->bi_iter.bi_sector = cc->start + io->sector;

	if (likely(!async) && test_bit(DM_CRYPT_NO_OFFLOAD, &cc->flags)) {
		generic_make_request(clone);
		return;
	}

	spin_lock_irqsave(&cc->write_thread_wait.lock, flags);
	rbp = &cc->write_tree.rb_node;
	parent = NULL;
	sector = io->sector;
	while (*rbp) {
		parent = *rbp;
		if (sector < crypt_io_from_node(parent)->sector)
			rbp = &(*rbp)->rb_left;
		else
			rbp = &(*rbp)->rb_right;
	}
	rb_link_node(&io->rb_node, parent, rbp);
	rb_insert_color(&io->rb_node, &cc->write_tree);

	wake_up_locked(&cc->write_thread_wait);
	spin_unlock_irqrestore(&cc->write_thread_wait.lock, flags);
}

static void kcryptd_crypt_write_convert(struct dm_crypt_io *io)
{
	struct crypt_config *cc = io->cc;
	struct bio *clone;
	int crypt_finished;
	sector_t sector = io->sector;
	blk_status_t r;

	/*
	 * Prevent io from disappearing until this function completes.
	 */
	crypt_inc_pending(io);
	crypt_convert_init(cc, &io->ctx, NULL, io->base_bio, sector);

	clone = crypt_alloc_buffer(io, io->base_bio->bi_iter.bi_size);
	if (unlikely(!clone)) {
		io->error = BLK_STS_IOERR;
		goto dec;
	}

	io->ctx.bio_out = clone;
	io->ctx.iter_out = clone->bi_iter;

	sector += bio_sectors(clone);

	crypt_inc_pending(io);
	r = crypt_convert_bio(cc, &io->ctx);
	if (r)
		io->error = r;
	crypt_finished = atomic_dec_and_test(&io->ctx.cc_pending);

	/* Encryption was already finished, submit io now */
	if (crypt_finished) {
		kcryptd_crypt_write_io_submit(io, 0);
		io->sector = sector;
	}

dec:
	crypt_dec_pending(io);
}

static void kcryptd_crypt_read_done(struct dm_crypt_io *io)
{
	crypt_dec_pending(io);
}

static void kcryptd_crypt_read_convert(struct dm_crypt_io *io)
{
	struct crypt_config *cc = io->cc;
	blk_status_t r;

	crypt_inc_pending(io);

	crypt_convert_init(cc, &io->ctx, io->base_bio, io->base_bio,
			   io->sector);

	r = crypt_convert_bio(cc, &io->ctx);
	if (r)
		io->error = r;

	if (atomic_dec_and_test(&io->ctx.cc_pending))
		kcryptd_crypt_read_done(io);

	crypt_dec_pending(io);
}

static void kcryptd_async_done(struct crypto_async_request *async_req,
			       int error)
{
	struct dm_crypt_request *dmreq = async_req->data;
	struct convert_context *ctx = dmreq->ctx;
	struct dm_crypt_io *io = container_of(ctx, struct dm_crypt_io, ctx);
	struct crypt_config *cc = io->cc;

	/*
	 * A request from crypto driver backlog is going to be processed now,
	 * finish the completion and continue in crypt_convert().
	 * (Callback will be called for the second time for this request.)
	 */
	if (error == -EINPROGRESS) {
		complete(&ctx->restart);
		return;
	}

	if (error == -EBADMSG) {
		DMERR("INTEGRITY AEAD ERROR\n");
		io->error = BLK_STS_PROTECTION;
	} else if (error < 0)
		io->error = BLK_STS_IOERR;

	DMDEBUG("dm-crypt: Freeing scatterlists and request struct [async]\n");
	kfree(dmreq->sg_in);
	kfree(dmreq->sg_out);

	crypt_free_req(cc, req_of_dmreq(cc, dmreq), io->base_bio);

	if (!atomic_dec_and_test(&ctx->cc_pending))
		return;

	if (bio_data_dir(io->base_bio) == READ)
		kcryptd_crypt_read_done(io);
	else
		kcryptd_crypt_write_io_submit(io, 1);
}

static void kcryptd_crypt(struct work_struct *work)
{
	struct dm_crypt_io *io = container_of(work, struct dm_crypt_io, work);

	if (bio_data_dir(io->base_bio) == READ)
		kcryptd_crypt_read_convert(io);
	else
		kcryptd_crypt_write_convert(io);
}

static void kcryptd_queue_crypt(struct dm_crypt_io *io)
{
	struct crypt_config *cc = io->cc;

	INIT_WORK(&io->work, kcryptd_crypt);
	queue_work(cc->crypt_queue, &io->work);
}

static void crypt_free_tfm(struct crypt_config *cc)
{
	if (crypt_integrity_aead(cc)) {
		if (!cc->cipher_tfm.tfm_aead)
			return;
		if (cc->cipher_tfm.tfm_aead && !IS_ERR(cc->cipher_tfm.tfm_aead)) {
			crypto_free_aead(cc->cipher_tfm.tfm_aead);
			cc->cipher_tfm.tfm_aead = NULL;
		}
	} else {
		if (!cc->cipher_tfm.tfm)
			return;
		if (cc->cipher_tfm.tfm && !IS_ERR(cc->cipher_tfm.tfm)) {
			crypto_free_skcipher(cc->cipher_tfm.tfm);
			cc->cipher_tfm.tfm = NULL;
		}
	}
}

static int crypt_alloc_tfm(struct crypt_config *cc, char *ciphermode)
{
	int err;

	if (crypt_integrity_aead(cc)) {
		cc->cipher_tfm.tfm_aead = crypto_alloc_aead(ciphermode, 0, 0);
		if (IS_ERR(cc->cipher_tfm.tfm_aead)) {
			err = PTR_ERR(cc->cipher_tfm.tfm_aead);
			crypt_free_tfm(cc);
			return err;
		}
	} else {
		cc->cipher_tfm.tfm = crypto_alloc_skcipher(ciphermode, 0, 0);
		if (IS_ERR(cc->cipher_tfm.tfm)) {
			err = PTR_ERR(cc->cipher_tfm.tfm);
			crypt_free_tfm(cc);
			return err;
		}
	}

	return 0;
}

static void init_key_info(struct crypt_config *cc, enum setkey_op keyop,
			char *ivopts, struct geniv_key_info *kinfo)
{
	kinfo->keyop = keyop;
	kinfo->tfms_count = cc->tfms_count;
	kinfo->key = cc->key;
	kinfo->cipher_flags = cc->cipher_flags;
	kinfo->ivopts = ivopts;
	kinfo->iv_offset = cc->iv_offset;
	kinfo->sector_size = cc->sector_size;
	kinfo->key_size = cc->key_size;
	kinfo->key_parts = cc->key_parts;
	kinfo->key_mac_size = cc->key_mac_size;
	kinfo->on_disk_tag_size = cc->on_disk_tag_size;
}

static int crypt_setkey(struct crypt_config *cc, enum setkey_op keyop,
			char *ivopts)
{
	int r = 0;
	struct geniv_key_info kinfo;

	init_key_info(cc, keyop, ivopts, &kinfo);

	if (crypt_integrity_aead(cc))
		r = crypto_aead_setkey(cc->cipher_tfm.tfm_aead, (u8 *)&kinfo, sizeof(kinfo));
	else
		r = crypto_skcipher_setkey(cc->cipher_tfm.tfm, (u8 *)&kinfo, sizeof(kinfo));

	return r;
}

#ifdef CONFIG_KEYS

static bool contains_whitespace(const char *str)
{
	while (*str)
		if (isspace(*str++))
			return true;
	return false;
}

static int crypt_set_keyring_key(struct crypt_config *cc,
				const char *key_string,
				enum setkey_op keyop, char *ivopts)
{
	char *new_key_string, *key_desc;
	int ret;
	struct key *key;
	const struct user_key_payload *ukp;

	/*
	 * Reject key_string with whitespace. dm core currently lacks code for
	 * proper whitespace escaping in arguments on DM_TABLE_STATUS path.
	 */
	if (contains_whitespace(key_string)) {
		DMERR("whitespace chars not allowed in key string");
		return -EINVAL;
	}

	/* look for next ':' separating key_type from key_description */
	key_desc = strpbrk(key_string, ":");
	if (!key_desc || key_desc == key_string || !strlen(key_desc + 1))
		return -EINVAL;

	if (strncmp(key_string, "logon:", key_desc - key_string + 1) &&
	    strncmp(key_string, "user:", key_desc - key_string + 1))
		return -EINVAL;

	new_key_string = kstrdup(key_string, GFP_KERNEL);
	if (!new_key_string)
		return -ENOMEM;

	key = request_key(key_string[0] == 'l' ? &key_type_logon : &key_type_user,
			  key_desc + 1, NULL);
	if (IS_ERR(key)) {
		kzfree(new_key_string);
		return PTR_ERR(key);
	}

	down_read(&key->sem);

	ukp = user_key_payload_locked(key);
	if (!ukp) {
		up_read(&key->sem);
		key_put(key);
		kzfree(new_key_string);
		return -EKEYREVOKED;
	}

	if (cc->key_size != ukp->datalen) {
		up_read(&key->sem);
		key_put(key);
		kzfree(new_key_string);
		return -EINVAL;
	}

	memcpy(cc->key, ukp->data, cc->key_size);

	up_read(&key->sem);
	key_put(key);

	/* clear the flag since following operations may invalidate previously valid key */
	clear_bit(DM_CRYPT_KEY_VALID, &cc->flags);

	ret = crypt_setkey(cc, keyop, ivopts);

	if (!ret) {
		set_bit(DM_CRYPT_KEY_VALID, &cc->flags);
		kzfree(cc->key_string);
		cc->key_string = new_key_string;
	} else
		kzfree(new_key_string);

	return ret;
}

static int get_key_size(char **key_string)
{
	char *colon, dummy;
	int ret;

	if (*key_string[0] != ':')
		return strlen(*key_string) >> 1;

	/* look for next ':' in key string */
	colon = strpbrk(*key_string + 1, ":");
	if (!colon)
		return -EINVAL;

	if (sscanf(*key_string + 1, "%u%c", &ret, &dummy) != 2 || dummy != ':')
		return -EINVAL;

	*key_string = colon;

	/* remaining key string should be :<logon|user>:<key_desc> */

	return ret;
}

#else

static int crypt_set_keyring_key(struct crypt_config *cc,
				const char *key_string,
				enum setkey_op keyop, char *ivopts)
{
	return -EINVAL;
}

static int get_key_size(char **key_string)
{
	return (*key_string[0] == ':') ? -EINVAL : strlen(*key_string) >> 1;
}

#endif

static int crypt_set_key(struct crypt_config *cc, enum setkey_op keyop,
			char *key, char *ivopts)
{
	int r = -EINVAL;
	int key_string_len = strlen(key);

	/* Hyphen (which gives a key_size of zero) means there is no key. */
	if (!cc->key_size && strcmp(key, "-"))
		goto out;

	/* ':' means the key is in kernel keyring, short-circuit normal key processing */
	if (key[0] == ':') {
		r = crypt_set_keyring_key(cc, key + 1, keyop, ivopts);
		goto out;
	}

	/* clear the flag since following operations may invalidate previously valid key */
	clear_bit(DM_CRYPT_KEY_VALID, &cc->flags);

	/* wipe references to any kernel keyring key */
	kzfree(cc->key_string);
	cc->key_string = NULL;

	/* Decode key from its hex representation. */
	if (cc->key_size && hex2bin(cc->key, key, cc->key_size) < 0)
		goto out;

	r = crypt_setkey(cc, keyop, ivopts);
	if (!r)
		set_bit(DM_CRYPT_KEY_VALID, &cc->flags);

out:
	/* Hex key string not needed after here, so wipe it. */
	memset(key, '0', key_string_len);

	return r;
}

static int crypt_init_key(struct dm_target *ti, char *key, char *ivopts)
{
	struct crypt_config *cc = ti->private;
	int ret;

	ret = crypt_set_key(cc, SETKEY_OP_INIT, key, ivopts);
	if (ret < 0)
		ti->error = "Error decoding and setting key";
	return ret;
}

static int crypt_wipe_key(struct crypt_config *cc)
{
	int r;

	clear_bit(DM_CRYPT_KEY_VALID, &cc->flags);
	get_random_bytes(&cc->key, cc->key_size);
	kzfree(cc->key_string);
	cc->key_string = NULL;
	r = crypt_setkey(cc, SETKEY_OP_WIPE, NULL);
	memset(&cc->key, 0, cc->key_size * sizeof(u8));

	return r;
}

static void crypt_calculate_pages_per_client(void)
{
	unsigned long pages = (totalram_pages - totalhigh_pages) * DM_CRYPT_MEMORY_PERCENT / 100;

	if (!dm_crypt_clients_n)
		return;

	pages /= dm_crypt_clients_n;
	if (pages < DM_CRYPT_MIN_PAGES_PER_CLIENT)
		pages = DM_CRYPT_MIN_PAGES_PER_CLIENT;
	dm_crypt_pages_per_client = pages;
}

static void *crypt_page_alloc(gfp_t gfp_mask, void *pool_data)
{
	struct crypt_config *cc = pool_data;
	struct page *page;

	if (unlikely(percpu_counter_compare(&cc->n_allocated_pages, dm_crypt_pages_per_client) >= 0) &&
	    likely(gfp_mask & __GFP_NORETRY))
		return NULL;

	page = alloc_page(gfp_mask);
	if (likely(page != NULL))
		percpu_counter_add(&cc->n_allocated_pages, 1);

	return page;
}

static void crypt_page_free(void *page, void *pool_data)
{
	struct crypt_config *cc = pool_data;

	__free_page(page);
	percpu_counter_sub(&cc->n_allocated_pages, 1);
}

static void crypt_dtr(struct dm_target *ti)
{
	struct crypt_config *cc = ti->private;

	ti->private = NULL;

	if (!cc)
		return;

	if (cc->write_thread)
		kthread_stop(cc->write_thread);

	if (cc->io_queue)
		destroy_workqueue(cc->io_queue);
	if (cc->crypt_queue)
		destroy_workqueue(cc->crypt_queue);

	crypt_free_tfm(cc);

	bioset_exit(&cc->bs);

	mempool_exit(&cc->page_pool);
	mempool_exit(&cc->req_pool);
	mempool_exit(&cc->tag_pool);

	WARN_ON(percpu_counter_sum(&cc->n_allocated_pages) != 0);
	percpu_counter_destroy(&cc->n_allocated_pages);

	if (cc->dev)
		dm_put_device(ti, cc->dev);

	kzfree(cc->cipher_string);
	kzfree(cc->key_string);
	kzfree(cc->cipher_auth);

	mutex_destroy(&cc->bio_alloc_lock);

	/* Must zero key material before freeing */
	kzfree(cc);

	spin_lock(&dm_crypt_clients_lock);
	WARN_ON(!dm_crypt_clients_n);
	dm_crypt_clients_n--;
	crypt_calculate_pages_per_client();
	spin_unlock(&dm_crypt_clients_lock);
}

static int get_iv_size_by_name(struct crypt_config *cc, char *alg_name)
{
	unsigned int iv_size;
	struct crypto_aead *tfm_aead;
	struct crypto_skcipher *tfm;

	if (crypt_integrity_aead(cc)) {
		tfm_aead = crypto_alloc_aead(alg_name, 0, 0);
		if (IS_ERR(tfm_aead))
			return -ENOMEM;

		iv_size = crypto_aead_ivsize(tfm_aead);
		crypto_free_aead(tfm_aead);
	} else {
		tfm = crypto_alloc_skcipher(alg_name, 0, 0);
		if (IS_ERR(tfm))
			return -ENOMEM;

		iv_size = crypto_skcipher_ivsize(tfm);
		crypto_free_skcipher(tfm);
	}

	return iv_size;

}

static int crypt_ctr_ivmode(struct dm_target *ti, const char *ivmode)
{
	struct crypt_config *cc = ti->private;

	if (crypt_integrity_aead(cc))
		cc->iv_size = crypto_aead_ivsize(any_tfm_aead(cc));
	else
		cc->iv_size = crypto_skcipher_ivsize(any_tfm(cc));

	if (cc->iv_size)
		/* at least a 64 bit sector number should fit in our buffer */
		cc->iv_size = max(cc->iv_size,
				  (unsigned int)(sizeof(u64) / sizeof(u8)));

	if (strcmp(ivmode, "random") == 0) {
		/* Need storage space in integrity fields. */
		cc->integrity_iv_size = cc->iv_size;
	}

	return 0;
}

/*
 * Workaround to parse HMAC algorithm from AEAD crypto API spec.
 * The HMAC is needed to calculate tag size (HMAC digest size).
 * This should be probably done by crypto-api calls (once available...)
 */
static int crypt_ctr_auth_cipher(struct crypt_config *cc, char *cipher_api)
{
	char *start, *end, *mac_alg = NULL;
	struct crypto_ahash *mac;

	if (!strstarts(cipher_api, "authenc("))
		return 0;

	start = strchr(cipher_api, '(');
	end = strchr(cipher_api, ',');
	if (!start || !end || ++start > end)
		return -EINVAL;

	mac_alg = kzalloc(end - start + 1, GFP_KERNEL);
	if (!mac_alg)
		return -ENOMEM;
	strncpy(mac_alg, start, end - start);

	mac = crypto_alloc_ahash(mac_alg, 0, 0);
	kfree(mac_alg);

	if (IS_ERR(mac))
		return PTR_ERR(mac);

	cc->key_mac_size = crypto_ahash_digestsize(mac);
	crypto_free_ahash(mac);

	return 0;
}

static int crypt_ctr_cipher_new(struct dm_target *ti, char *cipher_in, char *key,
				char **ivmode, char **ivopts)
{
	struct crypt_config *cc = ti->private;
	char *tmp, *cipher_api;
	char cipher_name[CRYPTO_MAX_ALG_NAME];
	int ret = -EINVAL;

	cc->tfms_count = 1;

	/*
	 * New format (capi: prefix)
	 * capi:cipher_api_spec-iv:ivopts
	 */
	tmp = &cipher_in[strlen("capi:")];
	cipher_api = strsep(&tmp, "-");
	*ivmode = strsep(&tmp, ":");
	*ivopts = tmp;

	if (*ivmode && !strcmp(*ivmode, "lmk"))
		cc->tfms_count = 64;

	cc->key_parts = cc->tfms_count;

	if (!*ivmode)
		*ivmode = "null";

	/*
	 * For those ciphers which do not support IVs, but input ivmode is not
	 * NULL, use "null" as ivmode compulsively.
	 */
	cc->iv_size = get_iv_size_by_name(cc, cipher_api);
	if (cc->iv_size < 0)
		return -ENOMEM;
	if (!cc->iv_size && ivmode) {
		DMWARN("Selected cipher does not support IVs");
		*ivmode = "null";
	}

	/* Allocate cipher */
	ret = snprintf(cipher_name, CRYPTO_MAX_ALG_NAME, "%s(%s)",
			*ivmode, cipher_api);
	if (ret < 0) {
		ti->error = "Cannot allocate cipher strings";
		return -ENOMEM;
	}
	ret = crypt_alloc_tfm(cc, cipher_name);
	if (ret < 0) {
		ti->error = "Error allocating crypto tfm";
		return ret;
	}

	/* Alloc AEAD, can be used only in new format. */
	if (crypt_integrity_aead(cc)) {
		ret = crypt_ctr_auth_cipher(cc, cipher_api);
		if (ret < 0) {
			ti->error = "Invalid AEAD cipher spec";
			return -ENOMEM;
		}
		cc->iv_size = crypto_aead_ivsize(any_tfm_aead(cc));
	} else
		cc->iv_size = crypto_skcipher_ivsize(any_tfm(cc));

	return 0;
}

static int crypt_ctr_cipher_old(struct dm_target *ti, char *cipher_in, char *key,
				char **ivmode, char **ivopts)
{
	struct crypt_config *cc = ti->private;
	char *tmp, *cipher, *chainmode, *keycount;
	char *cipher_api = NULL;
	int ret = -EINVAL;
	char dummy;

	if (strchr(cipher_in, '(') || crypt_integrity_aead(cc)) {
		ti->error = "Bad cipher specification";
		return -EINVAL;
	}

	/*
	 * Legacy dm-crypt cipher specification
	 * cipher[:keycount]-mode-iv:ivopts
	 */
	tmp = cipher_in;
	keycount = strsep(&tmp, "-");
	cipher = strsep(&keycount, ":");

	if (!keycount)
		cc->tfms_count = 1;
	else if (sscanf(keycount, "%u%c", &cc->tfms_count, &dummy) != 1 ||
		 !is_power_of_2(cc->tfms_count)) {
		ti->error = "Bad cipher key count specification";
		return -EINVAL;
	}
	cc->key_parts = cc->tfms_count;

	chainmode = strsep(&tmp, "-");
	*ivopts = strsep(&tmp, "-");
	*ivmode = strsep(&*ivopts, ":");

	if (tmp)
		DMWARN("Ignoring unexpected additional cipher options");

	/*
	 * For compatibility with the original dm-crypt mapping format, if
	 * only the cipher name is supplied, use cbc-plain.
	 */
	if (!chainmode || (!strcmp(chainmode, "plain") && !*ivmode)) {
		chainmode = "cbc";
		*ivmode = "plain";
	}

	if (strcmp(chainmode, "ecb") && !*ivmode) {
		ti->error = "IV mechanism required";
		return -EINVAL;
	}

	cipher_api = kmalloc(CRYPTO_MAX_ALG_NAME, GFP_KERNEL);
	if (!cipher_api)
		goto bad_mem;

	/* For those ciphers which do not support IVs,
	 * use the 'null' template cipher
	 */
	if (!*ivmode)
		*ivmode = "null";

	/*
	 * For those ciphers which do not support IVs, but input ivmode is not
	 * NULL, use "null" as ivmode compulsively.
	 */
	ret = snprintf(cipher_api, CRYPTO_MAX_ALG_NAME,
		       "%s(%s)", chainmode, cipher);
	cc->iv_size = get_iv_size_by_name(cc, cipher_api);
	if (cc->iv_size < 0)
		return -ENOMEM;
	if (!cc->iv_size && ivmode) {
		DMWARN("Selected cipher does not support IVs");
		*ivmode = "null";
	}

	ret = snprintf(cipher_api, CRYPTO_MAX_ALG_NAME,
		       "%s(%s(%s))", *ivmode, chainmode, cipher);
	if (ret < 0) {
		kfree(cipher_api);
		goto bad_mem;
	}

	/* Allocate cipher */
	ret = crypt_alloc_tfm(cc, cipher_api);
	if (ret < 0) {
		ti->error = "Error allocating crypto tfm";
		kfree(cipher_api);
		return ret;
	}
	kfree(cipher_api);

	return 0;
bad_mem:
	ti->error = "Cannot allocate cipher strings";
	return -ENOMEM;
}

static int crypt_ctr_cipher(struct dm_target *ti, char *cipher_in, char *key)
{
	struct crypt_config *cc = ti->private;
	char *ivmode = NULL, *ivopts = NULL;
	int ret;

	cc->cipher_string = kstrdup(cipher_in, GFP_KERNEL);
	if (!cc->cipher_string) {
		ti->error = "Cannot allocate cipher strings";
		return -ENOMEM;
	}

	if (strstarts(cipher_in, "capi:"))
		ret = crypt_ctr_cipher_new(ti, cipher_in, key, &ivmode, &ivopts);
	else
		ret = crypt_ctr_cipher_old(ti, cipher_in, key, &ivmode, &ivopts);
	if (ret)
		return ret;

	/* Initialize IV */
	ret = crypt_ctr_ivmode(ti, ivmode);
	if (ret < 0)
		return ret;

	/* Initialize and set key */
	ret = crypt_init_key(ti, key, ivopts);
	if (ret < 0) {
		ti->error = "Error decoding and setting key";
		return ret;
	}

	/* wipe the kernel key payload copy */
	if (cc->key_string)
		memset(cc->key, 0, cc->key_size * sizeof(u8));

	return ret;
}

static int crypt_ctr_optional(struct dm_target *ti, unsigned int argc, char **argv)
{
	struct crypt_config *cc = ti->private;
	struct dm_arg_set as;
	static const struct dm_arg _args[] = {
		{0, 6, "Invalid number of feature args"},
	};
	unsigned int opt_params, val;
	const char *opt_string, *sval;
	char dummy;
	int ret;

	/* Optional parameters */
	as.argc = argc;
	as.argv = argv;

	ret = dm_read_arg_group(_args, &as, &opt_params, &ti->error);
	if (ret)
		return ret;

	while (opt_params--) {
		opt_string = dm_shift_arg(&as);
		if (!opt_string) {
			ti->error = "Not enough feature arguments";
			return -EINVAL;
		}

		if (!strcasecmp(opt_string, "allow_discards"))
			ti->num_discard_bios = 1;

		else if (!strcasecmp(opt_string, "same_cpu_crypt"))
			set_bit(DM_CRYPT_SAME_CPU, &cc->flags);

		else if (!strcasecmp(opt_string, "submit_from_crypt_cpus"))
			set_bit(DM_CRYPT_NO_OFFLOAD, &cc->flags);
		else if (sscanf(opt_string, "integrity:%u:", &val) == 1) {
			if (val == 0 || val > MAX_TAG_SIZE) {
				ti->error = "Invalid integrity arguments";
				return -EINVAL;
			}
			cc->on_disk_tag_size = val;
			sval = strchr(opt_string + strlen("integrity:"), ':') + 1;
			if (!strcasecmp(sval, "aead")) {
				set_bit(CRYPT_MODE_INTEGRITY_AEAD, &cc->cipher_flags);
			} else  if (strcasecmp(sval, "none")) {
				ti->error = "Unknown integrity profile";
				return -EINVAL;
			}

			cc->cipher_auth = kstrdup(sval, GFP_KERNEL);
			if (!cc->cipher_auth)
				return -ENOMEM;
		} else if (sscanf(opt_string, "sector_size:%hu%c", &cc->sector_size, &dummy) == 1) {
			if (cc->sector_size < (1 << SECTOR_SHIFT) ||
			    cc->sector_size > 4096 ||
			    (cc->sector_size & (cc->sector_size - 1))) {
				ti->error = "Invalid feature value for sector_size";
				return -EINVAL;
			}
			if (ti->len & ((cc->sector_size >> SECTOR_SHIFT) - 1)) {
				ti->error = "Device size is not multiple of sector_size feature";
				return -EINVAL;
			}
			cc->sector_shift = __ffs(cc->sector_size) - SECTOR_SHIFT;
		} else if (!strcasecmp(opt_string, "iv_large_sectors"))
			set_bit(CRYPT_IV_LARGE_SECTORS, &cc->cipher_flags);
		else {
			ti->error = "Invalid feature arguments";
			return -EINVAL;
		}
	}

	return 0;
}

/*
 * Construct an encryption mapping:
 * <cipher> [<key>|:<key_size>:<user|logon>:<key_description>] <iv_offset> <dev_path> <start>
 */
static int crypt_ctr(struct dm_target *ti, unsigned int argc, char **argv)
{
	struct crypt_config *cc;
	int key_size;
	unsigned int align_mask;
	unsigned long long tmpll;
	int ret;
	size_t additional_req_size;
	char dummy;

	if (argc < 5) {
		ti->error = "Not enough arguments";
		return -EINVAL;
	}

	key_size = get_key_size(&argv[1]);
	if (key_size < 0) {
		ti->error = "Cannot parse key size";
		return -EINVAL;
	}

	cc = kzalloc(sizeof(*cc) + key_size * sizeof(u8), GFP_KERNEL);
	if (!cc) {
		ti->error = "Cannot allocate encryption context";
		return -ENOMEM;
	}
	cc->key_size = key_size;
	cc->sector_size = (1 << SECTOR_SHIFT);
	cc->sector_shift = 0;

	ti->private = cc;

	spin_lock(&dm_crypt_clients_lock);
	dm_crypt_clients_n++;
	crypt_calculate_pages_per_client();
	spin_unlock(&dm_crypt_clients_lock);

	ret = percpu_counter_init(&cc->n_allocated_pages, 0, GFP_KERNEL);
	if (ret < 0)
		goto bad;

	/* Optional parameters need to be read before cipher constructor */
	if (argc > 5) {
		ret = crypt_ctr_optional(ti, argc - 5, &argv[5]);
		if (ret)
			goto bad;
	}

	ret = crypt_ctr_cipher(ti, argv[0], argv[1]);
	if (ret < 0)
		goto bad;

	if (crypt_integrity_aead(cc)) {
		cc->dmreq_start = sizeof(struct aead_request);
		cc->dmreq_start += crypto_aead_reqsize(any_tfm_aead(cc));
		align_mask = crypto_aead_alignmask(any_tfm_aead(cc));
	} else {
		cc->dmreq_start = sizeof(struct skcipher_request);
		cc->dmreq_start += crypto_skcipher_reqsize(any_tfm(cc));
		align_mask = crypto_skcipher_alignmask(any_tfm(cc));
	}
	cc->dmreq_start = ALIGN(cc->dmreq_start, __alignof__(struct dm_crypt_request));

	additional_req_size = sizeof(struct dm_crypt_request);

	ret = mempool_init_kmalloc_pool(&cc->req_pool, MIN_IOS, cc->dmreq_start + additional_req_size);
	if (ret) {
		ti->error = "Cannot allocate crypt request mempool";
		goto bad;
	}

	cc->per_bio_data_size = ti->per_io_data_size =
		ALIGN(sizeof(struct dm_crypt_io) + cc->dmreq_start + additional_req_size,
		      ARCH_KMALLOC_MINALIGN);

	ret = mempool_init(&cc->page_pool, BIO_MAX_PAGES, crypt_page_alloc, crypt_page_free, cc);
	if (ret) {
		ti->error = "Cannot allocate page mempool";
		goto bad;
	}

	ret = bioset_init(&cc->bs, MIN_IOS, 0, BIOSET_NEED_BVECS);
	if (ret) {
		ti->error = "Cannot allocate crypt bioset";
		goto bad;
	}

	mutex_init(&cc->bio_alloc_lock);

	ret = -EINVAL;
	if ((sscanf(argv[2], "%llu%c", &tmpll, &dummy) != 1) ||
	    (tmpll & ((cc->sector_size >> SECTOR_SHIFT) - 1))) {
		ti->error = "Invalid iv_offset sector";
		goto bad;
	}
	cc->iv_offset = tmpll;

	ret = dm_get_device(ti, argv[3], dm_table_get_mode(ti->table), &cc->dev);
	if (ret) {
		ti->error = "Device lookup failed";
		goto bad;
	}

	ret = -EINVAL;
	if (sscanf(argv[4], "%llu%c", &tmpll, &dummy) != 1) {
		ti->error = "Invalid device sector";
		goto bad;
	}
	cc->start = tmpll;

	if (crypt_integrity_aead(cc) || cc->integrity_iv_size) {
		ret = crypt_integrity_ctr(cc, ti);
		if (ret)
			goto bad;

		cc->tag_pool_max_sectors = POOL_ENTRY_SIZE / cc->on_disk_tag_size;
		if (!cc->tag_pool_max_sectors)
			cc->tag_pool_max_sectors = 1;

		ret = mempool_init_kmalloc_pool(&cc->tag_pool, MIN_IOS,
			cc->tag_pool_max_sectors * cc->on_disk_tag_size);
		if (ret) {
			ti->error = "Cannot allocate integrity tags mempool";
			goto bad;
		}

		cc->tag_pool_max_sectors <<= cc->sector_shift;
	}

	ret = -ENOMEM;
	cc->io_queue = alloc_workqueue("kcryptd_io", WQ_HIGHPRI | WQ_CPU_INTENSIVE | WQ_MEM_RECLAIM, 1);
	if (!cc->io_queue) {
		ti->error = "Couldn't create kcryptd io queue";
		goto bad;
	}

	if (test_bit(DM_CRYPT_SAME_CPU, &cc->flags))
		cc->crypt_queue = alloc_workqueue("kcryptd", WQ_HIGHPRI | WQ_CPU_INTENSIVE | WQ_MEM_RECLAIM, 1);
	else
		cc->crypt_queue = alloc_workqueue("kcryptd",
						  WQ_HIGHPRI | WQ_CPU_INTENSIVE | WQ_MEM_RECLAIM | WQ_UNBOUND,
						  num_online_cpus());
	if (!cc->crypt_queue) {
		ti->error = "Couldn't create kcryptd queue";
		goto bad;
	}

	init_waitqueue_head(&cc->write_thread_wait);
	cc->write_tree = RB_ROOT;

	cc->write_thread = kthread_create(dmcrypt_write, cc, "dmcrypt_write");
	if (IS_ERR(cc->write_thread)) {
		ret = PTR_ERR(cc->write_thread);
		cc->write_thread = NULL;
		ti->error = "Couldn't spawn write thread";
		goto bad;
	}
	wake_up_process(cc->write_thread);

	ti->num_flush_bios = 1;

	return 0;

bad:
	crypt_dtr(ti);
	return ret;
}

static int crypt_map(struct dm_target *ti, struct bio *bio)
{
	struct dm_crypt_io *io;
	struct crypt_config *cc = ti->private;

	/*
	 * If bio is REQ_PREFLUSH or REQ_OP_DISCARD, just bypass crypt queues.
	 * - for REQ_PREFLUSH device-mapper core ensures that no IO is in-flight
	 * - for REQ_OP_DISCARD caller must use flush if IO ordering matters
	 */
	if (unlikely(bio->bi_opf & REQ_PREFLUSH ||
	    bio_op(bio) == REQ_OP_DISCARD)) {
		bio_set_dev(bio, cc->dev->bdev);
		if (bio_sectors(bio))
			bio->bi_iter.bi_sector = cc->start +
				dm_target_offset(ti, bio->bi_iter.bi_sector);
		return DM_MAPIO_REMAPPED;
	}

	/*
	 * Check if bio is too large, split as needed.
	 */
	if (unlikely(bio->bi_iter.bi_size > (BIO_MAX_PAGES << PAGE_SHIFT)) &&
	    (bio_data_dir(bio) == WRITE || cc->on_disk_tag_size))
		dm_accept_partial_bio(bio, ((BIO_MAX_PAGES << PAGE_SHIFT) >> SECTOR_SHIFT));

	/*
	 * Ensure that bio is a multiple of internal sector encryption size
	 * and is aligned to this size as defined in IO hints.
	 */
	if (unlikely((bio->bi_iter.bi_sector & ((cc->sector_size >> SECTOR_SHIFT) - 1)) != 0))
		return DM_MAPIO_KILL;

	if (unlikely(bio->bi_iter.bi_size & (cc->sector_size - 1)))
		return DM_MAPIO_KILL;

	io = dm_per_bio_data(bio, cc->per_bio_data_size);
	crypt_io_init(io, cc, bio, dm_target_offset(ti, bio->bi_iter.bi_sector));

	if (cc->on_disk_tag_size) {
		unsigned tag_len = cc->on_disk_tag_size * (bio_sectors(bio) >> cc->sector_shift);

		if (unlikely(tag_len > KMALLOC_MAX_SIZE) ||
		    unlikely(!(io->integrity_metadata = kmalloc(tag_len,
				GFP_NOIO | __GFP_NORETRY | __GFP_NOMEMALLOC | __GFP_NOWARN)))) {
			if (bio_sectors(bio) > cc->tag_pool_max_sectors)
				dm_accept_partial_bio(bio, cc->tag_pool_max_sectors);
			io->integrity_metadata = mempool_alloc(&cc->tag_pool, GFP_NOIO);
			io->integrity_metadata_from_pool = true;
		}
	}

	if (crypt_integrity_aead(cc))
		io->ctx.r.req_aead = (struct aead_request *)(io + 1);
	else
		io->ctx.r.req = (struct skcipher_request *)(io + 1);

	if (bio_data_dir(io->base_bio) == READ) {
		if (kcryptd_io_read(io, GFP_NOWAIT))
			kcryptd_queue_read(io);
	} else
		kcryptd_queue_crypt(io);

	return DM_MAPIO_SUBMITTED;
}

static void crypt_status(struct dm_target *ti, status_type_t type,
			 unsigned status_flags, char *result, unsigned maxlen)
{
	struct crypt_config *cc = ti->private;
	unsigned i, sz = 0;
	int num_feature_args = 0;

	switch (type) {
	case STATUSTYPE_INFO:
		result[0] = '\0';
		break;

	case STATUSTYPE_TABLE:
		DMEMIT("%s ", cc->cipher_string);

		if (cc->key_size > 0) {
			if (cc->key_string)
				DMEMIT(":%u:%s", cc->key_size, cc->key_string);
			else
				for (i = 0; i < cc->key_size; i++)
					DMEMIT("%02x", cc->key[i]);
		} else
			DMEMIT("-");

		DMEMIT(" %llu %s %llu", (unsigned long long)cc->iv_offset,
				cc->dev->name, (unsigned long long)cc->start);

		num_feature_args += !!ti->num_discard_bios;
		num_feature_args += test_bit(DM_CRYPT_SAME_CPU, &cc->flags);
		num_feature_args += test_bit(DM_CRYPT_NO_OFFLOAD, &cc->flags);
		num_feature_args += cc->sector_size != (1 << SECTOR_SHIFT);
		num_feature_args += test_bit(CRYPT_IV_LARGE_SECTORS, &cc->cipher_flags);
		if (cc->on_disk_tag_size)
			num_feature_args++;
		if (num_feature_args) {
			DMEMIT(" %d", num_feature_args);
			if (ti->num_discard_bios)
				DMEMIT(" allow_discards");
			if (test_bit(DM_CRYPT_SAME_CPU, &cc->flags))
				DMEMIT(" same_cpu_crypt");
			if (test_bit(DM_CRYPT_NO_OFFLOAD, &cc->flags))
				DMEMIT(" submit_from_crypt_cpus");
			if (cc->on_disk_tag_size)
				DMEMIT(" integrity:%u:%s", cc->on_disk_tag_size, cc->cipher_auth);
			if (cc->sector_size != (1 << SECTOR_SHIFT))
				DMEMIT(" sector_size:%d", cc->sector_size);
			if (test_bit(CRYPT_IV_LARGE_SECTORS, &cc->cipher_flags))
				DMEMIT(" iv_large_sectors");
		}

		break;
	}
}

static void crypt_postsuspend(struct dm_target *ti)
{
	struct crypt_config *cc = ti->private;

	set_bit(DM_CRYPT_SUSPENDED, &cc->flags);
}

static int crypt_preresume(struct dm_target *ti)
{
	struct crypt_config *cc = ti->private;

	if (!test_bit(DM_CRYPT_KEY_VALID, &cc->flags)) {
		DMERR("aborting resume - crypt key is not set.");
		return -EAGAIN;
	}

	return 0;
}

static void crypt_resume(struct dm_target *ti)
{
	struct crypt_config *cc = ti->private;

	clear_bit(DM_CRYPT_SUSPENDED, &cc->flags);
}

/* Message interface
 *	key set <key>
 *	key wipe
 */
static int crypt_message(struct dm_target *ti, unsigned argc, char **argv,
			 char *result, unsigned maxlen)
{
	struct crypt_config *cc = ti->private;
	int key_size, ret = -EINVAL;

	if (argc < 2)
		goto error;

	if (!strcasecmp(argv[0], "key")) {
		if (!test_bit(DM_CRYPT_SUSPENDED, &cc->flags)) {
			DMWARN("not suspended during key manipulation.");
			return -EINVAL;
		}
		if (argc == 3 && !strcasecmp(argv[1], "set")) {
			/* The key size may not be changed. */
			key_size = get_key_size(&argv[2]);
			if (key_size < 0 || cc->key_size != key_size) {
				memset(argv[2], '0', strlen(argv[2]));
				return -EINVAL;
			}

			ret = crypt_set_key(cc, SETKEY_OP_SET, argv[2], NULL);
			/* wipe the kernel key payload copy */
			if (cc->key_string)
				memset(cc->key, 0, cc->key_size * sizeof(u8));
			return ret;
		}
		if (argc == 2 && !strcasecmp(argv[1], "wipe")) {
			return crypt_wipe_key(cc);
		}
	}

error:
	DMWARN("unrecognised message received.");
	return -EINVAL;
}

static int crypt_iterate_devices(struct dm_target *ti,
				 iterate_devices_callout_fn fn, void *data)
{
	struct crypt_config *cc = ti->private;

	return fn(ti, cc->dev, cc->start, ti->len, data);
}

static void crypt_io_hints(struct dm_target *ti, struct queue_limits *limits)
{
	struct crypt_config *cc = ti->private;

	/*
	 * Unfortunate constraint that is required to avoid the potential
	 * for exceeding underlying device's max_segments limits -- due to
	 * crypt_alloc_buffer() possibly allocating pages for the encryption
	 * bio that are not as physically contiguous as the original bio.
	 */
	limits->max_segment_size = PAGE_SIZE;

	if (cc->sector_size != (1 << SECTOR_SHIFT)) {
		limits->logical_block_size = cc->sector_size;
		limits->physical_block_size = cc->sector_size;
		blk_limits_io_min(limits, cc->sector_size);
	}
}

static struct target_type crypt_target = {
	.name   = "crypt",
	.version = {1, 19, 1},
	.module = THIS_MODULE,
	.ctr    = crypt_ctr,
	.dtr    = crypt_dtr,
	.map    = crypt_map,
	.status = crypt_status,
	.postsuspend = crypt_postsuspend,
	.preresume = crypt_preresume,
	.resume = crypt_resume,
	.message = crypt_message,
	.iterate_devices = crypt_iterate_devices,
	.io_hints = crypt_io_hints,
};

static int __init dm_crypt_init(void)
{
	int r;

	r = dm_register_target(&crypt_target);
	if (r < 0)
		DMERR("register failed %d", r);

	return r;
}

void __exit dm_crypt_exit(void)
{
	dm_unregister_target(&crypt_target);
}

module_init(dm_crypt_init);
module_exit(dm_crypt_exit);

MODULE_AUTHOR("Jana Saout <jana@saout.de>");
MODULE_DESCRIPTION(DM_NAME " target for transparent encryption / decryption");
MODULE_LICENSE("GPL");
