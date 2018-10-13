// SPDX-License-Identifier: GPL-2.0
/*
 * virtio_pmem.c: Virtio pmem Driver
 *
 * Discovers persistent memory range information
 * from host and provides a virtio based flushing
 * interface.
 */
#include <linux/virtio_pmem.h>
#include "nd.h"

 /* The interrupt handler */
void host_ack(struct virtqueue *vq)
{
	unsigned int len;
	unsigned long flags;
	struct virtio_pmem_request *req, *req_buf;
	struct virtio_pmem *vpmem = vq->vdev->priv;

	spin_lock_irqsave(&vpmem->pmem_lock, flags);
	while ((req = virtqueue_get_buf(vq, &len)) != NULL) {
		req->done = true;
		wake_up(&req->host_acked);

		if (!list_empty(&vpmem->req_list)) {
			req_buf = list_first_entry(&vpmem->req_list,
					struct virtio_pmem_request, list);
			list_del(&vpmem->req_list);
			req_buf->wq_buf_avail = true;
			wake_up(&req_buf->wq_buf);
		}
	}
	spin_unlock_irqrestore(&vpmem->pmem_lock, flags);
}
EXPORT_SYMBOL_GPL(host_ack);

 /* The request submission function */
int virtio_pmem_flush(struct nd_region *nd_region)
{
	int err;
	unsigned long flags;
	struct scatterlist *sgs[2], sg, ret;
	struct virtio_device *vdev = nd_region->provider_data;
	struct virtio_pmem *vpmem = vdev->priv;
	struct virtio_pmem_request *req;

	might_sleep();
	req = kmalloc(sizeof(*req), GFP_KERNEL);
	if (!req)
		return -ENOMEM;

	req->done = req->wq_buf_avail = false;
	strcpy(req->name, "FLUSH");
	init_waitqueue_head(&req->host_acked);
	init_waitqueue_head(&req->wq_buf);
	sg_init_one(&sg, req->name, strlen(req->name));
	sgs[0] = &sg;
	sg_init_one(&ret, &req->ret, sizeof(req->ret));
	sgs[1] = &ret;

	spin_lock_irqsave(&vpmem->pmem_lock, flags);
	err = virtqueue_add_sgs(vpmem->req_vq, sgs, 1, 1, req, GFP_ATOMIC);
	if (err) {
		dev_err(&vdev->dev, "failed to send command to virtio pmem device\n");

		list_add_tail(&vpmem->req_list, &req->list);
		spin_unlock_irqrestore(&vpmem->pmem_lock, flags);

		/* When host has read buffer, this completes via host_ack */
		wait_event(req->wq_buf, req->wq_buf_avail);
		spin_lock_irqsave(&vpmem->pmem_lock, flags);
	}
	virtqueue_kick(vpmem->req_vq);
	spin_unlock_irqrestore(&vpmem->pmem_lock, flags);

	/* When host has read buffer, this completes via host_ack */
	wait_event(req->host_acked, req->done);
	err = req->ret;
	kfree(req);

	return err;
};
EXPORT_SYMBOL_GPL(virtio_pmem_flush);
MODULE_LICENSE("GPL");
