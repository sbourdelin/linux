/* -*- mode: c; c-basic-offset: 8; -*-
 * vim: noexpandtab sw=8 ts=8 sts=0:
 *
 * filecheck.h
 *
 * Online file check.
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


#ifndef FILECHECK_H
#define FILECHECK_H

#include <linux/types.h>
#include <linux/list.h>


/* File check errno */
enum {
	OCFS2_FILECHECK_ERR_SUCCESS = 0,	/* Success */
	OCFS2_FILECHECK_ERR_FAILED = 1000,	/* Other failure */
	OCFS2_FILECHECK_ERR_INPROGRESS,		/* In progress */
	OCFS2_FILECHECK_ERR_READONLY,		/* Read only */
	OCFS2_FILECHECK_ERR_INJBD,		/* Buffer in jbd */
	OCFS2_FILECHECK_ERR_INVALIDINO,		/* Invalid ino */
	OCFS2_FILECHECK_ERR_BLOCKECC,		/* Block ecc */
	OCFS2_FILECHECK_ERR_BLOCKNO,		/* Block number */
	OCFS2_FILECHECK_ERR_VALIDFLAG,		/* Inode valid flag */
	OCFS2_FILECHECK_ERR_GENERATION,		/* Inode generation */
	OCFS2_FILECHECK_ERR_UNSUPPORTED		/* Unsupported */
};

#define OCFS2_FILECHECK_ERR_START	OCFS2_FILECHECK_ERR_FAILED
#define OCFS2_FILECHECK_ERR_END		OCFS2_FILECHECK_ERR_UNSUPPORTED
#define OCFS2_FILECHECK_MAXSIZE         100
#define OCFS2_FILECHECK_MINSIZE         10

/* File check operation type */
#define OCFS2_FILECHECK_TYPE_CHK  	1   /* Check a file(inode) */
#define OCFS2_FILECHECK_TYPE_FIX  	2   /* Fix a file(inode) */

int ocfs2_filecheck_create_sysfs(struct super_block *sb);
int ocfs2_filecheck_remove_sysfs(struct super_block *sb);
int ocfs2_filefix_inode(struct ocfs2_super *osb, unsigned long ino);
int ocfs2_filecheck_add_inode(struct ocfs2_super *osb, unsigned long ino);
int ocfs2_filecheck_set_max_entries(struct ocfs2_super *osb, int num);
int ocfs2_filecheck_show(struct ocfs2_super *osb, unsigned int type, char *buf);

#endif  /* FILECHECK_H */
