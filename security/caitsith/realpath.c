/*
 * security/caitsith/realpath.c
 *
 * Copyright (C) 2005-2012  NTT DATA CORPORATION
 */

#include "caitsith.h"

/**
 * cs_realpath - Returns realpath(3) of the given pathname but ignores chroot'ed root.
 *
 * @path: Pointer to "struct path".
 *
 * Returns the realpath of the given @path on success, NULL otherwise.
 *
 * This function uses kzalloc(), so caller must kfree() if this function
 * didn't return NULL.
 */
char *cs_realpath(const struct path *path)
{
	char *buf = NULL;
	char *name = NULL;
	unsigned int buf_len = PAGE_SIZE / 2;
	struct dentry *dentry = path->dentry;
	struct super_block *sb;

	if (!dentry)
		return NULL;
	sb = dentry->d_sb;
	while (1) {
		char *pos;

		buf_len <<= 1;
		kfree(buf);
		buf = kmalloc(buf_len, GFP_NOFS);
		if (!buf)
			break;
		/* To make sure that pos is '\0' terminated. */
		buf[buf_len - 1] = '\0';
		/* For "pipe:[\$]". */
		if (dentry->d_op && dentry->d_op->d_dname)
			pos = dentry->d_op->d_dname(dentry, buf, buf_len - 1);
		else
			pos = d_absolute_path(path, buf, buf_len);
		if (IS_ERR(pos))
			continue;
		name = cs_encode(pos);
		break;
	}
	kfree(buf);
	if (!name)
		cs_warn_oom(__func__);
	return name;
}

/**
 * cs_encode2 - Encode binary string to ascii string.
 *
 * @str:     String in binary format. Maybe NULL.
 * @str_len: Size of @str in byte.
 *
 * Returns pointer to @str in ascii format on success, NULL otherwise.
 *
 * This function uses kzalloc(), so caller must kfree() if this function
 * didn't return NULL.
 */
static char *cs_encode2(const char *str, int str_len)
{
	int i;
	int len;
	const char *p = str;
	char *cp;
	char *cp0;

	if (!p)
		return NULL;
	len = str_len;
	for (i = 0; i < str_len; i++) {
		const unsigned char c = p[i];

		if (!(c > ' ' && c < 127 && c != '\\'))
			len += 3;
	}
	len++;
	cp = kzalloc(len, GFP_NOFS);
	if (!cp)
		return NULL;
	cp0 = cp;
	p = str;
	for (i = 0; i < str_len; i++) {
		const unsigned char c = p[i];

		if (c > ' ' && c < 127 && c != '\\') {
			*cp++ = c;
		} else {
			*cp++ = '\\';
			*cp++ = (c >> 6) + '0';
			*cp++ = ((c >> 3) & 7) + '0';
			*cp++ = (c & 7) + '0';
		}
	}
	return cp0;
}

/**
 * cs_encode - Encode binary string to ascii string.
 *
 * @str: String in binary format. Maybe NULL.
 *
 * Returns pointer to @str in ascii format on success, NULL otherwise.
 *
 * This function uses kzalloc(), so caller must kfree() if this function
 * didn't return NULL.
 */
char *cs_encode(const char *str)
{
	return str ? cs_encode2(str, strlen(str)) : NULL;
}

/**
 * cs_const_part_length - Evaluate the initial length without a pattern in a token.
 *
 * @filename: The string to evaluate. Maybe NULL.
 *
 * Returns the initial length without a pattern in @filename.
 */
static int cs_const_part_length(const char *filename)
{
	char c;
	int len = 0;

	if (!filename)
		return 0;
	while (1) {
		c = *filename++;
		if (!c)
			break;
		if (c != '\\') {
			len++;
			continue;
		}
		c = *filename++;
		switch (c) {
		case '0':   /* "\ooo" */
		case '1':
		case '2':
		case '3':
			c = *filename++;
			if (c < '0' || c > '7')
				break;
			c = *filename++;
			if (c < '0' || c > '7')
				break;
			len += 4;
			continue;
		}
		break;
	}
	return len;
}

/**
 * cs_fill_path_info - Fill in "struct cs_path_info" members.
 *
 * @ptr: Pointer to "struct cs_path_info" to fill in.
 *
 * Returns nothing.
 *
 * The caller sets "struct cs_path_info"->name.
 */
void cs_fill_path_info(struct cs_path_info *ptr)
{
	const char *name = ptr->name;
	const int len = strlen(name);

	ptr->total_len = len;
	ptr->const_len = cs_const_part_length(name);
	ptr->hash = full_name_hash(NULL, name, len);
}

/**
 * cs_get_exe - Get cs_realpath() of current process.
 *
 * Returns the cs_realpath() of current process on success, NULL otherwise.
 *
 * This function uses kzalloc(), so the caller must kfree()
 * if this function didn't return NULL.
 */
char *cs_get_exe(void)
{
	struct mm_struct *mm = current->mm;
	char *cp;
	struct file *exe_file;

	if (current->flags & PF_KTHREAD)
		return kstrdup("<kernel>", GFP_NOFS);
	if (!mm)
		goto task_has_no_mm;
	exe_file = get_mm_exe_file(mm);
	if (!exe_file)
		goto task_has_no_mm;
	cp = cs_realpath(&exe_file->f_path);
	fput(exe_file);
	return cp;
task_has_no_mm:
	return kstrdup("<unknown>", GFP_NOFS);
}

/**
 * cs_get_exename - Get cs_realpath() of current process.
 *
 * @buf: Pointer to "struct cs_path_info".
 *
 * Returns true on success, false otherwise.
 *
 * This function uses kzalloc(), so the caller must kfree()
 * if this function returned true.
 */
bool cs_get_exename(struct cs_path_info *buf)
{
	buf->name = cs_get_exe();
	if (buf->name) {
		cs_fill_path_info(buf);
		return true;
	}
	return false;
}
