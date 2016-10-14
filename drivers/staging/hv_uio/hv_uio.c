/*
 * Copyright (c) 2013-2016 Brocade Communications Systems, Inc.
 * Copyright (c) 2016, Microsoft Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, see <http://www.gnu.org/licenses/>.
 *
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/uio_driver.h>
#include <linux/netdevice.h>
#include <linux/if_ether.h>
#include <linux/skbuff.h>
#include <linux/hyperv.h>
#include <linux/vmalloc.h>
#include <linux/slab.h>

#include "../../hv/hyperv_vmbus.h"

/*
 * List of resources to be mapped to user space
 * can be extended up to MAX_UIO_MAPS(5) items
 */
enum hv_uio_map {
	TXRX_RING_MAP = 0,
	INT_PAGE_MAP,
	MON_PAGE_MAP,
};

#define HV_RING_SIZE	512

struct hv_uio_private_data {
	struct uio_info info;
	struct hv_device *device;
};

static int
hv_uio_mmap(struct uio_info *info, struct vm_area_struct *vma)
{
	int mi;

	if (vma->vm_pgoff >= MAX_UIO_MAPS)
		return -EINVAL;

	if (info->mem[vma->vm_pgoff].size == 0)
		return -EINVAL;

	mi = (int)vma->vm_pgoff;

	return remap_pfn_range(vma, vma->vm_start,
			       virt_to_phys((void *)info->mem[mi].addr) >> PAGE_SHIFT,
			       vma->vm_end - vma->vm_start, vma->vm_page_prot);
}

/*
 * This is the irqcontrol callback to be registered to uio_info.
 * It can be used to disable/enable interrupt from user space processes.
 *
 * @param info
 *  pointer to uio_info.
 * @param irq_state
 *  state value. 1 to enable interrupt, 0 to disable interrupt.
 */
static int
hv_uio_irqcontrol(struct uio_info *info, s32 irq_state)
{
	struct hv_uio_private_data *pdata = info->priv;
	struct hv_device *dev = pdata->device;

	dev->channel->inbound.ring_buffer->interrupt_mask = !irq_state;
	virt_mb();

	return 0;
}

/*
 * Callback from vmbus_event when something is in inbound ring.
 */
static void hv_uio_channel_cb(void *context)
{
	struct hv_uio_private_data *pdata = context;
	struct hv_device *dev = pdata->device;

	dev->channel->inbound.ring_buffer->interrupt_mask = 1;
	virt_mb();

	uio_event_notify(&pdata->info);
}

static int
hv_uio_probe(struct hv_device *dev,
	     const struct hv_vmbus_device_id *dev_id)
{
	struct hv_uio_private_data *pdata;
	int ret;

	pdata = kzalloc(sizeof(*pdata), GFP_KERNEL);
	if (!pdata)
		return -ENOMEM;

	dev->channel->inbound.ring_buffer->interrupt_mask = 1;
	dev->channel->batched_reading = false;

	ret = vmbus_open(dev->channel, HV_RING_SIZE * PAGE_SIZE,
			 HV_RING_SIZE * PAGE_SIZE, NULL, 0,
			 hv_uio_channel_cb, pdata);
	if (ret)
		goto fail;

	/* Fill general uio info */
	pdata->info.name = "hv_uio";
	pdata->info.version = "0.1";
	pdata->info.irqcontrol = hv_uio_irqcontrol;
	pdata->info.mmap = hv_uio_mmap;
	pdata->info.irq = UIO_IRQ_CUSTOM;

	/* mem resources */
	pdata->info.mem[TXRX_RING_MAP].name = "txrx_rings";
	pdata->info.mem[TXRX_RING_MAP].addr
		= (phys_addr_t)dev->channel->ringbuffer_pages;
	pdata->info.mem[TXRX_RING_MAP].size
		= dev->channel->ringbuffer_pagecount * PAGE_SIZE;
	pdata->info.mem[TXRX_RING_MAP].memtype = UIO_MEM_LOGICAL;

	pdata->info.mem[INT_PAGE_MAP].name = "int_page";
	pdata->info.mem[INT_PAGE_MAP].addr = (phys_addr_t)vmbus_connection.int_page;
	pdata->info.mem[INT_PAGE_MAP].size = PAGE_SIZE;
	pdata->info.mem[INT_PAGE_MAP].memtype = UIO_MEM_LOGICAL;

	pdata->info.mem[MON_PAGE_MAP].name = "monitor_pages";
	pdata->info.mem[MON_PAGE_MAP].addr = (phys_addr_t)vmbus_connection.monitor_pages[1];
	pdata->info.mem[MON_PAGE_MAP].size = PAGE_SIZE;
	pdata->info.mem[MON_PAGE_MAP].memtype = UIO_MEM_LOGICAL;

	pdata->info.priv = pdata;
	pdata->device = dev;

	ret = uio_register_device(&dev->device, &pdata->info);
	if (ret) {
		dev_err(&dev->device, "hv_uio register failed\n");
		goto fail_close;
	}

	hv_set_drvdata(dev, pdata);

	dev_info(&dev->device, "hv_uio device registered\n");

	return 0;

fail_close:
	vmbus_close(dev->channel);
fail:
	kfree(pdata);

	return ret;
}

static int
hv_uio_remove(struct hv_device *dev)
{
	struct hv_uio_private_data *pdata = hv_get_drvdata(dev);

	if (!pdata)
		return 0;

	pr_devel("unregister hyperv driver for hv_device {%pUl}\n",
		 dev->dev_instance.b);

	uio_unregister_device(&pdata->info);
	hv_set_drvdata(dev, NULL);
	vmbus_close(dev->channel);
	kfree(pdata);
	return 0;
}

/*
 * The device table is intentionally left blank so that
 * this device driver is not automatically bound to any device.
 */
static const struct hv_vmbus_device_id hyperv_id_table[] = {
	{ },
};

MODULE_DEVICE_TABLE(vmbus, hyperv_id_table);

static struct hv_driver hv_uio_drv = {
	.name = KBUILD_MODNAME,
	.id_table = hyperv_id_table,
	.probe = hv_uio_probe,
	.remove = hv_uio_remove,
};

static int __init
hyperv_module_init(void)
{
	return vmbus_driver_register(&hv_uio_drv);
}

static void __exit
hyperv_module_exit(void)
{
	vmbus_driver_unregister(&hv_uio_drv);
}

module_init(hyperv_module_init);
module_exit(hyperv_module_exit);

MODULE_DESCRIPTION("UIO driver for Hyper-V");
MODULE_LICENSE("GPL");
