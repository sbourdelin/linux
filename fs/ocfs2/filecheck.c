/* -*- mode: c; c-basic-offset: 8; -*-
 * vim: noexpandtab sw=8 ts=8 sts=0:
 *
 * filecheck.c
 *
 * Code which implements online file check.
 *
 * Copyright (C) 2016 SuSE.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation, version 2.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */

#include <linux/fs.h>
#include <cluster/masklog.h>

#include "ocfs2.h"
#include "ocfs2_fs.h"
#include "stackglue.h"
#include "inode.h"

#include "filecheck.h"


/* File check error strings,
 * must correspond with error number in header file.
 */
static const char * const ocfs2_filecheck_errs[] = {
	"SUCCESS",
	"FAILED",
	"INPROGRESS",
	"READONLY",
	"INJBD",
	"INVALIDINO",
	"BLOCKECC",
	"BLOCKNO",
	"VALIDFLAG",
	"GENERATION",
	"UNSUPPORTED"
};

struct ocfs2_filecheck_entry {
	struct list_head fe_list;
	unsigned long fe_ino;
	unsigned int fe_type;
	unsigned int fe_done:1;
	unsigned int fe_status:31;
};

static const char *
ocfs2_filecheck_error(int errno)
{
	if (!errno)
		return ocfs2_filecheck_errs[errno];

	BUG_ON(errno < OCFS2_FILECHECK_ERR_START ||
	       errno > OCFS2_FILECHECK_ERR_END);
	return ocfs2_filecheck_errs[errno - OCFS2_FILECHECK_ERR_START + 1];
}

static int
ocfs2_filecheck_erase_entries(struct ocfs2_super *,
			      unsigned int count);

int ocfs2_filecheck_set_max_entries(struct ocfs2_super *osb,
				int len)
{
	int ret;

	if ((len < OCFS2_FILECHECK_MINSIZE) || (len > OCFS2_FILECHECK_MAXSIZE))
		return -EINVAL;

	spin_lock(&osb->fc_lock);
	if (len < (osb->fc_size - osb->fc_done)) {
		mlog(ML_ERROR,
		"Cannot set online file check maximum entry number "
		"to %u due to too many pending entries(%u)\n",
		len, osb->fc_size - osb->fc_done);
		ret = -EBUSY;
	} else {
		if (len < osb->fc_size)
			BUG_ON(!ocfs2_filecheck_erase_entries(osb,
				osb->fc_size - len));

		osb->fc_max = len;
		ret = 0;
	}
	spin_unlock(&osb->fc_lock);

	return ret;
}


int ocfs2_filecheck_show(struct ocfs2_super *osb, unsigned int type,
				    char *buf)
{

	ssize_t ret = 0, total = 0, remain = PAGE_SIZE;
	struct ocfs2_filecheck_entry *p;

	ret = snprintf(buf, remain, "INO\t\tDONE\tERROR\n");
	total += ret;
	remain -= ret;
	spin_lock(&osb->fc_lock);
	list_for_each_entry(p, &osb->file_check_entries, fe_list) {
		if (p->fe_type != type)
			continue;

		ret = snprintf(buf + total, remain, "%lu\t\t%u\t%s\n",
			       p->fe_ino, p->fe_done,
			       ocfs2_filecheck_error(p->fe_status));
		if (ret < 0) {
			total = ret;
			break;
		}
		if (ret == remain) {
			/* snprintf() didn't fit */
			total = -E2BIG;
			break;
		}
		total += ret;
		remain -= ret;
	}
	spin_unlock(&osb->fc_lock);
	return total;
}

static int
ocfs2_filecheck_erase_entry(struct ocfs2_super *osb)
{
	struct ocfs2_filecheck_entry *p;

	list_for_each_entry(p, &osb->file_check_entries, fe_list) {
		if (p->fe_done) {
			list_del(&p->fe_list);
			kfree(p);
			osb->fc_size--;
			osb->fc_done--;
			return 1;
		}
	}

	return 0;
}

static int
ocfs2_filecheck_erase_entries(struct ocfs2_super *osb,
			      unsigned int count)
{
	unsigned int i = 0;
	unsigned int ret = 0;

	while (i++ < count) {
		if (ocfs2_filecheck_erase_entry(osb))
			ret++;
		else
			break;
	}

	return (ret == count ? 1 : 0);
}

static void
ocfs2_filecheck_done_entry(struct ocfs2_super *osb,
			   struct ocfs2_filecheck_entry *entry)
{
	entry->fe_done = 1;
	spin_lock(&osb->fc_lock);
	osb->fc_done++;
	spin_unlock(&osb->fc_lock);
}

static unsigned int
ocfs2_filecheck_handle(struct ocfs2_super *osb,
		       unsigned long ino, unsigned int flags)
{
	unsigned int ret = OCFS2_FILECHECK_ERR_SUCCESS;
	struct inode *inode = NULL;
	int rc;

	inode = ocfs2_iget(osb, ino, flags, 0);
	if (IS_ERR(inode)) {
		rc = (int)(-(long)inode);
		if (rc >= OCFS2_FILECHECK_ERR_START &&
		    rc < OCFS2_FILECHECK_ERR_END)
			ret = rc;
		else
			ret = OCFS2_FILECHECK_ERR_FAILED;
	} else
		iput(inode);

	return ret;
}

static void
ocfs2_filecheck_handle_entry(struct ocfs2_super *osb,
			     struct ocfs2_filecheck_entry *entry)
{
	if (entry->fe_type == OCFS2_FILECHECK_TYPE_CHK)
		entry->fe_status = ocfs2_filecheck_handle(osb,
				entry->fe_ino, OCFS2_FI_FLAG_FILECHECK_CHK);
	else if (entry->fe_type == OCFS2_FILECHECK_TYPE_FIX)
		entry->fe_status = ocfs2_filecheck_handle(osb,
				entry->fe_ino, OCFS2_FI_FLAG_FILECHECK_FIX);
	else
		entry->fe_status = OCFS2_FILECHECK_ERR_UNSUPPORTED;

	ocfs2_filecheck_done_entry(osb, entry);
}

int ocfs2_filecheck_add_inode(struct ocfs2_super *osb,
				     unsigned long ino)
{
	struct ocfs2_filecheck_entry *entry;
	ssize_t ret = 0;

	entry = kmalloc(sizeof(struct ocfs2_filecheck_entry), GFP_NOFS);
	if (!entry) {
		ret = -ENOMEM;
		goto exit;
	}

	spin_lock(&osb->fc_lock);
	if ((osb->fc_size >= osb->fc_max) &&
	    (osb->fc_done == 0)) {
		mlog(ML_ERROR,
		"Cannot do more file check "
		"since file check queue(%u) is full now\n",
		osb->fc_max);
		ret = -EBUSY;
		kfree(entry);
	} else {
		if ((osb->fc_size >= osb->fc_max) &&
		    (osb->fc_done > 0)) {
			/* Delete the oldest entry which was done,
			 * make sure the entry size in list does
			 * not exceed maximum value
			 */
			BUG_ON(!ocfs2_filecheck_erase_entry(osb));
		}

		entry->fe_ino = ino;
		entry->fe_type = OCFS2_FILECHECK_TYPE_CHK;
		entry->fe_done = 0;
		entry->fe_status = OCFS2_FILECHECK_ERR_INPROGRESS;
		list_add_tail(&entry->fe_list, &osb->file_check_entries);
		osb->fc_size++;
	}
	spin_unlock(&osb->fc_lock);

	if (!ret)
		ocfs2_filecheck_handle_entry(osb, entry);

exit:
	return ret;
}
