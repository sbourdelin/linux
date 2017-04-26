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
#include <linux/namei.h>
#include <linux/xattr.h>
#include <linux/ratelimit.h>
#include <linux/mount.h>
#include <linux/exportfs.h>
#include "overlayfs.h"
#include "ovl_entry.h"

struct ovl_lookup_data {
	struct qstr name;
	bool is_dir;
	bool opaque;
	bool stop;
	bool last;
	bool by_path;		/* redirect by path: */
	char *redirect;		/* - path to follow */
	bool by_fh;		/* redirect by file handle: */
	struct ovl_fh *fh;	/* - file handle to follow */
	struct ovl_fh *rootfh;	/* - file handle of layer root */
	unsigned char uuid[16];	/* - uuid of layer filesystem */
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

static struct ovl_fh *ovl_get_fh(struct dentry *dentry, const char *name)
{
	int res;
	void *buf = NULL;

	res = vfs_getxattr(dentry, name, NULL, 0);
	if (res <= 0) {
		if (res == -ENODATA || res == -EOPNOTSUPP)
			return 0;
		goto fail;
	}
	buf = kzalloc(res, GFP_TEMPORARY);
	if (!buf) {
		res = -ENOMEM;
		goto fail;
	}

	res = vfs_getxattr(dentry, name, buf, res);
	if (res < 0 || !ovl_redirect_fh_ok(buf, res))
		goto fail;

	return (struct ovl_fh *)buf;

err_free:
	kfree(buf);
	return NULL;
fail:
	pr_warn_ratelimited("overlayfs: failed to get %s (%i)\n",
			    name, res);
	goto err_free;
}

static int ovl_check_redirect_fh(struct dentry *dentry,
				 struct ovl_lookup_data *d)
{
	int res;

	kfree(d->fh);
	d->fh = ovl_get_fh(dentry, OVL_XATTR_ORIGIN_FH);
	kfree(d->rootfh);
	d->rootfh = ovl_get_fh(dentry, OVL_XATTR_ORIGIN_ROOT);

	res = vfs_getxattr(dentry, OVL_XATTR_ORIGIN_UUID, d->uuid,
			   sizeof(d->uuid));
	if (res == sizeof(d->uuid))
		return 0;

	if (res != -ENODATA && res != -EOPNOTSUPP) {
		pr_warn_ratelimited("overlayfs: failed to get %s (%i)\n",
				    OVL_XATTR_ORIGIN_UUID, res);
	}

	memset(d->uuid, 0, sizeof(d->uuid));
	return 0;
}

static void ovl_reset_redirect_fh(struct ovl_lookup_data *d)
{
	kfree(d->fh);
	d->fh = NULL;
	kfree(d->rootfh);
	d->rootfh = NULL;
	memset(d->uuid, 0, sizeof(d->uuid));
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

/* Update ovl_lookup_data struct from dentry found in layer */
static int ovl_lookup_data(struct dentry *this, struct ovl_lookup_data *d,
			   size_t prelen, const char *post,
			   struct dentry **ret)
{
	int err;

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

static int ovl_lookup_single(struct dentry *base, struct ovl_lookup_data *d,
			     const char *name, unsigned int namelen,
			     size_t prelen, const char *post,
			     struct dentry **ret)
{
	struct dentry *this = lookup_one_len_unlocked(name, base, namelen);
	int err;

	if (IS_ERR(this)) {
		err = PTR_ERR(this);
		*ret = NULL;
		if (err == -ENOENT || err == -ENAMETOOLONG)
			return 0;
		return err;
	}

	return ovl_lookup_data(this, d, prelen, post, ret);
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
					 0, "", ret);

	while (!IS_ERR_OR_NULL(base) && d_can_lookup(base)) {
		const char *s = d->name.name + d->name.len - rem;
		const char *next = strchrnul(s, '/');
		size_t thislen = next - s;
		bool end = !next[0];

		/* Verify we did not go off the rails */
		if (WARN_ON(s[-1] != '/'))
			return -EIO;

		err = ovl_lookup_single(base, d, s, thislen,
					d->name.len - rem, next, &base);
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

static struct dentry *ovl_decode_fh(struct vfsmount *mnt,
				    const struct ovl_fh *fh,
				    int (*acceptable)(void *, struct dentry *))
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
				  acceptable, mnt);
}

/* Check if dentry belongs to this layer */
static int ovl_dentry_in_layer(void *mnt, struct dentry *dentry)
{
	return is_subdir(dentry, ((struct vfsmount *)mnt)->mnt_root);
}

/* Lookup by file handle in a lower layer mounted at @mnt */
static int ovl_lookup_layer_fh(struct vfsmount *mnt, struct ovl_lookup_data *d,
			       struct dentry **ret)
{
	struct dentry *this = ovl_decode_fh(mnt, d->fh, ovl_dentry_in_layer);
	int err;

	if (IS_ERR(this)) {
		err = PTR_ERR(this);
		*ret = NULL;
		if (err == -ESTALE)
			return 0;
		return err;
	}

	/* If found by file handle - don't follow that handle again */
	ovl_reset_redirect_fh(d);
	return ovl_lookup_data(this, d, 0, "", ret);
}

static int ovl_is_dir(void *ctx, struct dentry *dentry)
{
	return d_is_dir(dentry);
}

/* Find lower layer index by layer root file handle and uuid */
static int ovl_find_layer_by_fh(struct dentry *dentry, struct ovl_lookup_data *d)
{
	struct ovl_entry *roe = dentry->d_sb->s_root->d_fsdata;
	struct super_block *lower_sb = ovl_same_lower_sb(dentry->d_sb);
	struct dentry *this;
	int i;

	/*
	 * For now, we only support lookup by fh for all lower layers on the
	 * same sb.  Not all filesystems set sb->s_uuid.  For those who don't
	 * this code will compare zeros, which at least ensures us that the
	 * file handles are not crossing from filesystem with sb->s_uuid to
	 * a filesystem without sb->s_uuid and vice versa.
	 */
	if (!lower_sb || memcmp(lower_sb->s_uuid, &d->uuid, sizeof(d->uuid)))
		return -1;

	/* Don't bother verifying rootfh with a single lower layer */
	if (roe->numlower == 1)
		return 0;

	/*
	 * Layer root dentries are pinned, there are no aliases for dirs, and
	 * all lower layers are on the same sb.  If rootfh is correct,
	 * exportfs_decode_fh() will find it in dcache and return the only
	 * instance, regardless of the mnt argument and we can compare the
	 * returned pointer with the pointers in lowerstack.
	 */
	this = ovl_decode_fh(roe->lowerstack[0].mnt, d->rootfh, ovl_is_dir);
	if (IS_ERR(this))
		return -1;

	for (i = 0; i < roe->numlower; i++) {
		if (this == roe->lowerstack[i].dentry)
			break;
	}

	dput(this);
	return i < roe->numlower ? i : -1;
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
		.rootfh = NULL,
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
	if (!ofs->redirect_fh || (d.is_dir && ofs->numlower > 1))
		ovl_reset_redirect_fh(&d);

	if (!d.stop && (poe->numlower || d.fh)) {
		err = -ENOMEM;
		stack = kcalloc(ofs->numlower, sizeof(struct path),
				GFP_TEMPORARY);
		if (!stack)
			goto out_put_upper;
	}

	/* Try to lookup lower layers by file handle */
	d.by_path = false;
	if (!d.stop && d.fh) {
		struct vfsmount *lowermnt;
		int layer = ovl_find_layer_by_fh(dentry, &d);

		if (layer < 0 || layer >= roe->numlower)
			goto lookup_by_path;

		lowermnt = roe->lowerstack[layer].mnt;
		d.last = true;
		err = ovl_lookup_layer_fh(lowermnt, &d, &this);
		if (err)
			goto out_put;

		if (!this)
			goto lookup_by_path;

		stack[ctr].dentry = this;
		stack[ctr].mnt = lowermnt;
		ctr++;
		/*
		 * Found by fh - won't lookup by path.
		 * TODO: set d.redirect to dentry_path(this), so
		 *       lookup can continue by path in next layers
		 */
		d.stop = true;
	}

lookup_by_path:
	/* Fallback to lookup lower layers by path */
	d.by_path = true;
	d.by_fh = false;
	ovl_reset_redirect_fh(&d);
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
	ovl_reset_redirect_fh(&d);
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
