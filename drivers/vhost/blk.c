// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2018 IBM Corporation
 * Author: Vitaly Mayatskikh <v.mayatskih@gmail.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2.
 *
 * virtio-blk server in host kernel.
 */

#include <linux/module.h>
#include <linux/miscdevice.h>
#include <linux/virtio_blk.h>
#include <linux/vhost.h>
#include <linux/fs.h>
#include "vhost.h"

enum {
	VHOST_BLK_FEATURES =
	VHOST_FEATURES |
	(1ULL << VIRTIO_F_IOMMU_PLATFORM) |
	(1ULL << VIRTIO_RING_F_INDIRECT_DESC) |
	(1ULL << VIRTIO_RING_F_EVENT_IDX) |
	(1ULL << VIRTIO_BLK_F_MQ)
};

#define VHOST_BLK_SET_BACKEND _IOW(VHOST_VIRTIO, 0x50, int)

enum {
	VHOST_BLK_VQ_MAX = 16,
	VHOST_BLK_VQ_MAX_REQS = 128,
};

struct vhost_blk_req {
	struct llist_node list;
	int index;
	struct vhost_blk_queue *q;
	struct virtio_blk_outhdr hdr;
	struct iovec *out_iov;
	struct iovec *in_iov;
	u8 out_num;
	u8 in_num;
	long len;
	struct kiocb iocb;
	struct iov_iter i;
	int res;
	void __user *status;
};

struct vhost_blk_queue {
	int index;
	struct vhost_blk *blk;
	struct vhost_virtqueue vq;
	struct vhost_work w;
	struct llist_head wl;
	struct vhost_blk_req req[VHOST_BLK_VQ_MAX_REQS];
};

struct vhost_blk {
	struct vhost_dev dev;
	struct file *backend;
	int num_queues;
	struct vhost_virtqueue *vqs[VHOST_BLK_VQ_MAX];
	struct vhost_blk_queue queue[VHOST_BLK_VQ_MAX];
};

static void vhost_blk_flush(struct vhost_blk *blk)
{
	int i;

	for (i = 0; i < blk->num_queues; i++)
		vhost_poll_flush(&blk->queue[i].vq.poll);
}


static void vhost_blk_stop(struct vhost_blk *blk)
{
	struct vhost_virtqueue *vq;
	int i;

	for (i = 0; i < blk->num_queues; i++) {
		vq = &blk->queue[i].vq;
		mutex_lock(&vq->mutex);
		rcu_assign_pointer(vq->private_data, NULL);
		mutex_unlock(&vq->mutex);
	}
}

static int vhost_blk_req_done(struct vhost_blk_req *req, unsigned char status)
{
	int ret;
	int len = req->len;

	pr_debug("%s vq[%d] req->index %d status %d len %d\n", __func__,
		 req->q->index, req->index, status, len);
	ret = put_user(status, (unsigned char __user *)req->status);

	WARN(ret, "%s: vq[%d] req->index %d failed to write status\n", __func__,
	     req->q->index, req->index);

	vhost_add_used(&req->q->vq, req->index, len);

	return ret;
}

static void vhost_blk_io_done_work(struct vhost_work *w)
{
	struct vhost_blk_queue *q = container_of(w, struct vhost_blk_queue, w);
	struct llist_node *node;
	struct vhost_blk_req *req, *tmp;

	node = llist_del_all(&q->wl);
	llist_for_each_entry_safe(req, tmp, node, list) {
		vhost_blk_req_done(req, req->res);
	}
	vhost_signal(&q->blk->dev, &q->vq);
}

static void vhost_blk_iocb_complete(struct kiocb *iocb, long ret, long ret2)
{
	struct vhost_blk_req *req = container_of(iocb, struct vhost_blk_req,
						 iocb);

	pr_debug("%s vq[%d] req->index %d ret %ld ret2 %ld\n", __func__,
		 req->q->index, req->index, ret, ret2);

	req->res = (ret == req->len) ? VIRTIO_BLK_S_OK : VIRTIO_BLK_S_IOERR;
	llist_add(&req->list, &req->q->wl);
	vhost_vq_work_queue(&req->q->vq, &req->q->w);
}

static int vhost_blk_req_handle(struct vhost_blk_req *req)
{
	struct vhost_blk *blk = req->q->blk;
	struct vhost_virtqueue *vq = &req->q->vq;
	int type = le32_to_cpu(req->hdr.type);
	int ret;
	u8 status;

	if ((type == VIRTIO_BLK_T_IN) || (type == VIRTIO_BLK_T_OUT)) {
		bool write = (type == VIRTIO_BLK_T_OUT);
		int nr_seg = (write ? req->out_num : req->in_num) - 1;
		unsigned long sector = le64_to_cpu(req->hdr.sector);
		ssize_t len, rem_len;

		if (!req->q->blk->backend) {
			vq_err(vq, "blk %p no backend!\n", req->q->blk);
			ret = -EINVAL;
			goto out_err;
		}

		len = iov_length(&vq->iov[1], nr_seg);
		pr_debug("%s: [pid:%d %s] %s sector %lld, len %ld\n",
			 __func__, current->pid, current->comm,
			 write ? "WRITE" : "READ", req->hdr.sector, len);

		req->len = len;
		rem_len = len;
		iov_iter_init(&req->i, (write ? WRITE : READ),
			      write ? &req->out_iov[0] : &req->in_iov[0],
			      nr_seg, len);

		req->iocb.ki_pos = sector << 9;
		req->iocb.ki_filp = blk->backend;
		req->iocb.ki_complete = vhost_blk_iocb_complete;
		req->iocb.ki_flags = IOCB_DIRECT;

		if (write)
			ret = call_write_iter(blk->backend, &req->iocb,
					      &req->i);
		else
			ret = call_read_iter(blk->backend, &req->iocb,
					     &req->i);

		if (ret != -EIOCBQUEUED)
			vhost_blk_iocb_complete(&req->iocb, ret, 0);

		ret = 0;
		goto out;
	}

	if (type == VIRTIO_BLK_T_GET_ID) {
		char s[] = "vhost_blk";
		size_t len = min_t(size_t, req->in_iov[0].iov_len,
				   strlen(s));

		ret = copy_to_user(req->in_iov[0].iov_base, s, len);
		status = ret ? VIRTIO_BLK_S_IOERR : VIRTIO_BLK_S_OK;
		if (put_user(status, (unsigned char __user *)req->status)) {
			ret = -EFAULT;
			goto out_err;
		}
		vhost_add_used_and_signal(&blk->dev, vq, req->index, 1);
		ret = 0;
		goto out;
	} else {
		pr_warn("Unsupported request type %d\n", type);
		vhost_discard_vq_desc(vq, 1);
		ret = -EINVAL;
		return ret;
	}
out_err:
	vhost_discard_vq_desc(vq, 1);
out:
	return ret;
}

static void vhost_blk_handle_guest_kick(struct vhost_work *work)
{
	struct vhost_virtqueue *vq;
	struct vhost_blk_queue *q;
	struct vhost_blk *blk;
	struct vhost_blk_req *req;
	int in, out;
	int head;

	vq = container_of(work, struct vhost_virtqueue, poll.work);
	q = container_of(vq, struct vhost_blk_queue, vq);
	blk = container_of(vq->dev, struct vhost_blk, dev);

	vhost_disable_notify(&blk->dev, vq);
	for (;;) {
		in = out = -1;

		head = vhost_get_vq_desc(vq, vq->iov,
					 ARRAY_SIZE(vq->iov),
					 &out, &in, NULL, NULL);

		if (head < 0)
			break;

		if (head == vq->num) {
			if (vhost_enable_notify(&blk->dev, vq)) {
				vhost_disable_notify(&blk->dev, vq);
				continue;
			}
			break;
		}

		req = &q->req[head];
		req->index = head;
		req->out_num = out;
		req->in_num = in;
		req->out_iov = &vq->iov[1];
		req->in_iov = &vq->iov[out];
		req->status = vq->iov[out + in - 1].iov_base;

		if (copy_from_user(&req->hdr, vq->iov[0].iov_base,
				   sizeof(req->hdr))) {
			vq_err(vq, "Failed to get block header!\n");
			vhost_discard_vq_desc(vq, 1);
			continue;
		}
		if (vhost_blk_req_handle(req) < 0)
			break;
	}
}

static int vhost_blk_open(struct inode *inode, struct file *file)
{
	struct vhost_blk *blk;
	struct vhost_blk_queue *q;
	int i, j;

	blk = kvzalloc(sizeof(*blk), GFP_KERNEL);
	if (!blk)
		return -ENOMEM;

	for (i = 0; i < VHOST_BLK_VQ_MAX; i++) {
		q = &blk->queue[i];
		q->index = i;
		q->blk = blk;
		q->vq.handle_kick = vhost_blk_handle_guest_kick;
		vhost_work_init(&q->w, vhost_blk_io_done_work);
		blk->vqs[i] = &q->vq;
		for (j = 0; j < VHOST_BLK_VQ_MAX_REQS; j++) {
			q->req[j].index = j;
			q->req[j].q = q;
		}
	}
	vhost_dev_init(&blk->dev, (struct vhost_virtqueue **)&blk->vqs,
		       VHOST_BLK_VQ_MAX);
	file->private_data = blk;

	return 0;
}

static int vhost_blk_release(struct inode *inode, struct file *f)
{
	struct vhost_blk *blk = f->private_data;

	vhost_blk_stop(blk);
	mutex_lock(&blk->dev.mutex);
	vhost_blk_flush(blk);
	vhost_dev_stop(&blk->dev);
	vhost_dev_cleanup(&blk->dev);
	vhost_blk_flush(blk);

	if (blk->backend) {
		fput(blk->backend);
		blk->backend = NULL;
	}

	mutex_unlock(&blk->dev.mutex);
	kvfree(blk);

	return 0;
}

static int vhost_blk_set_features(struct vhost_blk *blk, u64 features)
{
	int i;
	int ret = -EFAULT;

	mutex_lock(&blk->dev.mutex);
	if ((features & (1 << VHOST_F_LOG_ALL)) &&
	    !vhost_log_access_ok(&blk->dev))
		goto out_unlock;

	if ((features & (1ULL << VIRTIO_F_IOMMU_PLATFORM))) {
		if (vhost_init_device_iotlb(&blk->dev, true))
			goto out_unlock;
	}

	for (i = 0; i < VHOST_BLK_VQ_MAX; ++i) {
		struct vhost_virtqueue *vq = blk->vqs[i];

		mutex_lock(&vq->mutex);
		vq->acked_features = features & VHOST_BLK_FEATURES;
		mutex_unlock(&vq->mutex);
	}
	ret = 0;
out_unlock:
	mutex_unlock(&blk->dev.mutex);

	return ret;
}

static long vhost_blk_reset_owner(struct vhost_blk *blk)
{
	long err;
	struct vhost_umem *umem;

	mutex_lock(&blk->dev.mutex);
	err = vhost_dev_check_owner(&blk->dev);
	if (err)
		goto done;
	umem = vhost_dev_reset_owner_prepare();
	if (!umem) {
		err = -ENOMEM;
		goto done;
	}
	vhost_blk_stop(blk);
	vhost_blk_flush(blk);
	vhost_dev_reset_owner(&blk->dev, umem);
done:
	mutex_unlock(&blk->dev.mutex);
	return err;
}

static long vhost_blk_set_backend(struct vhost_blk *blk, int fd)
{
	struct file *backend;
	int ret, i;
	struct vhost_virtqueue *vq;

	mutex_lock(&blk->dev.mutex);
	ret = vhost_dev_check_owner(&blk->dev);
	if (ret)
		goto out_dev;

	backend = fget(fd);
	if (IS_ERR(backend)) {
		ret = PTR_ERR(backend);
		goto out_dev;
	}

	if (backend == blk->backend) {
		ret = 0;
		goto out_file;
	}

	if (blk->backend)
		fput(blk->backend);
	blk->backend = backend;
	for (i = 0; i < blk->num_queues; i++) {
		vq = &blk->queue[i].vq;
		if (!vhost_vq_access_ok(vq)) {
			ret = -EFAULT;
			goto out_file;
		}
		mutex_lock(&vq->mutex);
		rcu_assign_pointer(vq->private_data, backend);
		ret = vhost_vq_init_access(vq);
		mutex_unlock(&vq->mutex);
		if (ret) {
			pr_err("vhost_vq_init_access failed: %d\n", ret);
			goto out_file;
		}

	}
	ret = 0;
	goto out_dev;
out_file:
	fput(backend);
	blk->backend = NULL;
out_dev:
	mutex_unlock(&blk->dev.mutex);
	vhost_blk_flush(blk);
	return ret;
}

static long vhost_blk_pass_ioctl(struct vhost_blk *blk, unsigned int ioctl,
				 void __user *argp)
{
	long ret;

	mutex_lock(&blk->dev.mutex);
	ret = vhost_dev_ioctl(&blk->dev, ioctl, argp);
	if (ret == -ENOIOCTLCMD)
		ret = vhost_vring_ioctl(&blk->dev, ioctl, argp);
	else
		vhost_blk_flush(blk);
	mutex_unlock(&blk->dev.mutex);
	return ret;
}

static long vhost_blk_ioctl(struct file *f, unsigned int ioctl,
			    unsigned long arg)
{
	struct vhost_blk *blk = f->private_data;
	void __user *argp = (void __user *)arg;
	int fd;
	u64 __user *featurep = argp;
	u64 features;
	long ret;
	struct vhost_vring_state s;

	switch (ioctl) {
	case VHOST_SET_MEM_TABLE:
		vhost_blk_stop(blk);
		ret = vhost_blk_pass_ioctl(blk, ioctl, argp);
		break;
	case VHOST_SET_VRING_NUM:
		if (copy_from_user(&s, argp, sizeof(s)))
			return -EFAULT;
		ret = vhost_blk_pass_ioctl(blk, ioctl, argp);
		if (!ret)
			blk->num_queues = s.index + 1;
		break;
	case VHOST_BLK_SET_BACKEND:
		if (copy_from_user(&fd, argp, sizeof(fd)))
			return -EFAULT;
		ret = vhost_blk_set_backend(blk, fd);
		break;
	case VHOST_GET_FEATURES:
		features = VHOST_BLK_FEATURES;
		if (copy_to_user(featurep, &features, sizeof(features)))
			return -EFAULT;
		ret = 0;
		break;
	case VHOST_SET_FEATURES:
		if (copy_from_user(&features, featurep, sizeof(features)))
			return -EFAULT;
		if (features & ~VHOST_BLK_FEATURES)
			return -EOPNOTSUPP;
		ret = vhost_blk_set_features(blk, features);
		break;
	case VHOST_RESET_OWNER:
		ret = vhost_blk_reset_owner(blk);
		break;
	default:
		ret = vhost_blk_pass_ioctl(blk, ioctl, argp);
		break;
	}
	return ret;
}

static const struct file_operations vhost_blk_fops = {
	.owner          = THIS_MODULE,
	.open           = vhost_blk_open,
	.release        = vhost_blk_release,
	.llseek		= noop_llseek,
	.unlocked_ioctl = vhost_blk_ioctl,
};

static struct miscdevice vhost_blk_misc = {
	MISC_DYNAMIC_MINOR,
	"vhost-blk",
	&vhost_blk_fops,
};

static int vhost_blk_init(void)
{
	return misc_register(&vhost_blk_misc);
}
module_init(vhost_blk_init);

static void vhost_blk_exit(void)
{
	misc_deregister(&vhost_blk_misc);
}

module_exit(vhost_blk_exit);

MODULE_VERSION("1.0");
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Vitaly Mayatskikh");
MODULE_DESCRIPTION("Host kernel accelerator for virtio blk");
MODULE_ALIAS("devname:vhost-blk");
