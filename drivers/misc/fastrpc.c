// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2011-2018, The Linux Foundation. All rights reserved.
// Copyright (c) 2018, Linaro Limited

#include <linux/completion.h>
#include <linux/device.h>
#include <linux/dma-buf.h>
#include <linux/dma-mapping.h>
#include <linux/idr.h>
#include <linux/list.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/rpmsg.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <uapi/misc/fastrpc.h>

#define ADSP_DOMAIN_ID (0)
#define MDSP_DOMAIN_ID (1)
#define SDSP_DOMAIN_ID (2)
#define CDSP_DOMAIN_ID (3)
#define FASTRPC_DEV_MAX		4 /* adsp, mdsp, slpi, cdsp*/
#define FASTRPC_MAX_SESSIONS	9 /*8 compute, 1 cpz*/
#define FASTRPC_ALIGN		128
#define FASTRPC_MAX_FDLIST	16
#define FASTRPC_MAX_CRCLIST	64
#define FASTRPC_PHYS(p)	((p) & 0xffffffff)
#define FASTRPC_CTX_MAX (256)
#define FASTRPC_INIT_HANDLE	1
#define FASTRPC_CTXID_MASK (0xFF0)
#define INIT_FILELEN_MAX (2 * 1024 * 1024)
#define INIT_MEMLEN_MAX  (8 * 1024 * 1024)
#define FASTRPC_DEVICE_NAME	"fastrpc"

/* Retrives number of input buffers from the scalars parameter */
#define REMOTE_SCALARS_INBUFS(sc)	(((sc) >> 16) & 0x0ff)

/* Retrives number of output buffers from the scalars parameter */
#define REMOTE_SCALARS_OUTBUFS(sc)	(((sc) >> 8) & 0x0ff)

/* Retrives number of input handles from the scalars parameter */
#define REMOTE_SCALARS_INHANDLES(sc)	(((sc) >> 4) & 0x0f)

/* Retrives number of output handles from the scalars parameter */
#define REMOTE_SCALARS_OUTHANDLES(sc)	((sc) & 0x0f)

#define REMOTE_SCALARS_LENGTH(sc)	(REMOTE_SCALARS_INBUFS(sc) +   \
					 REMOTE_SCALARS_OUTBUFS(sc) +  \
					 REMOTE_SCALARS_INHANDLES(sc)+ \
					 REMOTE_SCALARS_OUTHANDLES(sc))

#define FASTRPC_BUILD_SCALARS(attr, method, in, out, oin, oout) \
		((((uint32_t)  (attr) & 0x07) << 29) | \
		(((uint32_t) (method) & 0x1f) << 24) | \
		(((uint32_t)     (in) & 0xff) << 16) | \
		(((uint32_t)    (out) & 0xff) <<  8) | \
		(((uint32_t)    (oin) & 0x0f) <<  4) | \
		((uint32_t)    (oout) & 0x0f))

#define FASTRPC_SCALARS(method, in, out) \
		FASTRPC_BUILD_SCALARS(0, method, in, out, 0, 0)

/* Remote Method id table */
#define FASTRPC_RMID_INIT_ATTACH	0
#define FASTRPC_RMID_INIT_RELEASE	1
#define FASTRPC_RMID_INIT_CREATE	6
#define FASTRPC_RMID_INIT_CREATE_ATTR	7
#define FASTRPC_RMID_INIT_CREATE_STATIC	8

#define miscdev_to_cctx(d) container_of(d, struct fastrpc_channel_ctx, miscdev)

static const char *domains[FASTRPC_DEV_MAX] = { "adsp", "mdsp",
						"sdsp", "cdsp"};
struct fastrpc_phy_page {
	uint64_t addr;		/* physical address */
	uint64_t size;		/* size of contiguous region */
};

struct fastrpc_invoke_buf {
	int num;		/* number of contiguous regions */
	int pgidx;		/* index to start of contiguous region */
};

struct fastrpc_remote_arg {
	uint64_t pv;
	uint64_t len;
};

struct fastrpc_msg {
	uint32_t pid;		/* process group id */
	uint32_t tid;		/* thread id */
	uint64_t ctx;		/* invoke caller context */
	uint32_t handle;	/* handle to invoke */
	uint32_t sc;		/* scalars structure describing the data */
	uint64_t addr;		/* physical address */
	uint64_t size;		/* size of contiguous region */
};

struct fastrpc_invoke_rsp {
	uint64_t ctx;		/* invoke caller context */
	int retval;		/* invoke return value */
};

struct fastrpc_buf {
	struct fastrpc_user *fl;
	struct dma_buf *dmabuf;
	struct device *dev;
	void *virt;
	uint64_t phys;
	size_t size;
	/* Lock for dma buf attachments */
	struct mutex lock;
	struct list_head attachments;
};

struct fastrpc_dma_buf_attachment {
	struct device *dev;
	struct sg_table sgt;
	struct list_head node;
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
	void *va;
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

	int nscalars;
	int nbufs;
	uint32_t *crc;

	struct fastrpc_remote_arg *rpra;
	struct fastrpc_map **maps;
	struct fastrpc_buf *buf;
	struct fastrpc_invoke_args *args;
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
	struct miscdevice miscdev;
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

static int fastrpc_map_get(struct fastrpc_user *fl, int fd, size_t len,
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

	INIT_LIST_HEAD(&buf->attachments);
	mutex_init(&buf->lock);

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

	kfree(ctx->maps);
	kfree(ctx);
}

static struct fastrpc_invoke_ctx *
fastrpc_context_alloc(struct fastrpc_user *user, uint32_t kernel,
		      struct fastrpc_invoke *inv)
{
	struct fastrpc_channel_ctx *cctx = user->cctx;
	struct fastrpc_invoke_ctx *ctx = NULL;
	int bufs, ret;
	int err = 0;

	bufs = REMOTE_SCALARS_LENGTH(inv->sc);
	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return ERR_PTR(-ENOMEM);

	INIT_LIST_HEAD(&ctx->node);
	ctx->fl = user;
	ctx->nscalars = bufs;
	ctx->nbufs = REMOTE_SCALARS_INBUFS(inv->sc) +
		     REMOTE_SCALARS_OUTBUFS(inv->sc);

	if (ctx->nscalars) {
		ctx->maps = kcalloc(ctx->nscalars,
				    sizeof(*ctx->maps), GFP_KERNEL);
		if (!ctx->maps) {
			kfree(ctx);
			return ERR_PTR(-ENOMEM);
		}
		ctx->args = inv->args;
	}

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
	kfree(ctx->maps);
	kfree(ctx);

	return ERR_PTR(err);
}

static struct sg_table *
fastrpc_map_dma_buf(struct dma_buf_attachment *attachment,
		    enum dma_data_direction dir)
{
	struct fastrpc_dma_buf_attachment *a = attachment->priv;
	struct sg_table *table;

	table = &a->sgt;

	if (!dma_map_sg(attachment->dev, table->sgl, table->nents, dir))
		return ERR_PTR(-ENOMEM);

	return table;
}

static void fastrpc_unmap_dma_buf(struct dma_buf_attachment *attach,
				  struct sg_table *table,
				  enum dma_data_direction dir)
{
}

static void fastrpc_release(struct dma_buf *dmabuf)
{
	struct fastrpc_buf *buffer = dmabuf->priv;

	fastrpc_buf_free(buffer);
}

static int fastrpc_dma_buf_attach(struct dma_buf *dmabuf,
				  struct dma_buf_attachment *attachment)
{
	struct fastrpc_dma_buf_attachment *a;
	struct fastrpc_buf *buffer = dmabuf->priv;
	int ret;

	a = kzalloc(sizeof(*a), GFP_KERNEL);
	if (!a)
		return -ENOMEM;

	ret = dma_get_sgtable(buffer->dev, &a->sgt, buffer->virt,
			      FASTRPC_PHYS(buffer->phys), buffer->size);
	if (ret < 0) {
		dev_err(buffer->dev, "failed to get scatterlist from DMA API\n");
		return -EINVAL;
	}

	a->dev = attachment->dev;
	INIT_LIST_HEAD(&a->node);
	attachment->priv = a;

	mutex_lock(&buffer->lock);
	list_add(&a->node, &buffer->attachments);
	mutex_unlock(&buffer->lock);

	return 0;
}

static void fastrpc_dma_buf_detatch(struct dma_buf *dmabuf,
				    struct dma_buf_attachment *attachment)
{
	struct fastrpc_dma_buf_attachment *a = attachment->priv;
	struct fastrpc_buf *buffer = dmabuf->priv;

	mutex_lock(&buffer->lock);
	list_del(&a->node);
	mutex_unlock(&buffer->lock);
	kfree(a);
}

static void *fastrpc_kmap(struct dma_buf *dmabuf, unsigned long pgnum)
{
	struct fastrpc_buf *buf = dmabuf->priv;

	return buf->virt ? buf->virt + pgnum * PAGE_SIZE : NULL;
}

static void *fastrpc_vmap(struct dma_buf *dmabuf)
{
	struct fastrpc_buf *buf = dmabuf->priv;

	return buf->virt;
}

static int fastrpc_mmap(struct dma_buf *dmabuf,
			struct vm_area_struct *vma)
{
	struct fastrpc_buf *buf = dmabuf->priv;
	size_t size = vma->vm_end - vma->vm_start;

	return dma_mmap_coherent(buf->dev, vma, buf->virt,
				 FASTRPC_PHYS(buf->phys), size);
}

static const struct dma_buf_ops fastrpc_dma_buf_ops = {
	.attach = fastrpc_dma_buf_attach,
	.detach = fastrpc_dma_buf_detatch,
	.map_dma_buf = fastrpc_map_dma_buf,
	.unmap_dma_buf = fastrpc_unmap_dma_buf,
	.mmap = fastrpc_mmap,
	.map = fastrpc_kmap,
	.vmap = fastrpc_vmap,
	.release = fastrpc_release,
};

static int fastrpc_map_create(struct fastrpc_user *fl, int fd,
			      size_t len, struct fastrpc_map **ppmap)
{
	struct fastrpc_session_ctx *sess = fl->sctx;
	struct fastrpc_map *map = NULL;
	int err = 0;

	if (!fastrpc_map_get(fl, fd, len, ppmap))
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

	map->table = dma_buf_map_attachment(map->attach, DMA_BIDIRECTIONAL);
	if (IS_ERR(map->table)) {
		err = PTR_ERR(map->table);
		goto map_err;
	}

	map->phys = sg_dma_address(map->table->sgl);
	map->phys += ((uint64_t)fl->sctx->sid << 32);
	map->size = len;
	map->va = sg_virt(map->table->sgl);
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

/*
 * Fastrpc payload buffer with metadata looks like:
 *
 * >>>>>>  START of METADATA <<<<<<<<<
 * +---------------------------------+
 * |           Arguments             |
 * | type:(struct fastrpc_remote_arg)|
 * |             (0 - N)             |
 * +---------------------------------+
 * |         Invoke Buffer list      |
 * | type:(struct fastrpc_invoke_buf)|
 * |           (0 - N)               |
 * +---------------------------------+
 * |         Page info list          |
 * | type:(struct fastrpc_phy_page)  |
 * |             (0 - N)             |
 * +---------------------------------+
 * |         Optional info           |
 * |(can be specific to SoC/Firmware)|
 * +---------------------------------+
 * >>>>>>>>  END of METADATA <<<<<<<<<
 * +---------------------------------+
 * |         Inline ARGS             |
 * |            (0-N)                |
 * +---------------------------------+
 */

static int fastrpc_get_meta_size(struct fastrpc_invoke_ctx *ctx)
{
	int size = 0;

	size = (sizeof(struct fastrpc_remote_arg) +
		sizeof(struct fastrpc_invoke_buf) +
		sizeof(struct fastrpc_phy_page)) * ctx->nscalars +
		sizeof(uint64_t) * FASTRPC_MAX_FDLIST +
		sizeof(uint32_t) * FASTRPC_MAX_CRCLIST;

	return size;
}

static int fastrpc_get_payload_size(struct fastrpc_invoke_ctx *ctx, int metalen)
{
	int i, size = 0;

	size = ALIGN(metalen, FASTRPC_ALIGN);
	for (i = 0; i < ctx->nscalars; i++) {
		if (ctx->args[i].fd == 0 || ctx->args[i].fd == -1) {
			size = ALIGN(size, FASTRPC_ALIGN);
			size += ctx->args[i].length;
		}
	}

	return size;
}

static int fastrpc_create_maps(struct fastrpc_invoke_ctx *ctx)
{
	struct device *dev = ctx->fl->sctx->dev;
	int i, err;

	for (i = 0; i < ctx->nscalars; ++i) {
		if (ctx->args[i].fd == 0 || ctx->args[i].fd == -1 ||
		    ctx->args[i].length == 0)
			continue;

		err = fastrpc_map_create(ctx->fl, ctx->args[i].fd,
					 ctx->args[i].length, &ctx->maps[i]);
		if (err) {
			dev_err(dev, "Error Creating map %d\n", err);
			return -EINVAL;
		}

	}
	return 0;
}

static int fastrpc_get_args(uint32_t kernel, struct fastrpc_invoke_ctx *ctx)
{
	struct device *dev = ctx->fl->sctx->dev;
	struct fastrpc_remote_arg *rpra;
	struct fastrpc_invoke_buf *list;
	struct fastrpc_phy_page *pages;
	uintptr_t args;
	size_t rlen = 0, pkt_size = 0, metalen = 0;
	int inbufs, i, err = 0;

	inbufs = REMOTE_SCALARS_INBUFS(ctx->sc);
	metalen = fastrpc_get_meta_size(ctx);
	pkt_size = fastrpc_get_payload_size(ctx, metalen);
	fastrpc_create_maps(ctx);
	ctx->used_sz = pkt_size;

	err = fastrpc_buf_alloc(ctx->fl, dev, pkt_size, &ctx->buf);
	if (err)
		goto bail;

	rpra = ctx->buf->virt;
	list = ctx->buf->virt + ctx->nscalars * sizeof(*rpra);
	pages = ctx->buf->virt + ctx->nscalars * (sizeof(*list) +
		sizeof(*rpra));
	args = (uintptr_t)ctx->buf->virt + metalen;
	rlen = pkt_size - metalen;
	ctx->rpra = rpra;

	for (i = 0; i < ctx->nbufs; ++i) {
		size_t len = ctx->args[i].length;

		rpra[i].pv = 0;
		rpra[i].len = len;
		list[i].num = len ? 1 : 0;
		list[i].pgidx = i;

		if (!len)
			continue;

		pages[i].size = roundup(len, PAGE_SIZE);

		if (ctx->maps[i]) {
			rpra[i].pv = (uint64_t) ctx->args[i].ptr;
			pages[i].addr = ctx->maps[i]->phys;
		} else {
			rlen -= ALIGN(args, FASTRPC_ALIGN) - args;
			args = ALIGN(args, FASTRPC_ALIGN);
			if (rlen < len)
				goto bail;

			rpra[i].pv = (args);
			pages[i].addr = ctx->buf->phys + (pkt_size - rlen);
			pages[i].addr = pages[i].addr &	PAGE_MASK;
			args = args + len;
			rlen -= len;
		}

		if (i < inbufs && !ctx->maps[i]) {
			if (!kernel) {
				err = copy_from_user((void *)rpra[i].pv,
					     (void __user *)ctx->args[i].ptr,
					     ctx->args[i].length);
				if (err)
					goto bail;
			} else {
				memcpy((void *)rpra[i].pv, ctx->args[i].ptr,
				       ctx->args[i].length);
			}
		}
	}

	for (i = ctx->nbufs; i < ctx->nscalars; ++i) {
		rpra[i].pv = (uint64_t) ctx->args[i].ptr;
		rpra[i].len = ctx->args[i].length;
		list[i].num = ctx->args[i].length ? 1 : 0;
		list[i].pgidx = i;
		pages[i].addr = ctx->maps[i]->phys;
		pages[i].size = ctx->maps[i]->size;
	}

bail:
	if (err)
		dev_err(dev, "Error: get invoke args failed:%d\n", err);

	return err;
}

static int fastrpc_put_args(struct fastrpc_invoke_ctx *ctx,
			    uint32_t kernel)
{
	struct fastrpc_remote_arg *rpra = ctx->rpra;
	struct device *dev = ctx->fl->sctx->dev;
	int i, inbufs, err;

	inbufs = REMOTE_SCALARS_INBUFS(ctx->sc);

	for (i = inbufs; i < ctx->nbufs; ++i) {
		if (ctx->maps[i]) {
			fastrpc_map_put(ctx->maps[i]);
			ctx->maps[i] = NULL;
			continue;
		}

		if (!kernel) {
			err = copy_to_user((void __user *)ctx->args[i].ptr,
					  (void  *)rpra[i].pv, rpra[i].len);
			if (err) {
				dev_err(dev, "Error: copy buffer %d\n", err);
				return err;
			}
		} else {
			memcpy(ctx->args[i].ptr, (void *)rpra[i].pv,
			       rpra[i].len);
		}
	}

	return 0;
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

	msg->ctx = ctx->ctxid | fl->pd;
	msg->handle = handle;
	msg->sc = ctx->sc;
	msg->addr = ctx->buf ? ctx->buf->phys : 0;
	msg->size = roundup(ctx->used_sz, PAGE_SIZE);

	return rpmsg_send(cctx->rpdev->ept, (void *)msg, sizeof(*msg));
}

static int fastrpc_internal_invoke(struct fastrpc_user *fl,
				   uint32_t kernel,
				   struct fastrpc_invoke *inv)
{
	struct fastrpc_invoke_ctx *ctx = NULL;
	int err = 0;

	if (!fl->sctx)
		return -EINVAL;

	ctx = fastrpc_context_alloc(fl, kernel, inv);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);

	if (ctx->nscalars) {
		err = fastrpc_get_args(kernel, ctx);
		if (err)
			goto bail;
	}
	/* Send invoke buffer to remote dsp */
	err = fastrpc_invoke_send(fl->sctx, ctx, kernel, inv->handle);
	if (err)
		goto bail;

	/* Wait for remote dsp to respond or time out */
	err = wait_for_completion_interruptible(&ctx->work);
	if (err)
		goto bail;

	/* Check the response from remote dsp */
	err = ctx->retval;
	if (err)
		goto bail;

	/* populate all the output buffers with results */
	err = fastrpc_put_args(ctx, kernel);
	if (err)
		goto bail;

	/* We are done with this compute, release it now! */
bail:
	if (ctx)
		fastrpc_context_free(ctx);

	if (err)
		dev_err(fl->sctx->dev, "Error: Invoke Failed %d\n", err);

	return err;
}

static long fastrpc_init_create_process(struct fastrpc_user *fl,
					char __user *argp)
{
	struct fastrpc_init_create init;
	struct fastrpc_invoke_args args[6];
	struct fastrpc_phy_page pages[1];
	struct fastrpc_invoke inv;
	struct fastrpc_map *map = NULL;
	struct fastrpc_buf *imem = NULL;
	int err, memlen;
	struct {
		int pgid;
		unsigned int namelen;
		unsigned int filelen;
		unsigned int pageslen;
		int attrs;
		int siglen;
	} inbuf;

	err = copy_from_user(&init, argp, sizeof(init));
	if (err)
		goto bail;

	if (init.filelen > INIT_FILELEN_MAX ||
	    init.memlen > INIT_MEMLEN_MAX)
		goto bail;

	inbuf.pgid = fl->tgid;
	inbuf.namelen = strlen(current->comm) + 1;
	inbuf.filelen = init.filelen;
	inbuf.pageslen = 1;
	inbuf.attrs = init.attrs;
	inbuf.siglen = init.siglen;
	fl->pd = 1;

	if (init.filelen && init.filefd) {
		err = fastrpc_map_create(fl, init.filefd, init.filelen, &map);
		if (err)
			goto bail;
	}
	memlen = ALIGN(max(INIT_FILELEN_MAX, (int)init.filelen * 4),
		       1024 * 1024);
	err = fastrpc_buf_alloc(fl, fl->sctx->dev, memlen,
				&imem);
	if (err)
		goto bail;

	fl->init_mem = imem;
	args[0].ptr = &inbuf;
	args[0].length = sizeof(inbuf);
	args[0].fd = -1;

	args[1].ptr = current->comm;
	args[1].length = inbuf.namelen;
	args[1].fd = -1;

	args[2].ptr = (void *)init.file;
	args[2].length = inbuf.filelen;
	args[2].fd = init.filefd;

	pages[0].addr = imem->phys;
	pages[0].size = imem->size;

	args[3].ptr = pages;
	args[3].length = 1 * sizeof(*pages);
	args[3].fd = -1;

	args[4].ptr = &inbuf.attrs;
	args[4].length = sizeof(inbuf.attrs);
	args[4].fd = -1;

	args[5].ptr = &inbuf.siglen;
	args[5].length = sizeof(inbuf.siglen);
	args[5].fd = -1;

	inv.handle = 1;
	inv.sc = FASTRPC_SCALARS(FASTRPC_RMID_INIT_CREATE, 4, 0);
	if (init.attrs)
		inv.sc = FASTRPC_SCALARS(FASTRPC_RMID_INIT_CREATE_ATTR, 6, 0);
	inv.args = &args[0];
	err = fastrpc_internal_invoke(fl, 1, &inv);
bail:
	if (map)
		fastrpc_map_put(map);

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

static int fastrpc_release_current_dsp_process(struct fastrpc_user *fl)
{
	struct fastrpc_invoke inv;
	struct fastrpc_invoke_args args[1];
	int tgid = 0;

	tgid = fl->tgid;
	args[0].ptr = &tgid;
	args[0].length = sizeof(tgid);
	args[0].fd = -1;
	inv.handle = 1;
	inv.sc = FASTRPC_SCALARS(FASTRPC_RMID_INIT_RELEASE, 1, 0);
	inv.args = &args[0];

	return fastrpc_internal_invoke(fl, 1, &inv);
}

static int fastrpc_device_release(struct inode *inode, struct file *file)
{
	struct fastrpc_user *fl = (struct fastrpc_user *)file->private_data;
	struct fastrpc_channel_ctx *cctx = fl->cctx;
	struct fastrpc_invoke_ctx *ctx, *n;
	struct fastrpc_map *map, *m;

	fastrpc_release_current_dsp_process(fl);

	spin_lock(&cctx->lock);
	list_del(&fl->user);
	spin_unlock(&cctx->lock);

	if (fl->init_mem)
		fastrpc_buf_free(fl->init_mem);

	list_for_each_entry_safe(ctx, n, &fl->pending, node)
		fastrpc_context_free(ctx);

	list_for_each_entry_safe(map, m, &fl->maps, node)
		fastrpc_map_put(map);

	fastrpc_session_free(fl->cctx, fl->sctx);

	mutex_destroy(&fl->mutex);
	kfree(fl);
	file->private_data = NULL;

	return 0;
}

static int fastrpc_device_open(struct inode *inode, struct file *filp)
{
	struct fastrpc_channel_ctx *cctx = miscdev_to_cctx(filp->private_data);
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
	spin_lock(&cctx->lock);
	list_add_tail(&fl->user, &cctx->users);
	spin_unlock(&cctx->lock);
	fl->sctx = fastrpc_session_alloc(cctx, 0);

	return 0;
}

static long fastrpc_dmabuf_free(struct fastrpc_user *fl, char __user *argp)
{
	struct dma_buf *buf;
	uint32_t info;

	if (copy_from_user(&info, argp, sizeof(info)))
		return -EFAULT;

	buf = dma_buf_get(info);
	if (IS_ERR_OR_NULL(buf))
		return -EINVAL;
	/*
	 * one for the last get and other for the ALLOC_DMA_BUFF ioctl
	 */
	dma_buf_put(buf);
	dma_buf_put(buf);

	return 0;
}
static long fastrpc_dmabuf_alloc(struct fastrpc_user *fl, char __user *argp)
{
	struct fastrpc_alloc_dma_buf bp;
	DEFINE_DMA_BUF_EXPORT_INFO(exp_info);
	struct fastrpc_buf *buf = NULL;
	int err;

	err = copy_from_user(&bp, argp, sizeof(bp));
	if (err)
		return err;

	err = fastrpc_buf_alloc(fl, fl->sctx->dev, bp.size, &buf);
	if (err)
		return err;
	exp_info.ops = &fastrpc_dma_buf_ops;
	exp_info.size = bp.size;
	exp_info.flags = O_RDWR;
	exp_info.priv = buf;
	buf->dmabuf = dma_buf_export(&exp_info);
	if (IS_ERR(buf->dmabuf)) {
		err = PTR_ERR(buf->dmabuf);
		return err;
	}

	get_dma_buf(buf->dmabuf);
	bp.fd = dma_buf_fd(buf->dmabuf, O_ACCMODE);
	if (bp.fd < 0) {
		dma_buf_put(buf->dmabuf);
		return -EINVAL;
	}

	return copy_to_user(argp, &bp, sizeof(bp));
}

static long fastrpc_init_attach(struct fastrpc_user *fl)
{
	struct fastrpc_invoke_args args[1];
	struct fastrpc_invoke inv;
	int tgid = fl->tgid;

	args[0].ptr = &tgid;
	args[0].length = sizeof(tgid);
	args[0].fd = -1;
	inv.handle = FASTRPC_INIT_HANDLE;
	inv.sc = FASTRPC_SCALARS(FASTRPC_RMID_INIT_ATTACH, 1, 0);
	inv.args = &args[0];
	fl->pd = 0;

	return fastrpc_internal_invoke(fl, 1, &inv);
}

static long fastrpc_invoke(struct fastrpc_user *fl, char __user *argp)
{
	struct fastrpc_invoke_args *args = NULL;
	struct fastrpc_invoke inv;
	int nscalars, err;

	if (copy_from_user(&inv, argp, sizeof(inv)))
		return -EFAULT;

	nscalars = REMOTE_SCALARS_LENGTH(inv.sc);
	if (nscalars) {
		args = kcalloc(nscalars, sizeof(*args), GFP_KERNEL);
		if (!args)
			return -ENOMEM;

		if (copy_from_user(args, (void __user *)inv.args,
				   nscalars * sizeof(*args))) {
			kfree(args);
			return -EFAULT;
		}
	}

	inv.args = args;
	err = fastrpc_internal_invoke(fl, 0, &inv);
	kfree(args);

	return err;
}

static long fastrpc_device_ioctl(struct file *file, unsigned int cmd,
				 unsigned long arg)
{
	struct fastrpc_user *fl = (struct fastrpc_user *)file->private_data;
	char __user *argp = (char __user *)arg;
	int err;

	switch (cmd) {
	case FASTRPC_IOCTL_INVOKE:
		err = fastrpc_invoke(fl, argp);
		break;
	case FASTRPC_IOCTL_INIT_ATTACH:
		err = fastrpc_init_attach(fl);
		break;
	case FASTRPC_IOCTL_INIT_CREATE:
		err = fastrpc_init_create_process(fl, argp);
		break;
	case FASTRPC_IOCTL_FREE_DMA_BUFF:
		err = fastrpc_dmabuf_free(fl, argp);
		break;
	case FASTRPC_IOCTL_ALLOC_DMA_BUFF:
		err = fastrpc_dmabuf_alloc(fl, argp);
		break;
	default:
		err = -ENOTTY;
		dev_err(fl->sctx->dev, "bad ioctl: %d\n", cmd);
		break;
	}

	if (err)
		dev_err(fl->sctx->dev, "Error: IOCTL Failed with %d\n", err);

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

static const struct of_device_id fastrpc_match_table[] = {
	{ .compatible = "qcom,fastrpc-compute-cb", },
	{}
};

static struct platform_driver fastrpc_cb_driver = {
	.probe = fastrpc_cb_probe,
	.remove = fastrpc_cb_remove,
	.driver = {
		.name = "qcom,fastrpc-cb",
		.of_match_table = fastrpc_match_table,
		.suppress_bind_attrs = true,
	},
};

static int fastrpc_rpmsg_probe(struct rpmsg_device *rpdev)
{
	struct device *rdev = &rpdev->dev;
	struct fastrpc_channel_ctx *data;
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

	data->miscdev.minor = MISC_DYNAMIC_MINOR;
	data->miscdev.name = kasprintf(GFP_KERNEL, "fastrpc-%s",
				domains[domain_id]);
	data->miscdev.fops = &fastrpc_fops;
	err = misc_register(&data->miscdev);
	if (err)
		return err;

	dev_set_drvdata(&rpdev->dev, data);
	dma_set_mask_and_coherent(rdev, DMA_BIT_MASK(32));
	INIT_LIST_HEAD(&data->users);
	spin_lock_init(&data->lock);
	idr_init(&data->ctx_idr);
	data->domain_id = domain_id;
	data->rpdev = rpdev;

	return of_platform_populate(rdev->of_node, NULL, NULL, rdev);
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

	misc_deregister(&cctx->miscdev);
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
		.name = "qcom,fastrpc",
		.of_match_table = fastrpc_rpmsg_of_match,
	},
};

static int fastrpc_init(void)
{
	int ret;

	ret = platform_driver_register(&fastrpc_cb_driver);
	if (ret < 0) {
		pr_err("fastrpc: failed to register cb driver\n");
		return ret;
	}

	ret = register_rpmsg_driver(&fastrpc_driver);
	if (ret < 0) {
		pr_err("fastrpc: failed to register rpmsg driver\n");
		platform_driver_unregister(&fastrpc_cb_driver);
		return ret;
	}

	return 0;
}
module_init(fastrpc_init);

static void fastrpc_exit(void)
{
	platform_driver_unregister(&fastrpc_cb_driver);
	unregister_rpmsg_driver(&fastrpc_driver);
}
module_exit(fastrpc_exit);

MODULE_ALIAS("fastrpc:fastrpc");
MODULE_LICENSE("GPL v2");
