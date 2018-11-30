// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2011-2018, The Linux Foundation. All rights reserved.
// Copyright (c) 2018, Linaro Limited

#include <linux/cdev.h>
#include <linux/completion.h>
#include <linux/device.h>
#include <linux/dma-buf.h>
#include <linux/dma-mapping.h>
#include <linux/idr.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/rpmsg.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <uapi/linux/fastrpc.h>

#define ADSP_DOMAIN_ID (0)
#define MDSP_DOMAIN_ID (1)
#define SDSP_DOMAIN_ID (2)
#define CDSP_DOMAIN_ID (3)
#define FASTRPC_DEV_MAX		4 /* adsp, mdsp, slpi, cdsp*/
#define FASTRPC_MAX_SESSIONS	9 /*8 compute, 1 cpz*/
#define FASTRPC_ALIGN		128
#define FASTRPC_MAX_FDLIST	16
#define FASTRPC_MAX_CRCLIST	64
#define FASTRPC_PHYS(p)	(p & 0xffffffff)
#define FASTRPC_CTX_MAX (256)
#define FASTRPC_CTXID_MASK (0xFF0)
#define FASTRPC_DEVICE_NAME	"fastrpc"

/* Retrives number of input buffers from the scalars parameter */
#define REMOTE_SCALARS_INBUFS(sc)        (((sc) >> 16) & 0x0ff)

/* Retrives number of output buffers from the scalars parameter */
#define REMOTE_SCALARS_OUTBUFS(sc)       (((sc) >> 8) & 0x0ff)

/* Retrives number of input handles from the scalars parameter */
#define REMOTE_SCALARS_INHANDLES(sc)     (((sc) >> 4) & 0x0f)

/* Retrives number of output handles from the scalars parameter */
#define REMOTE_SCALARS_OUTHANDLES(sc)    ((sc) & 0x0f)

#define REMOTE_SCALARS_LENGTH(sc)	(REMOTE_SCALARS_INBUFS(sc) +\
					REMOTE_SCALARS_OUTBUFS(sc) +\
					REMOTE_SCALARS_INHANDLES(sc) +\
					REMOTE_SCALARS_OUTHANDLES(sc))

#define FASTRPC_BUILD_SCALARS(attr, method, in, out, oin, oout) \
		((((uint32_t)   (attr) & 0x7) << 29) | \
		(((uint32_t) (method) & 0x1f) << 24) | \
		(((uint32_t)     (in) & 0xff) << 16) | \
		(((uint32_t)    (out) & 0xff) <<  8) | \
		(((uint32_t)    (oin) & 0x0f) <<  4) | \
		((uint32_t)   (oout) & 0x0f))

#define FASTRPC_SCALARS(method, in, out) \
		FASTRPC_BUILD_SCALARS(0, method, in, out, 0, 0)
#define cdev_to_cctx(d) container_of(d, struct fastrpc_channel_ctx, cdev)

static const char *domains[FASTRPC_DEV_MAX] = { "adsp", "mdsp",
						"sdsp", "cdsp"};
static dev_t fastrpc_major;
static struct class *fastrpc_class;

struct fastrpc_invoke_header {
	uint64_t ctx;		/* invoke caller context */
	uint32_t handle;	/* handle to invoke */
	uint32_t sc;		/* scalars structure describing the data */
};

struct fastrpc_phy_page {
	uint64_t addr;		/* physical address */
	uint64_t size;		/* size of contiguous region */
};

struct fastrpc_invoke_buf {
	int num;		/* number of contiguous regions */
	int pgidx;		/* index to start of contiguous region */
};

struct fastrpc_invoke {
	struct fastrpc_invoke_header header;
	struct fastrpc_phy_page page; /* list of pages address */
};

struct fastrpc_msg {
	uint32_t pid;		/* process group id */
	uint32_t tid;		/* thread id */
	struct fastrpc_invoke invoke;
};

struct fastrpc_invoke_rsp {
	uint64_t ctx;		/* invoke caller context */
	int retval;		/* invoke return value */
};

struct fastrpc_buf {
	struct fastrpc_user *fl;
	struct device *dev;
	void *virt;
	uint64_t phys;
	size_t size;
};

struct fastrpc_map {
	struct list_head node;
	struct fastrpc_user *fl;
	int fd;
	struct dma_buf *buf;
	struct sg_table *table;
	struct dma_buf_attachment *attach;
	uint64_t phys;
	size_t size;
	uintptr_t va;
	size_t len;
	struct kref refcount;
};

struct fastrpc_invoke_ctx {
	struct fastrpc_user *fl;
	struct list_head node; /* list of ctxs */
	struct completion work;
	int retval;
	int pid;
	int tgid;
	uint32_t sc;
	struct fastrpc_msg msg;
	uint64_t ctxid;
	size_t used_sz;

	remote_arg_t *lpra;
	unsigned int *attrs;
	int *fds;
	uint32_t *crc;

	remote_arg64_t *rpra;
	struct fastrpc_map **maps;
	struct fastrpc_buf *buf;
};

struct fastrpc_session_ctx {
	struct device *dev;
	int sid;
	bool used;
	bool valid;
	bool secure;
};

struct fastrpc_channel_ctx {
	int domain_id;
	int sesscount;
	struct rpmsg_device *rpdev;
	struct fastrpc_session_ctx session[FASTRPC_MAX_SESSIONS];
	spinlock_t lock;
	struct idr ctx_idr;
	struct list_head users;
	struct cdev cdev;
	struct device dev;
};

struct fastrpc_user {
	struct list_head user;
	struct list_head maps;
	struct list_head pending;

	struct fastrpc_channel_ctx *cctx;
	struct fastrpc_session_ctx *sctx;
	struct fastrpc_buf *init_mem;

	int tgid;
	int pd;
	/* Lock for lists */
	spinlock_t lock;
	/* lock for allocations */
	struct mutex mutex;
	struct device *dev;
};

static void fastrpc_free_map(struct kref *ref)
{
	struct fastrpc_map *map;

	map = container_of(ref, struct fastrpc_map, refcount);

	list_del(&map->node);

	if (map->table) {
		dma_buf_unmap_attachment(map->attach, map->table,
				DMA_BIDIRECTIONAL);
		dma_buf_detach(map->buf, map->attach);
		dma_buf_put(map->buf);
	}

	kfree(map);
}

static void fastrpc_map_put(struct fastrpc_map *map)
{
	struct fastrpc_user *fl;

	if (map) {
		fl = map->fl;
		mutex_lock(&fl->mutex);
		kref_put(&map->refcount, fastrpc_free_map);
		mutex_unlock(&fl->mutex);
	}
}

static int fastrpc_map_get(struct fastrpc_user *fl, int fd,
			     uintptr_t va, size_t len,
			     struct fastrpc_map **ppmap)
{
	struct fastrpc_map *map = NULL, *n;

	mutex_lock(&fl->mutex);
	list_for_each_entry_safe(map, n, &fl->maps, node) {
		if (map->fd == fd) {
			kref_get(&map->refcount);
			*ppmap = map;
			mutex_unlock(&fl->mutex);
			return 0;
		}
	}
	mutex_unlock(&fl->mutex);

	return -ENOENT;
}

static void fastrpc_buf_free(struct fastrpc_buf *buf)
{
	dma_free_coherent(buf->dev, buf->size, buf->virt,
				FASTRPC_PHYS(buf->phys));
	kfree(buf);
}

static int fastrpc_buf_alloc(struct fastrpc_user *fl, struct device *dev,
			     size_t size, struct fastrpc_buf **obuf)
{
	struct fastrpc_buf *buf;

	buf = kzalloc(sizeof(*buf), GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	buf->fl = fl;
	buf->virt = NULL;
	buf->phys = 0;
	buf->size = size;
	buf->dev = dev;

	buf->virt = dma_alloc_coherent(dev, buf->size, (dma_addr_t *)&buf->phys,
				       GFP_KERNEL);
	if (!buf->virt)
		return -ENOMEM;

	if (fl->sctx && fl->sctx->sid)
		buf->phys += ((uint64_t)fl->sctx->sid << 32);

	*obuf = buf;

	return 0;
}

static void fastrpc_context_free(struct fastrpc_invoke_ctx *ctx)
{
	struct fastrpc_channel_ctx *cctx = ctx->fl->cctx;
	struct fastrpc_user *user = ctx->fl;
	int scalars = REMOTE_SCALARS_LENGTH(ctx->sc);
	int i;

	spin_lock(&user->lock);
	list_del(&ctx->node);
	spin_unlock(&user->lock);

	for (i = 0; i < scalars; i++) {
		if (ctx->maps[i])
			fastrpc_map_put(ctx->maps[i]);
	}

	if (ctx->buf)
		fastrpc_buf_free(ctx->buf);

	spin_lock(&cctx->lock);
	idr_remove(&cctx->ctx_idr, ctx->ctxid >> 4);
	spin_unlock(&cctx->lock);

	kfree(ctx);
}

static struct fastrpc_invoke_ctx *fastrpc_context_alloc(
					struct fastrpc_user *user,
					uint32_t kernel,
					struct fastrpc_ioctl_invoke *inv)
{
	struct fastrpc_channel_ctx *cctx = user->cctx;
	struct fastrpc_invoke_ctx *ctx = NULL;
	int bufs, size, ret;
	int err = 0;

	bufs = REMOTE_SCALARS_LENGTH(inv->sc);
	size = (sizeof(*ctx->lpra) + sizeof(*ctx->maps) +
		sizeof(*ctx->fds)  + sizeof(*ctx->attrs)) * bufs;

	ctx = kzalloc(sizeof(*ctx) + size, GFP_KERNEL);
	if (!ctx)
		return ERR_PTR(-ENOMEM);

	INIT_LIST_HEAD(&ctx->node);
	ctx->fl = user;
	ctx->maps = (struct fastrpc_map **)(&ctx[1]);
	ctx->lpra = (remote_arg_t *)(&ctx->maps[bufs]);
	ctx->fds = (int *)(&ctx->lpra[bufs]);
	ctx->attrs = (unsigned int *)(&ctx->fds[bufs]);

	if (!kernel) {
		if (copy_from_user(ctx->lpra,
				     (void const __user *)inv->pra,
				     bufs * sizeof(*ctx->lpra))) {
			err = -EFAULT;
			goto err;
		}

		if (inv->fds) {
			if (copy_from_user(ctx->fds,
					     (void const __user *)inv->fds,
					     bufs * sizeof(*ctx->fds))) {
				err = -EFAULT;
				goto err;
			}
		}
		if (inv->attrs) {
			if (copy_from_user(
					ctx->attrs,
					(void const __user *)inv->attrs,
					bufs * sizeof(*ctx->attrs))) {
				err = -EFAULT;
				goto err;
			}
		}
	} else {
		memcpy(ctx->lpra, inv->pra, bufs * sizeof(*ctx->lpra));
		if (inv->fds)
			memcpy(ctx->fds, inv->fds,
			       bufs * sizeof(*ctx->fds));
		if (inv->attrs)
			memcpy(ctx->attrs, inv->attrs,
			       bufs * sizeof(*ctx->attrs));
	}

	ctx->crc = (uint32_t *)inv->crc;
	ctx->sc = inv->sc;
	ctx->retval = -1;
	ctx->pid = current->pid;
	ctx->tgid = user->tgid;
	init_completion(&ctx->work);

	spin_lock(&user->lock);
	list_add_tail(&ctx->node, &user->pending);
	spin_unlock(&user->lock);

	spin_lock(&cctx->lock);
	ret = idr_alloc_cyclic(&cctx->ctx_idr, ctx, 1,
				FASTRPC_CTX_MAX, GFP_ATOMIC);
	if (ret < 0) {
		spin_unlock(&cctx->lock);
		err = ret;
		goto err_idr;
	}
	ctx->ctxid = ret << 4;
	spin_unlock(&cctx->lock);

	return ctx;
err_idr:
	spin_lock(&user->lock);
	list_del(&ctx->node);
	spin_unlock(&user->lock);
err:
	kfree(ctx);

	return ERR_PTR(err);
}

static int fastrpc_map_create(struct fastrpc_user *fl, int fd, uintptr_t va,
			       size_t len, struct fastrpc_map **ppmap)
{
	struct fastrpc_session_ctx *sess = fl->sctx;
	struct fastrpc_map *map = NULL;
	int err = 0;

	if (!fastrpc_map_get(fl, fd, va, len, ppmap))
		return 0;

	map = kzalloc(sizeof(*map), GFP_KERNEL);
	if (!map)
		return -ENOMEM;

	INIT_LIST_HEAD(&map->node);
	map->fl = fl;
	map->fd = fd;
	map->buf = dma_buf_get(fd);
	if (!map->buf) {
		err = -EINVAL;
		goto get_err;
	}

	map->attach = dma_buf_attach(map->buf, sess->dev);
	if (IS_ERR(map->attach)) {
		dev_err(sess->dev, "Failed to attach dmabuf\n");
		err = PTR_ERR(map->attach);
		goto attach_err;
	}

	map->table = dma_buf_map_attachment(map->attach,
					    DMA_BIDIRECTIONAL);
	if (IS_ERR(map->table)) {
		err = PTR_ERR(map->table);
		goto map_err;
	}

	map->phys = sg_dma_address(map->table->sgl);
	map->phys += ((uint64_t)fl->sctx->sid << 32);
	map->size = len;
	map->va = (uintptr_t)sg_virt(map->table->sgl);
	map->len = len;
	kref_init(&map->refcount);

	spin_lock(&fl->lock);
	list_add_tail(&map->node, &fl->maps);
	spin_unlock(&fl->lock);
	*ppmap = map;

	return 0;

map_err:
	dma_buf_detach(map->buf, map->attach);
attach_err:
	dma_buf_put(map->buf);
get_err:
	kfree(map);

	return err;
}

static inline struct fastrpc_invoke_buf *fastrpc_invoke_buf_start(
							remote_arg64_t *pra,
							uint32_t sc)
{
	return (struct fastrpc_invoke_buf *)(&pra[REMOTE_SCALARS_LENGTH(sc)]);
}

static inline struct fastrpc_phy_page *fastrpc_phy_page_start(uint32_t sc,
						struct fastrpc_invoke_buf *buf)
{
	return (struct fastrpc_phy_page *)(&buf[REMOTE_SCALARS_LENGTH(sc)]);
}

static int fastrpc_get_args(uint32_t kernel, struct fastrpc_invoke_ctx *ctx)
{
	remote_arg64_t *rpra;
	remote_arg_t *lpra = ctx->lpra;
	struct fastrpc_invoke_buf *list;
	struct fastrpc_phy_page *pages;
	uint32_t sc = ctx->sc;
	uintptr_t args;
	size_t rlen = 0, copylen = 0, metalen = 0;
	int inbufs, handles, bufs, i, err = 0;
	uint64_t *fdlist;
	uint32_t *crclist;

	inbufs = REMOTE_SCALARS_INBUFS(sc);
	bufs = inbufs + REMOTE_SCALARS_OUTBUFS(sc);
	handles = REMOTE_SCALARS_INHANDLES(sc) + REMOTE_SCALARS_OUTHANDLES(sc);
	metalen = (bufs + handles) * (sizeof(remote_arg64_t) +
		  sizeof(struct fastrpc_invoke_buf) +
		  sizeof(struct fastrpc_phy_page)) +
		  sizeof(uint64_t) * FASTRPC_MAX_FDLIST +
		  sizeof(uint32_t) * FASTRPC_MAX_CRCLIST;

	copylen = metalen;

	for (i = 0; i < bufs + handles; ++i) {
		uintptr_t buf = (uintptr_t)lpra[i].buf.pv;
		size_t len = lpra[i].buf.len;

		if (i < bufs) {
			if (ctx->fds[i] && (ctx->fds[i] != -1))
				fastrpc_map_create(ctx->fl, ctx->fds[i], buf,
						len, &ctx->maps[i]);

			if (!len)
				continue;

			if (ctx->maps[i])
				continue;

			copylen = ALIGN(copylen, FASTRPC_ALIGN);
			copylen += len;
		} else {
			err = fastrpc_map_create(ctx->fl, ctx->fds[i], 0,
						  0, &ctx->maps[i]);
			if (err)
				goto bail;
		}
	}
	ctx->used_sz = copylen;

	/* allocate new buffer */
	if (copylen) {
		err = fastrpc_buf_alloc(ctx->fl, ctx->fl->sctx->dev,
					copylen, &ctx->buf);
		if (err)
			goto bail;
	}

	/* copy metadata */
	rpra = ctx->buf->virt;
	ctx->rpra = rpra;
	list = fastrpc_invoke_buf_start(rpra, sc);
	pages = fastrpc_phy_page_start(sc, list);
	args = (uintptr_t)ctx->buf->virt + metalen;
	fdlist = (uint64_t *)&pages[bufs + handles];
	memset(fdlist, 0, sizeof(uint32_t)*FASTRPC_MAX_FDLIST);
	crclist = (uint32_t *)&fdlist[FASTRPC_MAX_FDLIST];
	memset(crclist, 0, sizeof(uint32_t)*FASTRPC_MAX_CRCLIST);
	rlen = copylen - metalen;

	for (i = 0; i < bufs; ++i) {
		struct fastrpc_map *map = ctx->maps[i];
		size_t len = lpra[i].buf.len;
		size_t mlen;

		if (len)
			list[i].num = 1;
		else
			list[i].num = 0;

		list[i].pgidx = i;

		rpra[i].buf.pv = 0;
		rpra[i].buf.len = len;
		if (!len)
			continue;
		if (map) {
			uintptr_t offset = 0;
			uint64_t num = roundup(len,
					       PAGE_SIZE) / PAGE_SIZE;
			int idx = list[i].pgidx;

			pages[idx].addr = map->phys + offset;
			pages[idx].size = num << PAGE_SHIFT;
			rpra[i].buf.pv =
				(uint64_t)((uintptr_t)lpra[i].buf.pv);
		} else {
			rlen -= ALIGN(args, FASTRPC_ALIGN) - args;
			args = ALIGN(args, FASTRPC_ALIGN);
			mlen = len;
			if (rlen < mlen)
				goto bail;

			rpra[i].buf.pv = (args);
			pages[list[i].pgidx].addr = ctx->buf->phys +
							(copylen - rlen);
			pages[list[i].pgidx].addr = pages[list[i].pgidx].addr &
							PAGE_MASK;
			pages[list[i].pgidx].size = roundup(len, PAGE_SIZE);

			if (i < inbufs) {
				if (!kernel) {
					err = copy_from_user(
					(void *)rpra[i].buf.pv,
					(void const __user *)lpra[i].buf.pv,
					len);
					if (err)
						goto bail;
				} else {
					memcpy((void *)rpra[i].buf.pv,
					       lpra[i].buf.pv, len);
				}
			}
			args = args + mlen;
			rlen -= mlen;
		}
	}

	for (i = bufs; i < handles; ++i) {
		struct fastrpc_map *map = ctx->maps[i];
		size_t len = lpra[i].buf.len;

		if (len)
			list[i].num = 1;
		else
			list[i].num = 0;

		list[i].pgidx = i;

		pages[i].addr = map->phys;
		pages[i].size = map->size;
		rpra[i].dma.fd = ctx->fds[i];
		rpra[i].dma.len = len;
		rpra[i].dma.offset = (uint32_t)(uintptr_t)lpra[i].buf.pv;
	}

bail:
	return err;
}

static int fastrpc_put_args(struct fastrpc_invoke_ctx *ctx,
			    uint32_t kernel, remote_arg_t *upra)
{
	remote_arg64_t *rpra = ctx->rpra;
	int i, inbufs, outbufs, handles;
	struct fastrpc_invoke_buf *list;
	struct fastrpc_phy_page *pages;
	struct fastrpc_map *mmap;
	uint32_t sc = ctx->sc;
	uint64_t *fdlist;
	uint32_t *crclist;
	int err = 0;

	inbufs = REMOTE_SCALARS_INBUFS(sc);
	outbufs = REMOTE_SCALARS_OUTBUFS(sc);
	handles = REMOTE_SCALARS_INHANDLES(sc) + REMOTE_SCALARS_OUTHANDLES(sc);
	list = fastrpc_invoke_buf_start(ctx->rpra, sc);
	pages = fastrpc_phy_page_start(sc, list);
	fdlist = (uint64_t *)(pages + inbufs + outbufs + handles);
	crclist = (uint32_t *)(fdlist + FASTRPC_MAX_FDLIST);

	for (i = inbufs; i < inbufs + outbufs; ++i) {
		if (!ctx->maps[i]) {
			if (!kernel)
				err =
				copy_to_user((void __user *)ctx->lpra[i].buf.pv,
				       (void *)rpra[i].buf.pv, rpra[i].buf.len);
			else
				memcpy(ctx->lpra[i].buf.pv,
				       (void *)rpra[i].buf.pv, rpra[i].buf.len);

			if (err)
				goto bail;
		} else {
			fastrpc_map_put(ctx->maps[i]);
			ctx->maps[i] = NULL;
		}
	}

	if (inbufs + outbufs + handles) {
		for (i = 0; i < FASTRPC_MAX_FDLIST; i++) {
			if (!fdlist[i])
				break;
			if (!fastrpc_map_get(ctx->fl, (int)fdlist[i], 0,
					       0, &mmap))
				fastrpc_map_put(mmap);
		}
	}

	if (ctx->crc && crclist) {
		if (!kernel)
			err = copy_to_user((void __user *)ctx->crc, crclist,
					FASTRPC_MAX_CRCLIST*sizeof(uint32_t));
		else
			memcpy(ctx->crc, crclist,
					FASTRPC_MAX_CRCLIST*sizeof(uint32_t));
	}

bail:
	return err;
}

static int fastrpc_invoke_send(struct fastrpc_session_ctx *sctx,
			       struct fastrpc_invoke_ctx *ctx,
			       uint32_t kernel, uint32_t handle)
{
	struct fastrpc_channel_ctx *cctx;
	struct fastrpc_user *fl = ctx->fl;
	struct fastrpc_msg *msg = &ctx->msg;

	cctx = fl->cctx;
	msg->pid = fl->tgid;
	msg->tid = current->pid;

	if (kernel)
		msg->pid = 0;

	msg->invoke.header.ctx = ctx->ctxid | fl->pd;
	msg->invoke.header.handle = handle;
	msg->invoke.header.sc = ctx->sc;
	msg->invoke.page.addr = ctx->buf ? ctx->buf->phys : 0;
	msg->invoke.page.size = roundup(ctx->used_sz, PAGE_SIZE);

	return rpmsg_send(cctx->rpdev->ept, (void *)msg, sizeof(*msg));
}

static int fastrpc_internal_invoke(struct fastrpc_user *fl,
				   uint32_t kernel,
				   struct fastrpc_ioctl_invoke *inv)
{
	struct fastrpc_invoke_ctx *ctx = NULL;
	int err = 0;

	if (!fl->sctx)
		return -EINVAL;

	ctx = fastrpc_context_alloc(fl, kernel, inv);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);

	if (REMOTE_SCALARS_LENGTH(ctx->sc)) {
		err = fastrpc_get_args(kernel, ctx);
		if (err)
			goto bail;
	}

	err = fastrpc_invoke_send(fl->sctx, ctx, kernel, inv->handle);
	if (err)
		goto bail;

	err = wait_for_completion_interruptible(&ctx->work);
	if (err)
		goto bail;

	err = ctx->retval;
	if (err)
		goto bail;

	err = fastrpc_put_args(ctx, kernel, inv->pra);
	if (err)
		goto bail;
bail:
	if (ctx)
		fastrpc_context_free(ctx);

	return err;
}
static struct fastrpc_session_ctx *fastrpc_session_alloc(
					struct fastrpc_channel_ctx *cctx,
					int secure)
{
	struct fastrpc_session_ctx *session = NULL;
	int i;

	spin_lock(&cctx->lock);
	for (i = 0; i < cctx->sesscount; i++) {
		if (!cctx->session[i].used && cctx->session[i].valid &&
				cctx->session[i].secure == secure) {
			cctx->session[i].used = true;
			session = &cctx->session[i];
			break;
		}
	}
	spin_unlock(&cctx->lock);

	return session;
}

static void fastrpc_session_free(struct fastrpc_channel_ctx *cctx,
				 struct fastrpc_session_ctx *session)
{
	spin_lock(&cctx->lock);
	session->used = false;
	spin_unlock(&cctx->lock);
}

static const struct of_device_id fastrpc_match_table[] = {
	{ .compatible = "qcom,fastrpc-compute-cb", },
	{}
};

static int fastrpc_device_release(struct inode *inode, struct file *file)
{
	struct fastrpc_user *fl = (struct fastrpc_user *)file->private_data;
	struct fastrpc_channel_ctx *cctx = cdev_to_cctx(inode->i_cdev);
	struct fastrpc_invoke_ctx *ctx, *n;
	struct fastrpc_map *map, *m;

	spin_lock(&cctx->lock);
	list_del(&fl->user);
	spin_unlock(&cctx->lock);

	if (fl->init_mem)
		fastrpc_buf_free(fl->init_mem);

	list_for_each_entry_safe(ctx, n, &fl->pending, node)
		fastrpc_context_free(ctx);

	list_for_each_entry_safe(map, m, &fl->maps, node)
		fastrpc_map_put(map);

	if (fl->sctx)
		fastrpc_session_free(fl->cctx, fl->sctx);

	mutex_destroy(&fl->mutex);
	kfree(fl);
	file->private_data = NULL;

	return 0;
}

static int fastrpc_device_open(struct inode *inode, struct file *filp)
{
	struct fastrpc_channel_ctx *cctx = cdev_to_cctx(inode->i_cdev);
	struct fastrpc_user *fl = NULL;

	fl = kzalloc(sizeof(*fl), GFP_KERNEL);
	if (!fl)
		return -ENOMEM;

	filp->private_data = fl;

	spin_lock_init(&fl->lock);
	mutex_init(&fl->mutex);
	INIT_LIST_HEAD(&fl->pending);
	INIT_LIST_HEAD(&fl->maps);
	INIT_LIST_HEAD(&fl->user);

	fl->tgid = current->tgid;
	fl->cctx = cctx;
	fl->dev = &cctx->rpdev->dev;
	spin_lock(&cctx->lock);
	list_add_tail(&fl->user, &cctx->users);
	spin_unlock(&cctx->lock);

	return 0;
}

static long fastrpc_device_ioctl(struct file *file, unsigned int cmd,
				 unsigned long arg)
{
	struct fastrpc_user *fl = (struct fastrpc_user *)file->private_data;
	struct fastrpc_channel_ctx *cctx = fl->cctx;
	char __user *argp = (char __user *)arg;
	int err;

	if (!fl->sctx) {
		fl->sctx = fastrpc_session_alloc(cctx, 0);
		if (!fl->sctx)
			return -ENOENT;
	}

	switch (cmd) {
	case FASTRPC_IOCTL_INVOKE: {
		struct fastrpc_ioctl_invoke inv;

		inv.fds = NULL;
		inv.attrs = NULL;
		inv.crc = NULL;
		err = copy_from_user(&inv, argp, sizeof(inv));
		if (err)
			goto bail;
		err = fastrpc_internal_invoke(fl, 0, &inv);
		if (err)
			goto bail;
		break;
		}
default:
		err = -ENOTTY;
		pr_info("bad ioctl: %d\n", cmd);
		break;
	}
bail:
	return err;
}

static const struct file_operations fastrpc_fops = {
	.open = fastrpc_device_open,
	.release = fastrpc_device_release,
	.unlocked_ioctl = fastrpc_device_ioctl,
};

static int fastrpc_cb_probe(struct platform_device *pdev)
{
	struct fastrpc_channel_ctx *cctx;
	struct fastrpc_session_ctx *sess;
	struct device *dev = &pdev->dev;
	int i, sessions = 0;

	cctx = dev_get_drvdata(dev->parent);
	if (!cctx)
		return -EINVAL;

	of_property_read_u32(dev->of_node, "nsessions", &sessions);

	spin_lock(&cctx->lock);
	sess = &cctx->session[cctx->sesscount];
	sess->used = false;
	sess->valid = true;
	sess->dev = dev;
	dev_set_drvdata(dev, sess);
	sess->secure = of_property_read_bool(dev->of_node, "secured");

	if (of_property_read_u32(dev->of_node, "reg", &sess->sid))
		dev_err(dev, "FastRPC Session ID not specified in DT\n");

	if (sessions > 0) {
		struct fastrpc_session_ctx *dup_sess;

		for (i = 1; i < sessions; i++) {
			if (cctx->sesscount++ >= FASTRPC_MAX_SESSIONS)
				break;
			dup_sess = &cctx->session[cctx->sesscount];
			memcpy(dup_sess, sess, sizeof(*dup_sess));
		}
	}
	cctx->sesscount++;
	spin_unlock(&cctx->lock);
	dma_set_mask(dev, DMA_BIT_MASK(32));

	return 0;
}

static int fastrpc_cb_remove(struct platform_device *pdev)
{
	struct fastrpc_channel_ctx *cctx = dev_get_drvdata(pdev->dev.parent);
	struct fastrpc_session_ctx *sess = dev_get_drvdata(&pdev->dev);
	int i;

	spin_lock(&cctx->lock);
	for (i = 1; i < FASTRPC_MAX_SESSIONS; i++) {
		if (cctx->session[i].sid == sess->sid) {
			cctx->session[i].valid = false;
			cctx->sesscount--;
		}
	}
	spin_unlock(&cctx->lock);

	return 0;
}

static struct platform_driver fastrpc_cb_driver = {
	.probe = fastrpc_cb_probe,
	.remove = fastrpc_cb_remove,
	.driver = {
		.name = "fastrpc",
		.owner = THIS_MODULE,
		.of_match_table = fastrpc_match_table,
		.suppress_bind_attrs = true,
	},
};

static void fastrpc_cdev_release_device(struct device *dev)
{
	struct fastrpc_channel_ctx *data = dev_get_drvdata(dev->parent);

	cdev_del(&data->cdev);
}

static int fastrpc_rpmsg_probe(struct rpmsg_device *rpdev)
{
	struct device *rdev = &rpdev->dev;
	struct fastrpc_channel_ctx *data;
	struct device *dev;
	int err, domain_id;

	data = devm_kzalloc(rdev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	err = of_property_read_u32(rdev->of_node, "reg", &domain_id);
	if (err) {
		dev_err(rdev, "FastRPC Domain ID not specified in DT\n");
		return err;
	}

	if (domain_id > CDSP_DOMAIN_ID) {
		dev_err(rdev, "FastRPC Invalid Domain ID %d\n", domain_id);
		return -EINVAL;
	}

	dev = &data->dev;
	device_initialize(dev);
	dev->parent = &rpdev->dev;
	dev->class = fastrpc_class;

	cdev_init(&data->cdev, &fastrpc_fops);
	data->cdev.owner = THIS_MODULE;
	dev->devt = MKDEV(MAJOR(fastrpc_major), domain_id);
	dev->id = domain_id;
	dev_set_name(&data->dev, "fastrpc-%s", domains[domain_id]);
	dev->release = fastrpc_cdev_release_device;

	err = cdev_device_add(&data->cdev, &data->dev);
	if (err)
		goto cdev_err;

	dev_set_drvdata(&rpdev->dev, data);
	dma_set_mask_and_coherent(rdev, DMA_BIT_MASK(32));
	INIT_LIST_HEAD(&data->users);
	spin_lock_init(&data->lock);
	idr_init(&data->ctx_idr);
	data->domain_id = domain_id;
	data->rpdev = rpdev;

	return of_platform_populate(rdev->of_node, NULL, NULL, rdev);

cdev_err:
	put_device(dev);
	return err;
}

static void fastrpc_notify_users(struct fastrpc_user *user)
{
	struct fastrpc_invoke_ctx *ctx, *n;

	spin_lock(&user->lock);
	list_for_each_entry_safe(ctx, n, &user->pending, node)
		complete(&ctx->work);
	spin_unlock(&user->lock);
}

static void fastrpc_rpmsg_remove(struct rpmsg_device *rpdev)
{
	struct fastrpc_channel_ctx *cctx = dev_get_drvdata(&rpdev->dev);
	struct fastrpc_user *user, *n;

	spin_lock(&cctx->lock);
	list_for_each_entry_safe(user, n, &cctx->users, user)
		fastrpc_notify_users(user);
	spin_unlock(&cctx->lock);

	device_del(&cctx->dev);
	put_device(&cctx->dev);
	of_platform_depopulate(&rpdev->dev);
	kfree(cctx);
}

static int fastrpc_rpmsg_callback(struct rpmsg_device *rpdev, void *data,
				  int len, void *priv, u32 addr)
{
	struct fastrpc_channel_ctx *cctx = dev_get_drvdata(&rpdev->dev);
	struct fastrpc_invoke_rsp *rsp = data;
	struct fastrpc_invoke_ctx *ctx;
	unsigned long flags;
	int ctxid;

	if (rsp && len < sizeof(*rsp)) {
		dev_err(&rpdev->dev, "invalid response or context\n");
		return -EINVAL;
	}

	ctxid = (uint32_t)((rsp->ctx & FASTRPC_CTXID_MASK) >> 4);

	spin_lock_irqsave(&cctx->lock, flags);
	ctx = idr_find(&cctx->ctx_idr, ctxid);
	spin_unlock_irqrestore(&cctx->lock, flags);

	if (!ctx) {
		dev_err(&rpdev->dev, "No context ID matches response\n");
		return -ENOENT;
	}

	ctx->retval = rsp->retval;
	complete(&ctx->work);

	return 0;
}

static const struct of_device_id fastrpc_rpmsg_of_match[] = {
	{ .compatible = "qcom,fastrpc" },
	{ },
};
MODULE_DEVICE_TABLE(of, fastrpc_rpmsg_of_match);

static struct rpmsg_driver fastrpc_driver = {
	.probe = fastrpc_rpmsg_probe,
	.remove = fastrpc_rpmsg_remove,
	.callback = fastrpc_rpmsg_callback,
	.drv = {
		.name = "qcom,msm_fastrpc_rpmsg",
		.of_match_table = fastrpc_rpmsg_of_match,
	},
};

static int fastrpc_init(void)
{
	int ret;

	ret = alloc_chrdev_region(&fastrpc_major, 0, FASTRPC_DEV_MAX,
				  FASTRPC_DEVICE_NAME);
	if (ret < 0) {
		pr_err("fastrpc: failed to allocate char dev region\n");
		return ret;
	}

	fastrpc_class = class_create(THIS_MODULE, "fastrpc");
	if (IS_ERR(fastrpc_class)) {
		pr_err("failed to create rpmsg class\n");
		ret = PTR_ERR(fastrpc_class);
		goto err_class;
	}

	ret = platform_driver_register(&fastrpc_cb_driver);
	if (ret < 0) {
		pr_err("fastrpc: failed to register cb driver\n");
		goto err_pdev;
	}

	ret = register_rpmsg_driver(&fastrpc_driver);
	if (ret < 0) {
		pr_err("fastrpc: failed to register rpmsg driver\n");
		goto err_rpdrv;
	}

	return 0;
err_rpdrv:
	platform_driver_unregister(&fastrpc_cb_driver);
err_pdev:
	class_destroy(fastrpc_class);
err_class:
	unregister_chrdev_region(fastrpc_major, FASTRPC_DEV_MAX);
	return ret;
}
module_init(fastrpc_init);

static void fastrpc_exit(void)
{
	platform_driver_unregister(&fastrpc_cb_driver);
	unregister_rpmsg_driver(&fastrpc_driver);
	class_destroy(fastrpc_class);
	unregister_chrdev_region(fastrpc_major, FASTRPC_DEV_MAX);
}
module_exit(fastrpc_exit);

MODULE_ALIAS("fastrpc:fastrpc");
MODULE_LICENSE("GPL v2");
