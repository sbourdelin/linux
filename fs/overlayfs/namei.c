/*
 * Copyright (C) 2011 Novell Inc.
 * Copyright (C) 2016 Red Hat, Inc.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 */

#include <linux/fs.h>
#include <linux/cred.h>
#include <linux/mount.h>
#include <linux/namei.h>
#include <linux/xattr.h>
#include <linux/ratelimit.h>
#include <linux/exportfs.h>
#include "overlayfs.h"
#include "ovl_entry.h"

struct ovl_lookup_data {
	struct qstr name;
	bool is_dir;
	bool opaque;
	bool stop;
	bool last;
	bool by_path;		/* redirect by path */
	bool by_fh;		/* redirect by file handle */
	char *redirect;		/* path to follow */
	struct ovl_fh *fh;	/* file handle to follow */
};

static int ovl_check_redirect(struct dentry *dentry, struct ovl_lookup_data *d,
			      size_t prelen, const char *post)
{
	int res;
	char *s, *next, *buf = NULL;

	res = vfs_getxattr(dentry, OVL_XATTR_REDIRECT, NULL, 0);
	if (res < 0) {
		if (res == -ENODATA || res == -EOPNOTSUPP)
			return 0;
		goto fail;
	}
	buf = kzalloc(prelen + res + strlen(post) + 1, GFP_TEMPORARY);
	if (!buf)
		return -ENOMEM;

	if (res == 0)
		goto invalid;

	res = vfs_getxattr(dentry, OVL_XATTR_REDIRECT, buf, res);
	if (res < 0)
		goto fail;
	if (res == 0)
		goto invalid;
	if (buf[0] == '/') {
		for (s = buf; *s++ == '/'; s = next) {
			next = strchrnul(s, '/');
			if (s == next)
				goto invalid;
		}
	} else {
		if (strchr(buf, '/') != NULL)
			goto invalid;

		memmove(buf + prelen, buf, res);
		memcpy(buf, d->name.name, prelen);
	}

	strcat(buf, post);
	kfree(d->redirect);
	d->redirect = buf;
	d->name.name = d->redirect;
	d->name.len = strlen(d->redirect);

	return 0;

err_free:
	kfree(buf);
	return 0;
fail:
	pr_warn_ratelimited("overlayfs: failed to get redirect (%i)\n", res);
	goto err_free;
invalid:
	pr_warn_ratelimited("overlayfs: invalid redirect (%s)\n", buf);
	goto err_free;
}

static int ovl_check_redirect_fh(struct dentry *dentry,
				 struct ovl_lookup_data *d)
{
	int res;
	void *buf = NULL;

	res = vfs_getxattr(dentry, OVL_XATTR_FH, NULL, 0);
	if (res < 0) {
		if (res == -ENODATA || res == -EOPNOTSUPP)
			return 0;
		goto fail;
	}
	buf = kzalloc(res, GFP_TEMPORARY);
	if (!buf)
		return -ENOMEM;

	if (res == 0)
		goto fail;

	res = vfs_getxattr(dentry, OVL_XATTR_FH, buf, res);
	if (res < 0 || !ovl_redirect_fh_ok(buf, res))
		goto fail;

	kfree(d->fh);
	d->fh = buf;

	return 0;

err_free:
	kfree(buf);
	return 0;
fail:
	pr_warn_ratelimited("overlayfs: failed to get file handle (%i)\n", res);
	goto err_free;
}

static bool ovl_is_opaquedir(struct dentry *dentry)
{
	int res;
	char val;

	if (!d_is_dir(dentry))
		return false;

	res = vfs_getxattr(dentry, OVL_XATTR_OPAQUE, &val, 1);
	if (res == 1 && val == 'y')
		return true;

	return false;
}

/* Check if p1 is connected with a chain of hashed dentries to p2 */
static bool ovl_is_lookable(struct dentry *p1, struct dentry *p2)
{
	struct dentry *p;

	for (p = p2; !IS_ROOT(p); p = p->d_parent) {
		if (d_unhashed(p))
			return false;
		if (p->d_parent == p1)
			return true;
	}
	return false;
}

/* Check if dentry is reachable from mnt via path lookup */
static int ovl_dentry_under_mnt(void *ctx, struct dentry *dentry)
{
	struct vfsmount *mnt = ctx;

	return ovl_is_lookable(mnt->mnt_root, dentry);
}

static struct dentry *ovl_lookup_fh(struct vfsmount *mnt,
				    const struct ovl_fh *fh)
{
	int bytes = (fh->len - offsetof(struct ovl_fh, fid));

	/*
	 * When redirect_fh is disabled, 'invalid' file handles are stored
	 * to indicate that this entry has been copied up.
	 */
	if (!bytes || (int)fh->type == FILEID_INVALID)
		return ERR_PTR(-ESTALE);

	/*
	 * Several layers can be on the same fs and decoded dentry may be in
	 * either one of those layers. We are looking for a match of dentry
	 * and mnt to find out to which layer the decoded dentry belongs to.
	 */
	return exportfs_decode_fh(mnt, (struct fid *)fh->fid,
				  bytes >> 2, (int)fh->type,
				  ovl_dentry_under_mnt, mnt);
}

static int ovl_lookup_single(struct dentry *base, struct ovl_lookup_data *d,
			     const char *name, unsigned int namelen,
			     size_t prelen, const char *post,
			     struct vfsmount *mnt, struct dentry **ret)
{
	struct dentry *this;
	int err;

	/*
	 * Lookup of upper is with null d->fh.
	 * Lookup of lower is either by_fh with non-null d->fh
	 * or by_path with null d->fh.
	 */
	if (d->fh)
		this = ovl_lookup_fh(mnt, d->fh);
	else
		this = lookup_one_len_unlocked(name, base, namelen);
	if (IS_ERR(this)) {
		err = PTR_ERR(this);
		this = NULL;
		if (err == -ENOENT || err == -ENAMETOOLONG)
			goto out;
		if (d->fh && err == -ESTALE)
			goto out;
		goto out_err;
	}

	/* If found by file handle - don't follow that handle again */
	kfree(d->fh);
	d->fh = NULL;

	if (!this->d_inode)
		goto put_and_out;

	if (ovl_dentry_weird(this)) {
		/* Don't support traversing automounts and other weirdness */
		err = -EREMOTE;
		goto out_err;
	}
	if (ovl_is_whiteout(this)) {
		d->stop = d->opaque = true;
		goto put_and_out;
	}
	if (!d_can_lookup(this)) {
		if (d->is_dir) {
			d->stop = true;
			goto put_and_out;
		}
	} else {
		d->is_dir = true;
		if (!d->last && ovl_is_opaquedir(this)) {
			d->stop = d->opaque = true;
			goto out;
		}
	}
	if (d->last)
		goto out;
	if (d->by_path) {
		err = ovl_check_redirect(this, d, prelen, post);
		if (err)
			goto out_err;
	}
	if (d->by_fh) {
		err = ovl_check_redirect_fh(this, d);
		if (err)
			goto out_err;
	}
	/* No redirect for non-dir means pure upper */
	if (!d->is_dir)
		d->stop = !d->fh && !d->redirect;
out:
	*ret = this;
	return 0;

put_and_out:
	dput(this);
	this = NULL;
	goto out;

out_err:
	dput(this);
	return err;
}

static int ovl_lookup_layer_fh(struct path *path, struct ovl_lookup_data *d,
			       struct dentry **ret)
{
	return ovl_lookup_single(path->dentry, d, "", 0, 0, "", path->mnt, ret);
}

static int ovl_lookup_layer(struct dentry *base, struct ovl_lookup_data *d,
			    struct dentry **ret)
{
	/* Counting down from the end, since the prefix can change */
	size_t rem = d->name.len - 1;
	struct dentry *dentry = NULL;
	int err;

	if (d->name.name[0] != '/')
		return ovl_lookup_single(base, d, d->name.name, d->name.len,
					 0, "", NULL, ret);

	while (!IS_ERR_OR_NULL(base) && d_can_lookup(base)) {
		const char *s = d->name.name + d->name.len - rem;
		const char *next = strchrnul(s, '/');
		size_t thislen = next - s;
		bool end = !next[0];

		/* Verify we did not go off the rails */
		if (WARN_ON(s[-1] != '/'))
			return -EIO;

		err = ovl_lookup_single(base, d, s, thislen,
					d->name.len - rem, next, NULL, &base);
		dput(dentry);
		if (err)
			return err;
		dentry = base;
		if (end)
			break;

		rem -= thislen + 1;

		if (WARN_ON(rem >= d->name.len))
			return -EIO;
	}
	*ret = dentry;
	return 0;
}

/*
 * Returns next layer in stack starting from top.
 * Returns -1 if this is the last layer.
 */
int ovl_path_next(int idx, struct dentry *dentry, struct path *path)
{
	struct ovl_entry *oe = dentry->d_fsdata;

	BUG_ON(idx < 0);
	if (idx == 0) {
		ovl_path_upper(dentry, path);
		if (path->dentry)
			return oe->numlower ? 1 : -1;
		idx++;
	}
	BUG_ON(idx > oe->numlower);
	*path = oe->lowerstack[idx - 1];

	return (idx < oe->numlower) ? idx + 1 : -1;
}

struct dentry *ovl_lookup(struct inode *dir, struct dentry *dentry,
			  unsigned int flags)
{
	struct ovl_entry *oe;
	const struct cred *old_cred;
	struct ovl_fs *ofs = dentry->d_sb->s_fs_info;
	struct ovl_entry *poe = dentry->d_parent->d_fsdata;
	struct ovl_entry *roe = dentry->d_sb->s_root->d_fsdata;
	struct path *stack = NULL;
	struct dentry *upperdir, *upperdentry = NULL;
	unsigned int ctr = 0;
	struct inode *inode = NULL;
	enum ovl_path_type type = 0;
	char *upperredirect = NULL;
	struct dentry *this;
	unsigned int i;
	int err;
	struct ovl_lookup_data d = {
		.name = dentry->d_name,
		.is_dir = false,
		.opaque = false,
		.stop = false,
		.last = !poe->numlower,
		.by_path = true,
		.redirect = NULL,
		.by_fh = true,
		.fh = NULL,
	};

	if (dentry->d_name.len > ofs->namelen)
		return ERR_PTR(-ENAMETOOLONG);

	old_cred = ovl_override_creds(dentry->d_sb);
	upperdir = ovl_upperdentry_dereference(poe);
	if (upperdir) {
		err = ovl_lookup_layer(upperdir, &d, &upperdentry);
		if (err)
			goto out;

		if (upperdentry && unlikely(ovl_dentry_remote(upperdentry))) {
			dput(upperdentry);
			err = -EREMOTE;
			goto out;
		}

		if (d.redirect) {
			upperredirect = kstrdup(d.redirect, GFP_KERNEL);
			if (!upperredirect)
				goto out_put_upper;
			if (d.redirect[0] == '/')
				poe = roe;
		}
		if (d.opaque)
			type |= __OVL_PATH_OPAQUE;
		/* overlay.fh xattr implies this is a copy up */
		if (d.fh)
			type |= __OVL_PATH_COPYUP;
	}

	/*
	 * For now we only support lookup by fh in single layer for directory,
	 * because fallback from lookup by fh to lookup by path in mid layers
	 * for merge directory is not yet implemented.
	 */
	if (!ofs->redirect_fh || (d.is_dir && ofs->numlower > 1)) {
		kfree(d.fh);
		d.fh = NULL;
	}

	if (!d.stop && (poe->numlower || d.fh)) {
		err = -ENOMEM;
		stack = kcalloc(ofs->numlower, sizeof(struct path),
				GFP_TEMPORARY);
		if (!stack)
			goto out_put_upper;
	}

	d.by_path = false;
	for (i = 0; !d.stop && d.fh && i < roe->numlower; i++) {
		struct path lowerpath = poe->lowerstack[i];

		d.last = i == poe->numlower - 1;
		err = ovl_lookup_layer_fh(&lowerpath, &d, &this);
		if (err)
			goto out_put;

		if (!this)
			continue;

		stack[ctr].dentry = this;
		stack[ctr].mnt = lowerpath.mnt;
		ctr++;
		/*
		 * Found by fh - won't lookup by path.
		 * TODO: set d.redirect to dentry_path(this),
		 *       so lookup can continue by path.
		 */
		d.stop = true;
	}

	/* Fallback to lookup lower layers by path */
	d.by_path = true;
	d.by_fh = false;
	kfree(d.fh);
	d.fh = NULL;
	for (i = 0; !d.stop && i < poe->numlower; i++) {
		struct path lowerpath = poe->lowerstack[i];

		d.last = i == poe->numlower - 1;
		err = ovl_lookup_layer(lowerpath.dentry, &d, &this);
		if (err)
			goto out_put;

		if (!this)
			continue;

		stack[ctr].dentry = this;
		stack[ctr].mnt = lowerpath.mnt;
		ctr++;

		/* Do not follow non-dir copy up origin more than once */
		if (d.stop || !d.is_dir)
			break;

		if (d.redirect && d.redirect[0] == '/' && poe != roe) {
			poe = roe;

			/* Find the current layer on the root dentry */
			for (i = 0; i < poe->numlower; i++)
				if (poe->lowerstack[i].mnt == lowerpath.mnt)
					break;
			if (WARN_ON(i == poe->numlower))
				break;
		}
	}

	oe = ovl_alloc_entry(ctr);
	err = -ENOMEM;
	if (!oe)
		goto out_put;

	if (upperdentry || ctr) {
		struct dentry *realdentry;
		struct inode *realinode;

		realdentry = upperdentry ? upperdentry : stack[0].dentry;
		realinode = d_inode(realdentry);

		err = -ENOMEM;
		if (upperdentry && !d_is_dir(upperdentry)) {
			inode = ovl_get_inode(dentry->d_sb, realinode);
		} else {
			inode = ovl_new_inode(dentry->d_sb, realinode->i_mode,
					      realinode->i_rdev);
			if (inode)
				ovl_inode_init(inode, realinode, !!upperdentry);
		}
		if (!inode)
			goto out_free_oe;
		ovl_copyattr(realdentry->d_inode, inode);
	}

	revert_creds(old_cred);
	oe->__type = type;
	oe->redirect = upperredirect;
	oe->__upperdentry = upperdentry;
	memcpy(oe->lowerstack, stack, sizeof(struct path) * ctr);
	kfree(stack);
	kfree(d.redirect);
	dentry->d_fsdata = oe;
	ovl_update_type(dentry, d.is_dir);
	d_add(dentry, inode);

	return NULL;

out_free_oe:
	kfree(oe);
out_put:
	for (i = 0; i < ctr; i++)
		dput(stack[i].dentry);
	kfree(stack);
out_put_upper:
	dput(upperdentry);
	kfree(upperredirect);
out:
	kfree(d.fh);
	kfree(d.redirect);
	revert_creds(old_cred);
	return ERR_PTR(err);
}

bool ovl_lower_positive(struct dentry *dentry)
{
	struct ovl_entry *oe = dentry->d_fsdata;
	struct ovl_entry *poe = dentry->d_parent->d_fsdata;
	const struct qstr *name = &dentry->d_name;
	unsigned int i;
	bool positive = false;
	bool done = false;

	/*
	 * If dentry is negative, then lower is positive iff this is a
	 * whiteout.
	 */
	if (!dentry->d_inode)
		return OVL_TYPE_OPAQUE(oe->__type);

	/* Negative upper -> positive lower */
	if (!oe->__upperdentry)
		return true;

	/* Positive upper -> have to look up lower to see whether it exists */
	for (i = 0; !done && !positive && i < poe->numlower; i++) {
		struct dentry *this;
		struct dentry *lowerdir = poe->lowerstack[i].dentry;

		this = lookup_one_len_unlocked(name->name, lowerdir,
					       name->len);
		if (IS_ERR(this)) {
			switch (PTR_ERR(this)) {
			case -ENOENT:
			case -ENAMETOOLONG:
				break;

			default:
				/*
				 * Assume something is there, we just couldn't
				 * access it.
				 */
				positive = true;
				break;
			}
		} else {
			if (this->d_inode) {
				positive = !ovl_is_whiteout(this);
				done = true;
			}
			dput(this);
		}
	}

	return positive;
}
