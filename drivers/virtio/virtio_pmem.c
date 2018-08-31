// SPDX-License-Identifier: GPL-2.0
/*
 * virtio_pmem.c: Virtio pmem Driver
 *
 * Discovers persistent memory range information
 * from host and provides a virtio based flushing
 * interface.
 */
#include <linux/virtio.h>
#include <linux/module.h>
#include <linux/virtio_ids.h>
#include <linux/virtio_config.h>
#include <uapi/linux/virtio_pmem.h>
#include <linux/spinlock.h>
#include <linux/libnvdimm.h>
#include <linux/nd.h>

struct virtio_pmem_request {
	/* Host return status corresponding to flush request */
	int ret;

	/* command name*/
	char name[16];

	/* Wait queue to process deferred work after ack from host */
	wait_queue_head_t host_acked;
	bool done;

	/* Wait queue to process deferred work after virt queue buffer avail */
	wait_queue_head_t wq_buf;
	bool wq_buf_avail;
	struct list_head list;
};

struct virtio_pmem {
	struct virtio_device *vdev;

	/* Virtio pmem request queue */
	struct virtqueue *req_vq;

	/* nvdimm bus registers virtio pmem device */
	struct nvdimm_bus *nvdimm_bus;
	struct nvdimm_bus_descriptor nd_desc;

	/* List to store deferred work if virtqueue is full */
	struct list_head req_list;

	/* Synchronize virtqueue data */
	spinlock_t pmem_lock;

	/* Memory region information */
	uint64_t start;
	uint64_t size;
};

static struct virtio_device_id id_table[] = {
	{ VIRTIO_ID_PMEM, VIRTIO_DEV_ANY_ID },
	{ 0 },
};

 /* The interrupt handler */
static void host_ack(struct virtqueue *vq)
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
 /* Initialize virt queue */
static int init_vq(struct virtio_pmem *vpmem)
{
	struct virtqueue *vq;

	/* single vq */
	vpmem->req_vq = vq = virtio_find_single_vq(vpmem->vdev,
				host_ack, "flush_queue");
	if (IS_ERR(vq))
		return PTR_ERR(vq);

	spin_lock_init(&vpmem->pmem_lock);
	INIT_LIST_HEAD(&vpmem->req_list);

	return 0;
};

 /* The request submission function */
static int virtio_pmem_flush(struct nd_region *nd_region)
{
	int err;
	unsigned long flags;
	struct scatterlist *sgs[2], sg, ret;
	struct virtio_device *vdev =
		dev_to_virtio(nd_region->dev.parent->parent);
	struct virtio_pmem *vpmem = vdev->priv;
	struct virtio_pmem_request *req = kmalloc(sizeof(*req), GFP_KERNEL);

	if (!req)
		return -ENOMEM;

	req->done = req->wq_buf_avail = false;
	strcpy(req->name, "FLUSH");
	init_waitqueue_head(&req->host_acked);
	init_waitqueue_head(&req->wq_buf);

	spin_lock_irqsave(&vpmem->pmem_lock, flags);
	sg_init_one(&sg, req->name, strlen(req->name));
	sgs[0] = &sg;
	sg_init_one(&ret, &req->ret, sizeof(req->ret));
	sgs[1] = &ret;
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

static int virtio_pmem_probe(struct virtio_device *vdev)
{
	int err = 0;
	struct resource res;
	struct virtio_pmem *vpmem;
	struct nvdimm_bus *nvdimm_bus;
	struct nd_region_desc ndr_desc;
	int nid = dev_to_node(&vdev->dev);
	struct nd_region *nd_region;

	if (!vdev->config->get) {
		dev_err(&vdev->dev, "%s failure: config disabled\n",
			__func__);
		return -EINVAL;
	}

	vdev->priv = vpmem = devm_kzalloc(&vdev->dev, sizeof(*vpmem),
			GFP_KERNEL);
	if (!vpmem) {
		err = -ENOMEM;
		goto out_err;
	}

	vpmem->vdev = vdev;
	err = init_vq(vpmem);
	if (err)
		goto out_err;

	virtio_cread(vpmem->vdev, struct virtio_pmem_config,
			start, &vpmem->start);
	virtio_cread(vpmem->vdev, struct virtio_pmem_config,
			size, &vpmem->size);

	res.start = vpmem->start;
	res.end   = vpmem->start + vpmem->size-1;
	vpmem->nd_desc.provider_name = "virtio-pmem";
	vpmem->nd_desc.module = THIS_MODULE;

	vpmem->nvdimm_bus = nvdimm_bus = nvdimm_bus_register(&vdev->dev,
						&vpmem->nd_desc);
	if (!nvdimm_bus)
		goto out_vq;

	dev_set_drvdata(&vdev->dev, nvdimm_bus);
	memset(&ndr_desc, 0, sizeof(ndr_desc));

	ndr_desc.res = &res;
	ndr_desc.numa_node = nid;
	ndr_desc.flush = virtio_pmem_flush;
	set_bit(ND_REGION_PAGEMAP, &ndr_desc.flags);
	nd_region = nvdimm_pmem_region_create(nvdimm_bus, &ndr_desc);

	if (!nd_region)
		goto out_nd;

	//virtio_device_ready(vdev);
	return 0;
out_nd:
	err = -ENXIO;
	nvdimm_bus_unregister(nvdimm_bus);
out_vq:
	vdev->config->del_vqs(vdev);
out_err:
	dev_err(&vdev->dev, "failed to register virtio pmem memory\n");
	return err;
}

static void virtio_pmem_remove(struct virtio_device *vdev)
{
	struct virtio_pmem *vpmem = vdev->priv;
	struct nvdimm_bus *nvdimm_bus = dev_get_drvdata(&vdev->dev);

	nvdimm_bus_unregister(nvdimm_bus);
	vdev->config->del_vqs(vdev);
	kfree(vpmem);
}

#ifdef CONFIG_PM_SLEEP
static int virtio_pmem_freeze(struct virtio_device *vdev)
{
	/* todo: handle freeze function */
	return -EPERM;
}

static int virtio_pmem_restore(struct virtio_device *vdev)
{
	/* todo: handle restore function */
	return -EPERM;
}
#endif


static struct virtio_driver virtio_pmem_driver = {
	.driver.name		= KBUILD_MODNAME,
	.driver.owner		= THIS_MODULE,
	.id_table		= id_table,
	.probe			= virtio_pmem_probe,
	.remove			= virtio_pmem_remove,
#ifdef CONFIG_PM_SLEEP
	.freeze                 = virtio_pmem_freeze,
	.restore                = virtio_pmem_restore,
#endif
};

module_virtio_driver(virtio_pmem_driver);
MODULE_DEVICE_TABLE(virtio, id_table);
MODULE_DESCRIPTION("Virtio pmem driver");
MODULE_LICENSE("GPL");
