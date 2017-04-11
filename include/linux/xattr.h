/*
  File: linux/xattr.h

  Extended attributes handling.

  Copyright (C) 2001 by Andreas Gruenbacher <a.gruenbacher@computer.org>
  Copyright (c) 2001-2002 Silicon Graphics, Inc.  All Rights Reserved.
  Copyright (c) 2004 Red Hat, Inc., James Morris <jmorris@redhat.com>
*/
#ifndef _LINUX_XATTR_H
#define _LINUX_XATTR_H


#include <linux/slab.h>
#include <linux/types.h>
#include <linux/spinlock.h>
#include <uapi/linux/xattr.h>

struct inode;
struct dentry;

/*
 * struct xattr_handler: When @name is set, match attributes with exactly that
 * name.  When @prefix is set instead, match attributes with that prefix and
 * with a non-empty suffix.
 */
struct xattr_handler {
	const char *name;
	const char *prefix;
	int flags;      /* fs private flags */
	bool (*list)(struct dentry *dentry);
	int (*get)(const struct xattr_handler *, struct dentry *dentry,
		   struct inode *inode, const char *name, void *buffer,
		   size_t size);
	int (*set)(const struct xattr_handler *, struct dentry *dentry,
		   struct inode *inode, const char *name, const void *buffer,
		   size_t size, int flags);
};

struct xattr {
	const char *name;
	void *value;
	size_t value_len;
};

ssize_t xattr_getsecurity(struct inode *, const char *, void *, size_t);

#ifdef CONFIG_XATTR_SYSCALLS

ssize_t __vfs_getxattr(struct dentry *, struct inode *, const char *, void *, size_t);
ssize_t vfs_getxattr(struct dentry *, const char *, void *, size_t);
ssize_t vfs_listxattr(struct dentry *d, char *list, size_t size);
int __vfs_setxattr(struct dentry *, struct inode *, const char *, const void *, size_t, int);
int __vfs_setxattr_noperm(struct dentry *, const char *, const void *, size_t, int);
int vfs_setxattr(struct dentry *, const char *, const void *, size_t, int);
int __vfs_removexattr(struct dentry *, const char *);
int vfs_removexattr(struct dentry *, const char *);

ssize_t generic_listxattr(struct dentry *dentry, char *buffer, size_t buffer_size);
ssize_t vfs_getxattr_alloc(struct dentry *dentry, const char *name,
			   char **xattr_value, size_t size, gfp_t flags);

#else

static inline ssize_t __vfs_getxattr(struct dentry *dentry, struct inode *inode,
		const char *name, void *value, size_t size)
{
	return -EOPNOTSUPP;
}

static inline ssize_t vfs_getxattr(struct dentry *dentry, const char *name,
		void *value, size_t size)
{
	return -EOPNOTSUPP;
}

static inline ssize_t vfs_listxattr(struct dentry *dentry, char *list, size_t size)
{
	return -EOPNOTSUPP;
}

static inline int __vfs_setxattr(struct dentry *dentry, struct inode *inode,
		const char *name, const void *value, size_t size, int flags)
{
	return -EOPNOTSUPP;
}

static inline int __vfs_setxattr_noperm(struct dentry *dentry, const char *name,
		const void *value, size_t size, int flags)
{
	return -EOPNOTSUPP;
}

static inline ssize_t vfs_setxattr(struct dentry *dentry, const char *name,
		const void *value, size_t size, int flags)
{
	return -EOPNOTSUPP;
}

static inline int __vfs_removexattr(struct dentry *dentry, const char *name)
{
	return -EOPNOTSUPP;
}

static inline int vfs_removexattr(struct dentry *dentry, const char *name)
{
	return -EOPNOTSUPP;
}

static inline ssize_t generic_listxattr(struct dentry *dentry, char *buffer,
		size_t buffer_size)
{
	return -EOPNOTSUPP;
}

static inline ssize_t vfs_getxattr_alloc(struct dentry *dentry,
		const char *name, char **xattr_value, size_t xattr_size,
		gfp_t flags)
{
	return -EOPNOTSUPP;
}

#endif  /* CONFIG_XATTR_SYSCALLS */


static inline const char *xattr_prefix(const struct xattr_handler *handler)
{
	return handler->prefix ?: handler->name;
}

/**
 * xattr_full_name  -  Compute full attribute name from suffix
 *
 * @handler:	handler of the xattr_handler operation
 * @name:	name passed to the xattr_handler operation
 *
 * The get and set xattr handler operations are called with the remainder of
 * the attribute name after skipping the handler's prefix: for example, "foo"
 * is passed to the get operation of a handler with prefix "user." to get
 * attribute "user.foo".  The full name is still "there" in the name though.
 *
 * Note: the list xattr handler operation when called from the vfs is passed a
 * NULL name; some file systems use this operation internally, with varying
 * semantics.
 */
static inline const char *xattr_full_name(const struct xattr_handler *handler,
		const char *name)
{
	size_t prefix_len = strlen(xattr_prefix(handler));

	return name - prefix_len;
}

struct simple_xattrs {
	struct list_head head;
	spinlock_t lock;
};

struct simple_xattr {
	struct list_head list;
	char *name;
	size_t size;
	char value[0];
};

/*
 * initialize the simple_xattrs structure
 */
static inline void simple_xattrs_init(struct simple_xattrs *xattrs)
{
	INIT_LIST_HEAD(&xattrs->head);
	spin_lock_init(&xattrs->lock);
}

/*
 * free all the xattrs
 */
static inline void simple_xattrs_free(struct simple_xattrs *xattrs)
{
	struct simple_xattr *xattr, *node;

	list_for_each_entry_safe(xattr, node, &xattrs->head, list) {
		kfree(xattr->name);
		kfree(xattr);
	}
}

#ifdef CONFIG_XATTR_SYSCALLS

struct simple_xattr *simple_xattr_alloc(const void *value, size_t size);
int simple_xattr_get(struct simple_xattrs *xattrs, const char *name,
		     void *buffer, size_t size);
int simple_xattr_set(struct simple_xattrs *xattrs, const char *name,
		     const void *value, size_t size, int flags);
ssize_t simple_xattr_list(struct inode *inode, struct simple_xattrs *xattrs, char *buffer,
			  size_t size);
void simple_xattr_list_add(struct simple_xattrs *xattrs,
			   struct simple_xattr *new_xattr);

#else

static inline struct simple_xattr *simple_xattr_alloc(const void *value,
		size_t size)
{
	return NULL;
}

static inline int simple_xattr_get(struct simple_xattrs *xattrs,
		const char *name, void *buffer, size_t size)
{
	return -ENODATA;
}

static inline int simple_xattr_set(struct simple_xattrs *xattrs,
		const char *name, const void *value, size_t size, int flags)
{
	return -ENODATA;
}

static inline ssize_t simple_xattr_list(struct inode *inode,
		struct simple_xattrs *xattrs, char *buffer, size_t size)
{
	return -ERANGE;
}

static inline void simple_xattr_list_add(struct simple_xattrs *xattrs,
		struct simple_xattr *new_xattr)
{
}

#endif  /* CONFIG_XATTR_SYSCALLS */

#endif	/* _LINUX_XATTR_H */
