// SPDX-License-Identifier: GPL-2.0
/*
 * This contains encryption functions for per-file encryption.
 *
 * Copyright (C) 2015, Google, Inc.
 * Copyright (C) 2015, Motorola Mobility
 *
 * Written by Michael Halcrow, 2014.
 *
 * Filename encryption additions
 *	Uday Savagaonkar, 2014
 * Encryption policy handling additions
 *	Ildar Muslukhov, 2014
 * Add fscrypt_pullback_bio_page()
 *	Jaegeuk Kim, 2015.
 *
 * This has not yet undergone a rigorous security audit.
 *
 * The usage of AES-XTS should conform to recommendations in NIST
 * Special Publication 800-38E and IEEE P1619/D16.
 */

#include <linux/pagemap.h>
#include <linux/module.h>
#include <linux/bio.h>
#include <linux/namei.h>
#include <linux/buffer_head.h>
#include "fscrypt_private.h"

static void __fscrypt_decrypt_bio(struct bio *bio, bool done)
{
	struct bio_vec *bv;
	int i;

	bio_for_each_segment_all(bv, bio, i) {
		struct page *page = bv->bv_page;
		struct inode *inode = page->mapping->host;
		u64 blk;
		int ret;

		blk = page->index << (PAGE_SHIFT - inode->i_blkbits);
		blk += bv->bv_offset >> inode->i_blkbits;

		ret = fscrypt_decrypt_page(page->mapping->host, page,
					bv->bv_len, bv->bv_offset, blk);
		if (ret) {
			WARN_ON_ONCE(1);
			SetPageError(page);
		} else if (done) {
			SetPageUptodate(page);
		}
		if (done)
			unlock_page(page);
	}
}

void fscrypt_decrypt_bio(struct bio *bio)
{
	__fscrypt_decrypt_bio(bio, false);
}
EXPORT_SYMBOL(fscrypt_decrypt_bio);

void fscrypt_complete_pages(struct work_struct *work)
{
	struct fscrypt_ctx *ctx =
		container_of(work, struct fscrypt_ctx, r.work);
	struct bio *bio = ctx->r.bio;

	__fscrypt_decrypt_bio(bio, true);
	fscrypt_release_ctx(ctx);
	bio_put(bio);
}
EXPORT_SYMBOL(fscrypt_complete_pages);

void fscrypt_complete_block(struct work_struct *work)
{
	struct fscrypt_ctx *ctx =
		container_of(work, struct fscrypt_ctx, r.work);
	struct buffer_head *bh;
	struct bio *bio;
	struct bio_vec *bv;
	struct page *page;
	struct inode *inode;
	u64 blk_nr;
	int ret;

	bio = ctx->r.bio;
	WARN_ON(bio->bi_vcnt != 1);

	bv = bio->bi_io_vec;
	page = bv->bv_page;
	inode = page->mapping->host;

	WARN_ON(bv->bv_len != i_blocksize(inode));

	blk_nr = page->index << (PAGE_SHIFT - inode->i_blkbits);
	blk_nr += bv->bv_offset >> inode->i_blkbits;

	bh = ctx->r.bh;

	ret = fscrypt_decrypt_page(inode, page, bv->bv_len,
				bv->bv_offset, blk_nr);

	bh->b_end_io(bh, !ret);

	fscrypt_release_ctx(ctx);
	bio_put(bio);
}
EXPORT_SYMBOL(fscrypt_complete_block);

bool fscrypt_bio_encrypted(struct bio *bio)
{
	struct address_space *mapping;
	struct inode *inode;
	struct page *page;

	if (bio_op(bio) == REQ_OP_READ && bio->bi_vcnt) {
		page = bio->bi_io_vec->bv_page;

		if (!PageSwapCache(page)) {
			mapping = page_mapping(page);
			if (mapping) {
				inode = mapping->host;

				if (IS_ENCRYPTED(inode) &&
					S_ISREG(inode->i_mode))
					return true;
			}
		}
	}

	return false;
}
EXPORT_SYMBOL(fscrypt_bio_encrypted);

void fscrypt_enqueue_decrypt_bio(struct fscrypt_ctx *ctx, struct bio *bio,
			void (*process_bio)(struct work_struct *))
{
	BUG_ON(!process_bio);
	INIT_WORK(&ctx->r.work, process_bio);
	ctx->r.bio = bio;
	fscrypt_enqueue_decrypt_work(&ctx->r.work);
}
EXPORT_SYMBOL(fscrypt_enqueue_decrypt_bio);

post_process_read_t *fscrypt_get_post_process(struct fscrypt_ctx *ctx)
{
	return &(ctx->r.post_process);
}
EXPORT_SYMBOL(fscrypt_get_post_process);

void fscrypt_set_post_process(struct fscrypt_ctx *ctx,
			post_process_read_t *post_process)
{
	ctx->r.post_process = *post_process;
}

struct buffer_head *fscrypt_get_bh(struct fscrypt_ctx *ctx)
{
	return ctx->r.bh;
}
EXPORT_SYMBOL(fscrypt_get_bh);

void fscrypt_set_bh(struct fscrypt_ctx *ctx, struct buffer_head *bh)
{
	ctx->r.bh = bh;
}
EXPORT_SYMBOL(fscrypt_set_bh);

void fscrypt_pullback_bio_page(struct page **page, bool restore)
{
	struct fscrypt_ctx *ctx;
	struct page *bounce_page;

	/* The bounce data pages are unmapped. */
	if ((*page)->mapping)
		return;

	/* The bounce data page is unmapped. */
	bounce_page = *page;
	ctx = (struct fscrypt_ctx *)page_private(bounce_page);

	/* restore control page */
	*page = ctx->w.control_page;

	if (restore)
		fscrypt_restore_control_page(bounce_page);
}
EXPORT_SYMBOL(fscrypt_pullback_bio_page);

int fscrypt_zeroout_range(const struct inode *inode, pgoff_t lblk,
				sector_t pblk, unsigned int len)
{
	struct fscrypt_ctx *ctx;
	struct page *ciphertext_page = NULL;
	unsigned int page_nr_blks;
	unsigned int offset;
	struct bio *bio;
	int ret, err = 0;
	int i;

	ctx = fscrypt_get_ctx(inode, GFP_NOFS);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);

	ciphertext_page = fscrypt_alloc_bounce_page(ctx, GFP_NOWAIT);
	if (IS_ERR(ciphertext_page)) {
		err = PTR_ERR(ciphertext_page);
		goto errout;
	}

	page_nr_blks = 1 << (PAGE_SHIFT - inode->i_blkbits);

	while (len) {
		page_nr_blks = min_t(unsigned int, page_nr_blks, len);
		offset = 0;

		for (i = 0; i < page_nr_blks; i++) {
			err = fscrypt_do_block_crypto(inode, FS_ENCRYPT, lblk,
						ZERO_PAGE(0), ciphertext_page,
						inode->i_sb->s_blocksize,
						offset, GFP_NOFS);
			if (err)
				goto errout;
			lblk++;
			offset += inode->i_sb->s_blocksize;
		}

		bio = bio_alloc(GFP_NOWAIT, 1);
		if (!bio) {
			err = -ENOMEM;
			goto errout;
		}
		bio_set_dev(bio, inode->i_sb->s_bdev);
		bio->bi_iter.bi_sector =
			pblk << (inode->i_sb->s_blocksize_bits - 9);
		bio_set_op_attrs(bio, REQ_OP_WRITE, 0);
		ret = bio_add_page(bio, ciphertext_page, offset, 0);
		if (ret != offset) {
			/* should never happen! */
			WARN_ON(1);
			bio_put(bio);
			err = -EIO;
			goto errout;
		}
		err = submit_bio_wait(bio);
		if (err == 0 && bio->bi_status)
			err = -EIO;
		bio_put(bio);
		if (err)
			goto errout;
		pblk += page_nr_blks;
		len -= page_nr_blks;
	}
	err = 0;
errout:
	fscrypt_release_ctx(ctx);
	return err;
}
EXPORT_SYMBOL(fscrypt_zeroout_range);
