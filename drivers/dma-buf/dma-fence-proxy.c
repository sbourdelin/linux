/*
 * dma-fence-proxy: placeholder unsignaled fence
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

#include <linux/dma-fence.h>
#include <linux/export.h>
#include <linux/irq_work.h>
#include <linux/slab.h>

struct dma_fence_proxy {
	struct dma_fence base;
	spinlock_t lock;

	const char *driver_name;
	void *tag;

	struct dma_fence *real;
	struct dma_fence_cb cb;
	struct irq_work work;
};

static const char *proxy_get_driver_name(struct dma_fence *fence)
{
	struct dma_fence_proxy *p = container_of(fence, typeof(*p), base);

	return p->real ? p->real->ops->get_driver_name(p->real) : p->driver_name;
}

static const char *proxy_get_timeline_name(struct dma_fence *fence)
{
	struct dma_fence_proxy *p = container_of(fence, typeof(*p), base);

	return p->real ? p->real->ops->get_timeline_name(p->real) : "unset";
}

static void proxy_irq_work(struct irq_work *work)
{
	struct dma_fence_proxy *p = container_of(work, typeof(*p), work);

	dma_fence_signal(&p->base);
	dma_fence_put(&p->base);
}

static void proxy_callback(struct dma_fence *fence, struct dma_fence_cb *cb)
{
	struct dma_fence_proxy *p = container_of(cb, typeof(*p), cb);

	/* beware the alleged spinlock inversion */
	irq_work_queue(&p->work);
}

static bool proxy_enable_signaling(struct dma_fence *fence)
{
	struct dma_fence_proxy *p = container_of(fence, typeof(*p), base);

	if (!p->real)
		return true;

	if (dma_fence_add_callback(p->real, &p->cb, proxy_callback))
		return false;

	dma_fence_get(fence);
	return true;
}

static bool proxy_signaled(struct dma_fence *fence)
{
	struct dma_fence_proxy *p = container_of(fence, typeof(*p), base);

	return p->real ? dma_fence_is_signaled(p->real) : false;
}

static void proxy_release(struct dma_fence *fence)
{
	struct dma_fence_proxy *p = container_of(fence, typeof(*p), base);

	if (!p->real)
		dma_fence_signal(&p->base);

	dma_fence_put(p->real);
	dma_fence_free(&p->base);
}

static const struct dma_fence_ops dma_fence_proxy_ops = {
	.get_driver_name = proxy_get_driver_name,
	.get_timeline_name = proxy_get_timeline_name,
	.enable_signaling = proxy_enable_signaling,
	.signaled = proxy_signaled,
	.wait = dma_fence_default_wait,
	.release = proxy_release,
};

/**
 * dma_fence_proy_create - Create an unset proxy dma-fence
 * @driver_name: The driver name to report; must outlive the fence
 * @tag: A pointer which uniquely identifies the creator
 */
struct dma_fence *dma_fence_create_proxy(const char *driver_name, void *tag)
{
	struct dma_fence_proxy *p;

	p = kzalloc(sizeof(*p), GFP_KERNEL);
	if (!p)
		return NULL;

	p->driver_name = driver_name;
	p->tag = tag;
	spin_lock_init(&p->lock);
	dma_fence_init(&p->base, &dma_fence_proxy_ops, &p->lock, 0, 0);
	init_irq_work(&p->work, proxy_irq_work);

	return &p->base;
}
EXPORT_SYMBOL(dma_fence_create_proxy);

static bool dma_fence_is_proxy(struct dma_fence *fence)
{
	return fence->ops == &dma_fence_proxy_ops;
}

/**
 * dma_fence_is_proxy_tagged - identify a proxy fence
 * @fence: The fence to identify
 * @tag: The tag pointer provided to dma_fence_create_proxy
 *
 * This returns true if this is a proxy fence tag is the same pointer as
 * the tag provided to dma_fence_create_proxy.
 */
bool dma_fence_is_proxy_tagged(struct dma_fence *fence, void *tag)
{
	struct dma_fence_proxy *p = container_of(fence, typeof(*p), base);

	if (!dma_fence_is_proxy(fence))
		return false;

	return p->tag == tag;
}
EXPORT_SYMBOL(dma_fence_is_proxy_tagged);

/**
 * dma_fence_proxy_assign - assign a fence to a proxy fence
 * @proxy: The proxy fence
 * @real: The real fence to assign to proxy
 *
 * This assigns the given real fence to the proxy fence.  From this point
 * forward, the proxy fence will be almost indistinguishable from the real
 * fence.  It will report the same driver and timeline names and will
 * signal when the real fence signals.  If the real fence is already
 * signaled when this function is called, it will signal as soon as it has
 * any listeners, possibly immediately.
 */
void dma_fence_proxy_assign(struct dma_fence *proxy, struct dma_fence *real)
{
	struct dma_fence_proxy *p = container_of(proxy, typeof(*p), base);
	unsigned long flags;

	BUG_ON(!dma_fence_is_proxy(proxy));
	BUG_ON(p->real);

	spin_lock_irqsave(p->base.lock, flags);

	p->real = dma_fence_get(real);

	if (test_bit(DMA_FENCE_FLAG_ENABLE_SIGNAL_BIT, &p->base.flags)) {
		if (dma_fence_add_callback(real, &p->cb, proxy_callback))
			dma_fence_signal_locked(&p->base);
		else
			dma_fence_get(&p->base);
	} else if (dma_fence_is_signaled(real)) {
		dma_fence_signal_locked(&p->base);
	}

	spin_unlock_irqrestore(p->base.lock, flags);
}
EXPORT_SYMBOL(dma_fence_proxy_assign);
