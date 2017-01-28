/*
 * Copyright(c) 2017 Intel Corporation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */
#include <linux/pagemap.h>
#include <linux/module.h>
#include <linux/mount.h>
#include <linux/magic.h>
#include <linux/cdev.h>
#include <linux/hash.h>
#include <linux/slab.h>
#include <linux/fs.h>

static int nr_dax = CONFIG_NR_DEV_DAX;
module_param(nr_dax, int, S_IRUGO);
MODULE_PARM_DESC(nr_dax, "max number of dax device instances");

static dev_t dax_devt;
static struct vfsmount *dax_mnt;
static DEFINE_IDA(dax_minor_ida);
static struct kmem_cache *dax_cache __read_mostly;
static struct super_block *dax_superblock __read_mostly;

/**
 * struct dax_inode - anchor object for dax services
 * @inode: core vfs
 * @cdev: optional character interface for "device dax"
 * @private: dax driver private data
 * @alive: !alive + rcu grace period == no new operations / mappings
 */
struct dax_inode {
	struct inode inode;
	struct cdev cdev;
	void *private;
	bool alive;
};

bool dax_inode_alive(struct dax_inode *dax_inode)
{
	RCU_LOCKDEP_WARN(!rcu_read_lock_held(),
			"dax operations require rcu_read_lock()\n");
	return dax_inode->alive;
}
EXPORT_SYMBOL_GPL(dax_inode_alive);

/*
 * Note, rcu is not protecting the liveness of dax_inode, rcu is
 * ensuring that any fault handlers or operations that might have seen
 * dax_inode_alive(), have completed.  Any operations that start after
 * synchronize_rcu() has run will abort upon seeing !dax_inode_alive().
 */
void kill_dax_inode(struct dax_inode *dax_inode)
{
	if (!dax_inode)
		return;

	dax_inode->alive = false;
	synchronize_rcu();
	dax_inode->private = NULL;
}
EXPORT_SYMBOL_GPL(kill_dax_inode);

static struct inode *dax_alloc_inode(struct super_block *sb)
{
	struct dax_inode *dax_inode;

	dax_inode = kmem_cache_alloc(dax_cache, GFP_KERNEL);
	return &dax_inode->inode;
}

static struct dax_inode *to_dax_inode(struct inode *inode)
{
	return container_of(inode, struct dax_inode, inode);
}

static void dax_i_callback(struct rcu_head *head)
{
	struct inode *inode = container_of(head, struct inode, i_rcu);
	struct dax_inode *dax_inode = to_dax_inode(inode);

	ida_simple_remove(&dax_minor_ida, MINOR(inode->i_rdev));
	kmem_cache_free(dax_cache, dax_inode);
}

static void dax_destroy_inode(struct inode *inode)
{
	struct dax_inode *dax_inode = to_dax_inode(inode);

	WARN_ONCE(dax_inode->alive,
			"kill_dax_inode() must be called before final iput()\n");
	call_rcu(&inode->i_rcu, dax_i_callback);
}

static const struct super_operations dax_sops = {
	.statfs = simple_statfs,
	.alloc_inode = dax_alloc_inode,
	.destroy_inode = dax_destroy_inode,
	.drop_inode = generic_delete_inode,
};

static struct dentry *dax_mount(struct file_system_type *fs_type,
		int flags, const char *dev_name, void *data)
{
	return mount_pseudo(fs_type, "dax:", &dax_sops, NULL, DAXFS_MAGIC);
}

static struct file_system_type dax_type = {
	.name = "dax",
	.mount = dax_mount,
	.kill_sb = kill_anon_super,
};

static int dax_test(struct inode *inode, void *data)
{
	dev_t devt = *(dev_t *) data;

	return inode->i_rdev == devt;
}

static int dax_set(struct inode *inode, void *data)
{
	dev_t devt = *(dev_t *) data;

	inode->i_rdev = devt;
	return 0;
}

static struct dax_inode *dax_inode_get(dev_t devt)
{
	struct dax_inode *dax_inode;
	struct inode *inode;

	inode = iget5_locked(dax_superblock, hash_32(devt + DAXFS_MAGIC, 31),
			dax_test, dax_set, &devt);

	if (!inode)
		return NULL;

	dax_inode = to_dax_inode(inode);
	if (inode->i_state & I_NEW) {
		dax_inode->alive = true;
		inode->i_cdev = &dax_inode->cdev;
		inode->i_mode = S_IFCHR;
		inode->i_flags = S_DAX;
		mapping_set_gfp_mask(&inode->i_data, GFP_USER);
		unlock_new_inode(inode);
	}

	return dax_inode;
}

struct dax_inode *alloc_dax_inode(void *private)
{
	struct dax_inode *dax_inode;
	dev_t devt;
	int minor;

	minor = ida_simple_get(&dax_minor_ida, 0, nr_dax, GFP_KERNEL);
	if (minor < 0)
		return NULL;

	devt = MKDEV(MAJOR(dax_devt), minor);
	dax_inode = dax_inode_get(devt);
	if (!dax_inode)
		goto err_inode;

	dax_inode->private = private;
	return dax_inode;

 err_inode:
	ida_simple_remove(&dax_minor_ida, minor);
	return NULL;
}
EXPORT_SYMBOL_GPL(alloc_dax_inode);

void put_dax_inode(struct dax_inode *dax_inode)
{
	if (!dax_inode)
		return;
	iput(&dax_inode->inode);
}
EXPORT_SYMBOL_GPL(put_dax_inode);

/**
 * inode_to_dax_inode: convert a public inode into its dax_inode
 * @inode: An inode with i_cdev pointing to a dax_inode
 */
struct dax_inode *inode_to_dax_inode(struct inode *inode)
{
	struct cdev *cdev = inode->i_cdev;

	return container_of(cdev, struct dax_inode, cdev);
}
EXPORT_SYMBOL_GPL(inode_to_dax_inode);

struct inode *dax_inode_to_inode(struct dax_inode *dax_inode)
{
	return &dax_inode->inode;
}
EXPORT_SYMBOL_GPL(dax_inode_to_inode);

void *dax_inode_get_private(struct dax_inode *dax_inode)
{
	return dax_inode->private;
}
EXPORT_SYMBOL_GPL(dax_inode_get_private);

int dax_inode_register(struct dax_inode *dax_inode,
		const struct file_operations *fops, struct module *owner,
		struct kobject *parent)
{
	struct cdev *cdev = &dax_inode->cdev;
	struct inode *inode = &dax_inode->inode;

	cdev_init(cdev, fops);
	cdev->owner = owner;
	cdev->kobj.parent = parent;
	return cdev_add(cdev, inode->i_rdev, 1);
}
EXPORT_SYMBOL_GPL(dax_inode_register);

void dax_inode_unregister(struct dax_inode *dax_inode)
{
	struct cdev *cdev = &dax_inode->cdev;

	cdev_del(cdev);
}
EXPORT_SYMBOL_GPL(dax_inode_unregister);

static void init_once(void *_dax_inode)
{
	struct dax_inode *dax_inode = _dax_inode;
	struct inode *inode = &dax_inode->inode;

	inode_init_once(inode);
}

static int dax_inode_init(void)
{
	int rc;

	dax_cache = kmem_cache_create("dax_cache", sizeof(struct dax_inode), 0,
			(SLAB_HWCACHE_ALIGN|SLAB_RECLAIM_ACCOUNT|
			 SLAB_MEM_SPREAD|SLAB_ACCOUNT),
			init_once);
	if (!dax_cache)
		return -ENOMEM;

	rc = register_filesystem(&dax_type);
	if (rc)
		goto err_register_fs;

	dax_mnt = kern_mount(&dax_type);
	if (IS_ERR(dax_mnt)) {
		rc = PTR_ERR(dax_mnt);
		goto err_mount;
	}
	dax_superblock = dax_mnt->mnt_sb;

	return 0;

 err_mount:
	unregister_filesystem(&dax_type);
 err_register_fs:
	kmem_cache_destroy(dax_cache);

	return rc;
}

static void dax_inode_exit(void)
{
	kern_unmount(dax_mnt);
	unregister_filesystem(&dax_type);
	kmem_cache_destroy(dax_cache);
}

static int __init dax_fs_init(void)
{
	int rc;

	rc = dax_inode_init();
	if (rc)
		return rc;

	nr_dax = max(nr_dax, 256);
	rc = alloc_chrdev_region(&dax_devt, 0, nr_dax, "dax");
	if (rc)
		dax_inode_exit();
	return rc;
}

static void __exit dax_fs_exit(void)
{
	unregister_chrdev_region(dax_devt, nr_dax);
	ida_destroy(&dax_minor_ida);
	dax_inode_exit();
}

MODULE_AUTHOR("Intel Corporation");
MODULE_LICENSE("GPL v2");
subsys_initcall(dax_fs_init);
module_exit(dax_fs_exit);
