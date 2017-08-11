/*
 * dma-fence-proxy: allows waiting upon unset fences
 *
 * Copyright (C) 2017 Intel Corporation
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 */

#ifndef __LINUX_DMA_FENCE_PROXY_H
#define __LINUX_DMA_FENCE_PROXY_H

#include <linux/dma-fence.h>

struct dma_fence *dma_fence_create_proxy(const char *driver_name, void *tag);
bool dma_fence_is_proxy_tagged(struct dma_fence *fence, void *tag);
void dma_fence_proxy_assign(struct dma_fence *proxy, struct dma_fence *real);

#endif /* __LINUX_DMA_FENCE_PROXY_H */
