/*
 * (C) 2001 Clemson University and The University of Chicago
 *
 * See COPYING in top-level directory.
 */

/*
 *  Linux VFS inode operations.
 */

#include <linux/bvec.h>
#include "protocol.h"
#include "orangefs-kernel.h"
#include "orangefs-bufmap.h"

static int orangefs_setattr_size(struct inode *inode, struct iattr *iattr)
{
	struct orangefs_inode_s *orangefs_inode = ORANGEFS_I(inode);
	struct orangefs_kernel_op_s *new_op;
	int ret = -EINVAL;

	gossip_debug(GOSSIP_INODE_DEBUG,
		     "%s: %pU: Handle is %pU | fs_id %d | size is %llu\n",
		     __func__,
		     get_khandle_from_ino(inode),
		     &orangefs_inode->refn.khandle,
		     orangefs_inode->refn.fs_id,
		     iattr->ia_size);

	truncate_setsize(inode, iattr->ia_size);

	new_op = op_alloc(ORANGEFS_VFS_OP_TRUNCATE);
	if (!new_op)
		return -ENOMEM;

	new_op->upcall.req.truncate.refn = orangefs_inode->refn;
	new_op->upcall.req.truncate.size = (__s64) iattr->ia_size;

	ret = service_operation(new_op, __func__,
				get_interruptible_flag(inode));

	/*
	 * the truncate has no downcall members to retrieve, but
	 * the status value tells us if it went through ok or not
	 */
	gossip_debug(GOSSIP_INODE_DEBUG,
		     "orangefs: orangefs_truncate got return value of %d\n",
		     ret);

	op_release(new_op);

	if (ret != 0)
		return ret;

	return ret;
}

/*
 * Change attributes of an object referenced by dentry.
 */
int orangefs_setattr(struct dentry *dentry, struct iattr *iattr)
{
	int r;
	if (iattr->ia_valid & ATTR_SIZE) {
		/* XXX: ATTR_CTIME_SET and ATTR_MTIME_SET */
		if (i_size_read(d_inode(dentry)) != iattr->ia_size)
			iattr->ia_valid |= ATTR_CTIME | ATTR_MTIME;
	}
	r = simple_setattr(dentry, iattr);
	if (r)
		return r;
	if (iattr->ia_valid & ATTR_SIZE) {
		r = orangefs_setattr_size(d_inode(dentry), iattr);
		if (r)
			return r;
	}
	if (iattr->ia_valid & ATTR_MODE) {
		return posix_acl_chmod(d_inode(dentry),
		    d_inode(dentry)->i_mode);
	}
	return 0;
}

/*
 * Obtain attributes of an object given a dentry
 */
int orangefs_getattr(const struct path *path, struct kstat *stat,
		     u32 request_mask, unsigned int flags)
{
	struct inode *inode = path->dentry->d_inode;
	struct orangefs_inode_s *orangefs_inode = NULL;

	gossip_debug(GOSSIP_INODE_DEBUG,
		     "orangefs_getattr: called on %pd\n",
		     path->dentry);

	generic_fillattr(inode, stat);

	/* override block size reported to stat */
	orangefs_inode = ORANGEFS_I(inode);
	stat->blksize = orangefs_inode->blksize;
	stat->result_mask = STATX_BASIC_STATS;
	return 0;
}

/* ORANGEDS2 implementation of VFS inode operations for files */
const struct inode_operations orangefs_file_inode_operations = {
	.get_acl = orangefs_get_acl,
	.set_acl = orangefs_set_acl,
	.setattr = orangefs_setattr,
	.getattr = orangefs_getattr,
	.listxattr = orangefs_listxattr,
};

static int orangefs_init_iops(struct inode *inode)
{
	switch (inode->i_mode & S_IFMT) {
	case S_IFREG:
		inode->i_op = &orangefs_file_inode_operations;
		inode->i_fop = &orangefs_file_operations;
		inode->i_data.a_ops = &orangefs_address_operations;
		inode->i_blkbits = PAGE_SHIFT;
		return 0;
	case S_IFLNK:
		inode->i_op = &orangefs_symlink_inode_operations;
		return 0;
	case S_IFDIR:
		inode->i_op = &orangefs_dir_inode_operations;
		inode->i_fop = &orangefs_dir_operations;
		return 0;
	}
	BUG_ON(1);
}

/*
 * Given a ORANGEFS object identifier (fsid, handle), convert it into a ino_t type
 * that will be used as a hash-index from where the handle will
 * be searched for in the VFS hash table of inodes.
 */
static inline ino_t orangefs_handle_hash(struct orangefs_object_kref *ref)
{
	if (!ref)
		return 0;
	return orangefs_khandle_to_ino(&(ref->khandle));
}

/*
 * Called to set up an inode from iget5_locked.
 */
static int orangefs_set_inode(struct inode *inode, void *data)
{
	struct orangefs_object_kref *ref = (struct orangefs_object_kref *) data;
	ORANGEFS_I(inode)->refn.fs_id = ref->fs_id;
	ORANGEFS_I(inode)->refn.khandle = ref->khandle;
	return 0;
}

/*
 * Called to determine if handles match.
 */
static int orangefs_test_inode(struct inode *inode, void *data)
{
	struct orangefs_object_kref *ref = (struct orangefs_object_kref *) data;
	struct orangefs_inode_s *orangefs_inode = NULL;

	orangefs_inode = ORANGEFS_I(inode);
	return (!ORANGEFS_khandle_cmp(&(orangefs_inode->refn.khandle), &(ref->khandle))
		&& orangefs_inode->refn.fs_id == ref->fs_id);
}

/*
 * Front-end to lookup the inode-cache maintained by the VFS using the ORANGEFS
 * file handle.
 *
 * @sb: the file system super block instance.
 * @ref: The ORANGEFS object for which we are trying to locate an inode structure.
 */
struct inode *orangefs_iget(struct super_block *sb, struct orangefs_object_kref *ref)
{
	struct inode *inode = NULL;
	unsigned long hash;
	int error;

	hash = orangefs_handle_hash(ref);
	inode = iget5_locked(sb, hash, orangefs_test_inode, orangefs_set_inode, ref);
	if (!inode || !(inode->i_state & I_NEW))
		return inode;

	error = orangefs_inode_getattr(inode, 1, 1, STATX_ALL);
	if (error) {
		iget_failed(inode);
		return ERR_PTR(error);
	}

	inode->i_ino = hash;	/* needed for stat etc */
	orangefs_init_iops(inode);
	unlock_new_inode(inode);

	gossip_debug(GOSSIP_INODE_DEBUG,
		     "iget handle %pU, fsid %d hash %ld i_ino %lu\n",
		     &ref->khandle,
		     ref->fs_id,
		     hash,
		     inode->i_ino);

	return inode;
}

/*
 * Allocate an inode for a newly created file and insert it into the inode hash.
 */
struct inode *orangefs_new_inode(struct super_block *sb, struct inode *dir,
		int mode, dev_t dev, struct orangefs_object_kref *ref)
{
	unsigned long hash = orangefs_handle_hash(ref);
	struct inode *inode;
	int error;

	gossip_debug(GOSSIP_INODE_DEBUG,
		     "%s:(sb is %p | MAJOR(dev)=%u | MINOR(dev)=%u mode=%o)\n",
		     __func__,
		     sb,
		     MAJOR(dev),
		     MINOR(dev),
		     mode);

	inode = new_inode(sb);
	if (!inode)
		return NULL;

	orangefs_set_inode(inode, ref);
	inode->i_ino = hash;	/* needed for stat etc */

	error = orangefs_inode_getattr(inode, 1, 1, STATX_ALL);
	if (error)
		goto out_iput;

	orangefs_init_iops(inode);

	inode->i_mode = mode;
	inode->i_uid = current_fsuid();
	inode->i_gid = current_fsgid();
	inode->i_atime = inode->i_mtime = inode->i_ctime = current_time(inode);
	inode->i_size = 0;
	inode->i_rdev = dev;

	error = insert_inode_locked4(inode, hash, orangefs_test_inode, ref);
	if (error < 0)
		goto out_iput;

	gossip_debug(GOSSIP_INODE_DEBUG,
		     "Initializing ACL's for inode %pU\n",
		     get_khandle_from_ino(inode));
	orangefs_init_acl(inode, dir);
	return inode;

out_iput:
	iput(inode);
	return ERR_PTR(error);
}
