/*
 * Copyright (C) 2017 Red Hat, Inc.  All rights reserved.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU General Public License version 2.
 */

#ifndef __AOPS_DOT_H__
#define __AOPS_DOT_H__

#include "incore.h"

extern ssize_t gfs2_stuffed_write(struct kiocb *iocb, struct iov_iter *from);
extern void adjust_fs_space(struct inode *inode);
extern void gfs2_page_add_databufs(struct gfs2_inode *ip, struct page *page,
				   unsigned int from, unsigned int len);

#endif /* __AOPS_DOT_H__ */
