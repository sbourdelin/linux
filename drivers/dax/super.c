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
#include <linux/dax.h>
#include <linux/fs.h>

static int nr_dax = CONFIG_NR_DEV_DAX;
module_param(nr_dax, int, S_IRUGO);
MODULE_PARM_DESC(nr_dax, "max number of dax device instances");

static dev_t dax_devt;
DEFINE_STATIC_SRCU(dax_srcu);
static struct vfsmount *dax_mnt;
static DEFINE_IDA(dax_minor_ida);
static struct kmem_cache *dax_cache __read_mostly;
static struct super_block *dax_superblock __read_mostly;

#define DAX_HASH_SIZE (PAGE_SIZE / sizeof(struct hlist_head))
static struct hlist_head dax_host_list[DAX_HASH_SIZE];
static DEFINE_SPINLOCK(dax_host_lock);

int dax_read_lock(void)
{
	return srcu_read_lock(&dax_srcu);
}
EXPORT_SYMBOL_GPL(dax_read_lock);

void dax_read_unlock(int id)
{
	srcu_read_unlock(&dax_srcu, id);
}
EXPORT_SYMBOL_GPL(dax_read_unlock);

/**
 * struct dax_inode - anchor object for dax services
 * @inode: core vfs
 * @cdev: optional character interface for "device dax"
 * @host: optional name for lookups where the device path is not available
 * @private: dax driver private data
 * @alive: !alive + rcu grace period == no new operations / mappings
 */
struct dax_inode {
	struct hlist_node list;
	struct inode inode;
	struct cdev cdev;
	const char *host;
	void *private;
	bool alive;
	const struct dax_operations *ops;
};

long dax_direct_access(struct dax_inode *dax_inode, phys_addr_t dev_addr,
		void **kaddr, pfn_t *pfn, long size)
{
	long avail;

	/*
	 * The device driver is allowed to sleep, in order to make the
	 * memory directly accessible.
	 */
	might_sleep();

	if (!dax_inode)
		return -EOPNOTSUPP;

	if (!dax_inode_alive(dax_inode))
		return -ENXIO;

	if (size < 0)
		return size;

	if (dev_addr % PAGE_SIZE)
		return -EINVAL;

	avail = dax_inode->ops->direct_access(dax_inode, dev_addr, kaddr, pfn,
			size);
	if (!avail)
		return -ERANGE;
	if (avail > 0 && avail & ~PAGE_MASK)
		return -ENXIO;
	return min(avail, size);
}
EXPORT_SYMBOL_GPL(dax_direct_access);

bool dax_inode_alive(struct dax_inode *dax_inode)
{
	lockdep_assert_held(&dax_srcu);
	return dax_inode->alive;
}
EXPORT_SYMBOL_GPL(dax_inode_alive);

static int dax_host_hash(const char *host)
{
	return hashlen_hash(hashlen_string("DAX", host)) % DAX_HASH_SIZE;
}

/*
 * Note, rcu is not protecting the liveness of dax_inode, rcu is
 * ensuring that any fault handlers or operations that might have seen
 * dax_inode_alive(), have completed.  Any operations that start after
 * synchronize_srcu() has run will abort upon seeing !dax_inode_alive().
 */
void kill_dax_inode(struct dax_inode *dax_inode)
{
	if (!dax_inode)
		return;

	dax_inode->alive = false;

	spin_lock(&dax_host_lock);
	if (!hlist_unhashed(&dax_inode->list))
		hlist_del_init(&dax_inode->list);
	spin_unlock(&dax_host_lock);

	synchronize_srcu(&dax_srcu);
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

	kfree(dax_inode->host);
	dax_inode->host = NULL;
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

static void dax_add_host(struct dax_inode *dax_inode, const char *host)
{
	int hash;

	INIT_HLIST_NODE(&dax_inode->list);
	if (!host)
		return;

	dax_inode->host = host;
	hash = dax_host_hash(host);
	spin_lock(&dax_host_lock);
	hlist_add_head(&dax_inode->list, &dax_host_list[hash]);
	spin_unlock(&dax_host_lock);
}

struct dax_inode *alloc_dax_inode(void *private, const char *__host,
		const struct dax_operations *ops)
{
	struct dax_inode *dax_inode;
	const char *host;
	dev_t devt;
	int minor;

	host = kstrdup(__host, GFP_KERNEL);
	if (__host && !host)
		return NULL;

	minor = ida_simple_get(&dax_minor_ida, 0, nr_dax, GFP_KERNEL);
	if (minor < 0)
		goto err_minor;

	devt = MKDEV(MAJOR(dax_devt), minor);
	dax_inode = dax_inode_get(devt);
	if (!dax_inode)
		goto err_inode;

	dax_add_host(dax_inode, host);
	dax_inode->ops = ops;
	dax_inode->private = private;
	return dax_inode;

 err_inode:
	ida_simple_remove(&dax_minor_ida, minor);
 err_minor:
	kfree(host);
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
 * dax_get_by_host() - temporary lookup mechanism for filesystem-dax
 * @host: alternate name for the inode registered by a dax driver
 */
struct dax_inode *dax_get_by_host(const char *host)
{
	struct dax_inode *dax_inode, *found = NULL;
	int hash, id;

	if (!host)
		return NULL;

	hash = dax_host_hash(host);

	id = dax_read_lock();
	spin_lock(&dax_host_lock);
	hlist_for_each_entry(dax_inode, &dax_host_list[hash], list) {
		if (!dax_inode_alive(dax_inode)
				|| strcmp(host, dax_inode->host) != 0)
			continue;

		if (igrab(&dax_inode->inode))
			found = dax_inode;
		break;
	}
	spin_unlock(&dax_host_lock);
	dax_read_unlock(id);

	return found;
}
EXPORT_SYMBOL_GPL(dax_get_by_host);

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
