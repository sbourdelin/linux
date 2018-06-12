// SPDX-License-Identifier: GPL-2.0

/*
 * Xen dma-buf functionality for gntdev.
 *
 * Copyright (c) 2018 Oleksandr Andrushchenko, EPAM Systems Inc.
 */

#include <linux/slab.h>

#include "gntdev-dmabuf.h"

struct gntdev_dmabuf_priv {
	/* List of exported DMA buffers. */
	struct list_head exp_list;
	/* List of wait objects. */
	struct list_head exp_wait_list;
	/* This is the lock which protects dma_buf_xxx lists. */
	struct mutex lock;
};

/* DMA buffer export support. */

/* Implementation of wait for exported DMA buffer to be released. */

int gntdev_dmabuf_exp_wait_released(struct gntdev_dmabuf_priv *priv, int fd,
				    int wait_to_ms)
{
	return -EINVAL;
}

int gntdev_dmabuf_exp_from_refs(struct gntdev_priv *priv, int flags,
				int count, u32 domid, u32 *refs, u32 *fd)
{
	*fd = -1;
	return -EINVAL;
}

/* DMA buffer import support. */

struct gntdev_dmabuf *
gntdev_dmabuf_imp_to_refs(struct gntdev_dmabuf_priv *priv, struct device *dev,
			  int fd, int count, int domid)
{
	return ERR_PTR(-ENOMEM);
}

u32 *gntdev_dmabuf_imp_get_refs(struct gntdev_dmabuf *gntdev_dmabuf)
{
	return NULL;
}

int gntdev_dmabuf_imp_release(struct gntdev_dmabuf_priv *priv, u32 fd)
{
	return -EINVAL;
}

struct gntdev_dmabuf_priv *gntdev_dmabuf_init(void)
{
	struct gntdev_dmabuf_priv *priv;

	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return ERR_PTR(-ENOMEM);

	return priv;
}

void gntdev_dmabuf_fini(struct gntdev_dmabuf_priv *priv)
{
	kfree(priv);
}
