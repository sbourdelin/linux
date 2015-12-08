/***********************************************************************
**
** Implementation of the Skein hash function.
**
** Source code author: Doug Whiting, 2008.
**
** This algorithm and source code is released to the public domain.
**
************************************************************************/

#define BLK_BITS        (WCNT * 64) /* some useful definitions for code here */
#define KW_TWK_BASE     (0)
#define KW_KEY_BASE     (3)
#define ks              (kw + KW_KEY_BASE)
#define ts              (kw + KW_TWK_BASE)

#ifndef _SKEIN_BLOCK_H_
#define _SKEIN_BLOCK_H_

#include "skein_base.h" /* get the Skein API definitions   */

void skein_256_process_block(struct skein_256_ctx *ctx, const u8 *blk_ptr,
			     size_t blk_cnt, size_t byte_cnt_add);
void skein_512_process_block(struct skein_512_ctx *ctx, const u8 *blk_ptr,
			     size_t blk_cnt, size_t byte_cnt_add);
void skein_1024_process_block(struct skein_1024_ctx *ctx, const u8 *blk_ptr,
			      size_t blk_cnt, size_t byte_cnt_add);

#endif
