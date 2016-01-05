/*
 * Copyright (C) 2015 Nobuo Iwata
 *
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307,
 * USA.
 */

/*
 * USB/IP userspace transmission module.
 */

#include <linux/module.h>
#include <linux/semaphore.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/cdev.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/rcupdate.h>
#include <linux/file.h>
#include <asm/current.h>
#include <asm/uaccess.h>

#include <uapi/linux/usbip_ux.h>
#include "usbip_common.h"

#define DRIVER_AUTHOR "Nobuo Iwata <nobuo.iwata@fujixerox.co.jp>"
#define DRIVER_DESC "USB/IP Userspace Transfer"

static DEFINE_SEMAPHORE(usbip_ux_lock);
static struct list_head usbip_ux_list;

static int usbip_ux_get(usbip_ux_t *ux)
{
	if (down_interruptible(&ux->lock)) return -ERESTARTSYS;
	atomic_inc(&ux->count);
	up(&ux->lock);
	return 0;
}

static void usbip_ux_put(usbip_ux_t *ux)
{
	atomic_dec(&ux->count);
}

static int usbip_ux_is_linked(usbip_ux_t *ux)
{
	if (ux->ud) {
		return 1;
	}
	return 0;
}

static void usbip_ux_wakeup_all(usbip_ux_t *ux)
{
	wake_up(&ux->tx_req_q);
	wake_up(&ux->tx_rsp_q);
	wake_up(&ux->rx_req_q);
	wake_up(&ux->rx_rsp_q);
}

static ssize_t usbip_ux_read(struct file *file, char __user *buf, size_t blen, loff_t *off)
{
	usbip_ux_t *ux = (usbip_ux_t*)file->private_data;
	int ret, bytes;
	
	usbip_dbg_ux("read waiting.\n");
	ux->tx_error = 0;
	ret = usbip_ux_get(ux);
	if (ret) {
		pr_err("Fail to get ux.\n");
		goto err_out;
	}
	if (!usbip_ux_is_linked(ux)) {
		pr_info("Read from unlinked ux.\n");
		if (USBIP_UX_IS_TX_INT(ux)) {
			ret = -ERESTARTSYS;
			goto err_put_ux;
		}
		ret = 0;
		goto err_put_ux;
	}
	ret = wait_event_interruptible(ux->tx_req_q,
		USBIP_UX_HAS_TX_REQ(ux) || USBIP_UX_IS_TX_INT(ux));
	if (USBIP_UX_IS_TX_INT(ux)) {
		ret = -ERESTARTSYS;
	}
	if (ret) {
		ux->tx_error = ret;
		USBIP_UX_CLEAR_TX_REQ(ux);
		USBIP_UX_SET_TX_RSP(ux);
		wake_up(&ux->tx_rsp_q);
		goto err_put_ux;
	}
	bytes = ux->tx_bytes - ux->tx_count;
	if (blen < bytes) bytes = blen;
	usbip_dbg_ux("read copying %d.\n", bytes);
	ret = copy_to_user(buf, ux->tx_buf + ux->tx_count, bytes);
	if (ret > 0) {
		ux->tx_error = -EIO;
		USBIP_UX_CLEAR_TX_REQ(ux);
		USBIP_UX_SET_TX_RSP(ux);
		wake_up(&ux->tx_rsp_q);
		ret = -EIO;
		goto err_put_ux;
	}
	ux->tx_count += bytes;
	if (ux->tx_count >= ux->tx_bytes) {
		USBIP_UX_CLEAR_TX_REQ(ux);
		USBIP_UX_SET_TX_RSP(ux);
		wake_up(&ux->tx_rsp_q);
	}
	usbip_ux_put(ux);
	return bytes;
err_put_ux:
	usbip_ux_put(ux);
err_out:
	return ret;
}

static int usbip_ux_sendvec(usbip_ux_t *ux, struct kvec *vec)
{
	int ret;

	ux->tx_buf = vec->iov_base;
	ux->tx_bytes = vec->iov_len;
	ux->tx_count = 0;
	ux->tx_error = 0;
	USBIP_UX_CLEAR_TX_RSP(ux);
	USBIP_UX_SET_TX_REQ(ux);
	wake_up(&ux->tx_req_q);
	usbip_dbg_ux("sendvec waiting.\n");
	ret = wait_event_interruptible(ux->tx_rsp_q,
		USBIP_UX_HAS_TX_RSP(ux) || USBIP_UX_IS_TX_INT(ux));
	if (ret) {
		return ret;
	} else if (USBIP_UX_IS_TX_INT(ux)) {
		return -ERESTARTSYS;
	}
	return ux->tx_error;
}

static int usbip_ux_sendmsg(struct usbip_device *ud, struct msghdr *msg, 
		struct kvec *vec, size_t num, size_t size)
{
	int ret, i, count = 0;
	usbip_ux_t *ux;

	usbip_dbg_ux("sendmsg.\n");
	rcu_read_lock();
	ux = rcu_dereference(ud->ux);
	if (ux == NULL) {
		pr_info("Send to unlinked ux (0).\n");
		ret = 0;
		goto err_unlock_rcu;
	}
	ret = usbip_ux_get(ux);
	if (ret) {
		pr_err("Fail to get ux.\n");
		goto err_unlock_rcu;
	} else if (!usbip_ux_is_linked(ux)) {
		pr_info("Send to unlinked ux (1).\n");
		ret = 0;
		goto err_put_ux;
	}
	for(i = 0;i < num;i++) {
		ret = usbip_ux_sendvec(ux, vec+i);
		if (ret) {
			pr_err("Fail to send by %d.\n", ret);
			goto err_put_ux;
		}
		count += ux->tx_count;
	}
	usbip_ux_put(ux);
	rcu_read_unlock();
	usbip_dbg_ux("sendmsg. ok\n");
	return count;
err_put_ux:
	usbip_ux_put(ux);
err_unlock_rcu:
	rcu_read_unlock();
	return ret;
}

static ssize_t usbip_ux_write(struct file *file, const char __user *buf, size_t blen, loff_t *off)
{
	usbip_ux_t *ux = (usbip_ux_t*)file->private_data;
	int ret, bytes;

	usbip_dbg_ux("write waiting.\n");
	ret = usbip_ux_get(ux);
	if (ret) {
		pr_err("Fail to get ux.\n");
		goto err_out;
	} else if (!usbip_ux_is_linked(ux)) {
		pr_info("Write to unlinked ux.\n");
		ret = -EINTR;
		goto err_put_ux;
	}
	ux->rx_error = 0;
	ret = wait_event_interruptible(ux->rx_req_q,
		USBIP_UX_HAS_RX_REQ(ux) || USBIP_UX_IS_RX_INT(ux));
	if (USBIP_UX_IS_RX_INT(ux)) {
		ret = -ERESTARTSYS;
	}
	if (ret) {
		ux->rx_error = ret;
		USBIP_UX_CLEAR_RX_REQ(ux);
		USBIP_UX_SET_RX_RSP(ux);
		wake_up(&ux->rx_rsp_q);
		goto err_put_ux;
	}
	bytes = ux->rx_bytes - ux->rx_count;
	if (blen < bytes) bytes = blen;
	usbip_dbg_ux("write copying %d.\n", bytes);
	ret = copy_from_user(ux->rx_buf + ux->rx_count, buf, bytes);
	if (ret > 0) {
		ux->rx_error = -EIO;
		USBIP_UX_CLEAR_RX_REQ(ux);
		USBIP_UX_SET_RX_RSP(ux);
		wake_up(&ux->rx_rsp_q);
		ret = -EIO;
		goto err_put_ux;
	}
	ux->rx_count += bytes;
	USBIP_UX_CLEAR_RX_REQ(ux);
	USBIP_UX_SET_RX_RSP(ux);
	wake_up(&ux->rx_rsp_q);
	usbip_ux_put(ux);
	return bytes;
err_put_ux:
	usbip_ux_put(ux);
err_out:
	return ret;
}

static int usbip_ux_recvvec(usbip_ux_t *ux, struct kvec *vec)
{
	int ret;

	ux->rx_buf = vec->iov_base;
	ux->rx_bytes = vec->iov_len;
	ux->rx_count = 0;
	ux->rx_error = 0;
	USBIP_UX_CLEAR_RX_RSP(ux);
	USBIP_UX_SET_RX_REQ(ux);
	wake_up(&ux->rx_req_q);
	usbip_dbg_ux("recvvec waiting.\n");
	ret = wait_event_interruptible(ux->rx_rsp_q,
		USBIP_UX_HAS_RX_RSP(ux) || USBIP_UX_IS_RX_INT(ux));
	if (ret) {
		return ret;
	} else if (USBIP_UX_IS_RX_INT(ux)) {
		usbip_dbg_ux("interrupted.\n");
		return -ERESTARTSYS;
	}
	return ux->rx_error;
}

static int usbip_ux_recvmsg(struct usbip_device *ud, struct msghdr *msg,
		struct kvec *vec, size_t num, size_t size, int flags)
{
	int ret, i, count = 0;
	usbip_ux_t *ux;

	usbip_dbg_ux("recvmsg.\n");
	rcu_read_lock();
	ux = rcu_dereference(ud->ux);
	if (ux == NULL) {
		pr_err("Recv from unlinked ux (0).\n");
		ret = 0;
		goto err_unlock_rcu;
	}
	ret = usbip_ux_get(ux);
	if (ret) {
		pr_err("Fail to get ux.\n");
		goto err_unlock_rcu;
	} else if (!usbip_ux_is_linked(ux)) {
		pr_err("Recv from unlinked ux (1).\n");
		ret = 0;
		goto err_put_ux;
	}
	for(i = 0;i < num;i++) {
		usbip_dbg_ux("recvmsg. %d\n", i);
		ret = usbip_ux_recvvec(ux, vec+i);
		if (ret) {
			pr_err("Fail to recv by %d.\n", ret);
			goto err_put_ux;
		}
		usbip_dbg_ux("recvmsg ok. %d\n", i);
		count += ux->rx_count;
	}
	usbip_ux_put(ux);
	rcu_read_unlock();
	usbip_dbg_ux("recvmsg ok.");
	return count;
err_put_ux:
	usbip_ux_put(ux);
err_unlock_rcu:
	rcu_read_unlock();
	return ret;
}

static int usbip_ux_new(usbip_ux_t **uxp)
{
	usbip_ux_t *ux;

	ux = (usbip_ux_t*)kzalloc(sizeof(usbip_ux_t), GFP_KERNEL);
	if (!ux) {
		pr_err("Fail to alloc usbip_ux_t.\n");
		return -ENOMEM;
	}
	sema_init(&ux->lock, 1);
	atomic_set(&ux->count, 0);
	init_waitqueue_head(&ux->tx_req_q);
	init_waitqueue_head(&ux->tx_rsp_q);
	init_waitqueue_head(&ux->rx_req_q);
	init_waitqueue_head(&ux->rx_rsp_q);
	ux->pgid = task_pgrp_vnr(current);
	if (down_interruptible(&usbip_ux_lock)) return -ERESTARTSYS;
	list_add_tail(&ux->node, &usbip_ux_list);
	up(&usbip_ux_lock);
	*uxp = ux;
	return 0;
}

static int usbip_ux_delete(usbip_ux_t *ux)
{
	USBIP_UX_SET_INT(ux);
	usbip_ux_wakeup_all(ux);
	if (down_interruptible(&usbip_ux_lock)) return -ERESTARTSYS;
	if (down_interruptible(&ux->lock)) {
		up(&usbip_ux_lock);
		return -ERESTARTSYS;
	}
	pr_info("Waiting ux becomes free in delete.");
	while(atomic_read(&ux->count) > 0) yield();
	pr_info("End of waiting ux becomes free in delete.");
	list_del(&ux->node);
	if (ux->ud != NULL) {
		rcu_assign_pointer(ux->ud->ux, NULL);
		ux->ud = NULL;
	}
	up(&ux->lock);
	up(&usbip_ux_lock);
	synchronize_rcu();
	pr_info("Releasing ux %p.\n", ux);
	kfree(ux);
	return 0;
}

static int usbip_ux_link(struct usbip_device *ud, int sockfd)
{
	usbip_ux_t *ux;
	struct list_head *p;
	int ret;

	usbip_dbg_ux("linking ud:%p sock:%d\n", ud, sockfd);
	ret = usbip_kernel_link(ud, sockfd);
	if (ret) {
		return ret;
	}
	if (down_interruptible(&usbip_ux_lock)) return -ERESTARTSYS;
	list_for_each(p, &usbip_ux_list) {
		ux = list_entry(p, usbip_ux_t, node);
		if (ux->tcp_socket == ud->tcp_socket) {
			rcu_assign_pointer(ud->ux, ux);
			ux->ud = ud;
			up(&usbip_ux_lock);
			usbip_dbg_ux("linked ud:%p sock:%d ux:%p\n", ud, sockfd, ux);
			return 0;
		}
	}
	up(&usbip_ux_lock);
	usbip_dbg_ux("fail to link ud:%p sock:%d\n", ud, sockfd);
	return -EINVAL;
}

static int usbip_ux_unlink(struct usbip_device *ud)
{
	usbip_ux_t *ux;

	usbip_dbg_ux("unlinking ux:%p\n", ud->ux);
	rcu_read_lock();
	ux = rcu_dereference(ud->ux);
	if (ux == NULL) {
		pr_err("Unlink to unlinked ux.\n");
		rcu_read_unlock();
		return -EINVAL;
	}
	pr_info("Unlink ux sock:%d.\n", ux->sockfd);
	USBIP_UX_SET_INT(ux);
	usbip_ux_wakeup_all(ux);
	if (down_interruptible(&ux->lock)) {
		rcu_read_unlock();
		return -ERESTARTSYS;
	}
	rcu_read_unlock();
	pr_info("Waiting ux becomes free in unlink.");
	while(atomic_read(&ux->count) > 0) yield();
	pr_info("End of waiting ux becomes free in unlink.");
	rcu_assign_pointer(ud->ux, NULL);
	ux->ud = NULL;
	up(&ux->lock);
	return 0;
}

static int usbip_ux_set_sockfd(usbip_ux_t *ux, int sockfd)
{
	int err;

	if (ux->sockfd != 0) {
		return -EBUSY;
	}
	ux->tcp_socket = sockfd_lookup(sockfd, &err);
	if (ux->tcp_socket == NULL) {
		pr_debug("Fail to sock ptr fd:%d pid:%d\n", sockfd, task_pid_nr(current));
		return -EINVAL;
	}
	fput(ux->tcp_socket->file);
	ux->sockfd = sockfd;
	return 0;
}

static int usbip_ux_interrupt(usbip_ux_t *ux)
{
	usbip_dbg_ux("interrupt %p %d\n", ux, ux->sockfd);
	USBIP_UX_SET_INT(ux);
	usbip_ux_wakeup_all(ux);
	return 0;
}

static int usbip_ux_interrupt_pgrp(void)
{
	pid_t pgid;
	struct list_head *p;
	usbip_ux_t *ux;

	pgid = task_pgrp_vnr(current);
	if (down_interruptible(&usbip_ux_lock)) return -ERESTARTSYS;
	list_for_each(p, &usbip_ux_list) {
		ux = list_entry(p, usbip_ux_t, node);
		if (ux->pgid == pgid) {
			usbip_ux_interrupt(ux);
		}
	}
	up(&usbip_ux_lock);
	return 0;
}

static int usbip_ux_getkaddr(usbip_ux_t *ux, void __user *ubuf)
{
	struct usbip_ux_kaddr kaddr = {(void*)ux, (void*)(ux->tcp_socket)};
	if (copy_to_user(ubuf, &kaddr, sizeof(kaddr))) {
		return -EFAULT;
	}
	return 0;
}

static long usbip_ux_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	usbip_ux_t *ux = (usbip_ux_t*)file->private_data;

	if (((_IOC_DIR(cmd) & _IOC_READ) &&
		!access_ok(VERIFY_WRITE, (void __user *)arg, _IOC_SIZE(cmd))) ||
		((_IOC_DIR(cmd) & _IOC_WRITE) &&
		!access_ok(VERIFY_READ, (void __user *)arg, _IOC_SIZE(cmd)))) {
		return -EFAULT;
	}
	switch (_IOC_NR(cmd)) {
	case _IOC_NR(USBIP_UX_IOCSETSOCKFD):
		return usbip_ux_set_sockfd(ux, (int)arg);
	case _IOC_NR(USBIP_UX_IOCINTR):
		return usbip_ux_interrupt(ux);
	case _IOC_NR(USBIP_UX_IOCINTRPGRP):
		return usbip_ux_interrupt_pgrp();
	case _IOC_NR(USBIP_UX_IOCGETKADDR):
		return usbip_ux_getkaddr(ux, (void __user *)arg);
	}
	return -EINVAL;
}

static int usbip_ux_open(struct inode *inode, struct file *file)
{
	usbip_ux_t *ux = NULL;
	int ret;

	ret = usbip_ux_new(&ux);
	if (ux == NULL) {
		pr_err("Fail to alloc usbip_ux_t.\n");
		return -ENOMEM;
	}
	file->private_data = ux;
	return 0;
}

static int usbip_ux_release(struct inode *inode, struct file *file)
{
	usbip_ux_t *ux = file->private_data;

	return usbip_ux_delete(ux);
}

static struct usbip_trx_operations usbip_trx_user_ops = {
	.mode = USBIP_TRX_MODE_USER,
	.sendmsg = usbip_ux_sendmsg,
	.recvmsg = usbip_ux_recvmsg,
	.link = usbip_ux_link,
	.unlink = usbip_ux_unlink,
};

static struct file_operations usbip_ux_fops = {
	.owner = THIS_MODULE,
	.read = usbip_ux_read,
	.write = usbip_ux_write,
	.unlocked_ioctl = usbip_ux_ioctl,
	.compat_ioctl = usbip_ux_ioctl,
	.open = usbip_ux_open,
	.release = usbip_ux_release,
};

static dev_t usbip_ux_devno;
static struct cdev usbip_ux_cdev;
static struct class *usbip_ux_class;
static struct device *usbip_ux_dev;
static struct usbip_trx_operations *usbip_trx_ops_bak;

static int __init usbip_ux_init(void)
{
	int ret;

	usbip_trx_ops_bak = usbip_trx_ops;
	usbip_trx_ops = &usbip_trx_user_ops;

	INIT_LIST_HEAD(&usbip_ux_list);
	ret = alloc_chrdev_region(&usbip_ux_devno, USBIP_UX_MINOR, 1, USBIP_UX_DEV_NAME);
	if (ret < 0) {
		pr_err("Fail to alloc chrdev for %s\n", USBIP_UX_DEV_NAME);
		goto err_out;
	}
	cdev_init(&usbip_ux_cdev, &usbip_ux_fops);
	usbip_ux_cdev.owner = usbip_ux_fops.owner;
	ret = cdev_add(&usbip_ux_cdev, usbip_ux_devno, 1);
	if (ret < 0) {
		pr_err("Fail to add cdev: %s\n", USBIP_UX_DEV_NAME);
		kobject_put(&usbip_ux_cdev.kobj);
		goto err_unregister_chrdev;
	}
	usbip_ux_class = class_create(THIS_MODULE, USBIP_UX_CLASS_NAME);
	if (IS_ERR(usbip_ux_class)) {
		pr_err("Fail to create class: %s\n", USBIP_UX_CLASS_NAME);
		ret = PTR_ERR(usbip_ux_class);
		goto err_cdev_del;
	}
	usbip_ux_dev = device_create(usbip_ux_class, NULL, usbip_ux_devno, NULL, USBIP_UX_DEV_NAME);
	if (IS_ERR(usbip_ux_dev)) {
		pr_err("Fail to create sysfs entry for %s\n", USBIP_UX_DEV_NAME);
		ret = PTR_ERR(usbip_ux_class);
		goto err_cdev_del;
	}
	return 0;
err_cdev_del:
	cdev_del(&usbip_ux_cdev);
err_unregister_chrdev:
	unregister_chrdev_region(usbip_ux_devno, 1);
err_out:
	return ret;
}

static void __exit usbip_ux_exit(void)
{
	device_destroy(usbip_ux_class, usbip_ux_devno);
	class_destroy(usbip_ux_class);
	cdev_del(&usbip_ux_cdev);
	unregister_chrdev_region(usbip_ux_devno, 1);
	usbip_trx_ops = usbip_trx_ops_bak;
}

module_init(usbip_ux_init);
module_exit(usbip_ux_exit);

MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_LICENSE("GPL");
MODULE_VERSION(USBIP_VERSION);

