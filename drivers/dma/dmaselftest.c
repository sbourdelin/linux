/*
 * DMA self test code borrowed from Qualcomm Technologies HIDMA driver
 *
 * Copyright (c) 2015, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/dmaengine.h>
#include <linux/dma-mapping.h>
#include <linux/list.h>
#include <linux/atomic.h>
#include <linux/wait.h>
#include <linux/sched.h>
#include <linux/slab.h>

struct test_result {
	atomic_t counter;
	wait_queue_head_t wq;
	struct dma_device *dmadev;
};

static void dma_selftest_complete(void *arg)
{
	struct test_result *result = arg;
	struct dma_device *dmadev = result->dmadev;

	atomic_inc(&result->counter);
	wake_up(&result->wq);
	dev_dbg(dmadev->dev, "self test transfer complete :%d\n",
		atomic_read(&result->counter));
}

/*
 * Perform a transaction to verify the HW works.
 */
static int dma_selftest_sg(struct dma_device *dmadev,
			struct dma_chan *dma_chanptr, u64 size,
			unsigned long flags)
{
	dma_addr_t src_dma, dest_dma, dest_dma_it;
	u8 *dest_buf;
	u32 i, j = 0;
	dma_cookie_t cookie;
	struct dma_async_tx_descriptor *tx;
	int err = 0;
	int ret;
	struct sg_table sg_table;
	struct scatterlist	*sg;
	int nents = 10, count;
	bool free_channel = 1;
	u8 *src_buf;
	int map_count;
	struct test_result result;

	init_waitqueue_head(&result.wq);
	atomic_set(&result.counter, 0);
	result.dmadev = dmadev;

	if (!dma_chanptr)
		return -ENOMEM;

	if (dmadev->device_alloc_chan_resources(dma_chanptr) < 1)
		return -ENODEV;

	if (!dma_chanptr->device || !dmadev->dev) {
		dmadev->device_free_chan_resources(dma_chanptr);
		return -ENODEV;
	}

	ret = sg_alloc_table(&sg_table, nents, GFP_KERNEL);
	if (ret) {
		err = ret;
		goto sg_table_alloc_failed;
	}

	for_each_sg(sg_table.sgl, sg, nents, i) {
		u64 alloc_sz;
		void *cpu_addr;

		alloc_sz = round_up(size, nents);
		do_div(alloc_sz, nents);
		cpu_addr = kmalloc(alloc_sz, GFP_KERNEL);

		if (!cpu_addr) {
			err = -ENOMEM;
			goto sg_buf_alloc_failed;
		}

		dev_dbg(dmadev->dev, "set sg buf[%d] :%p\n", i, cpu_addr);
		sg_set_buf(sg, cpu_addr, alloc_sz);
	}

	dest_buf = kmalloc(round_up(size, nents), GFP_KERNEL);
	if (!dest_buf) {
		err = -ENOMEM;
		goto dst_alloc_failed;
	}
	dev_dbg(dmadev->dev, "dest:%p\n", dest_buf);

	/* Fill in src buffer */
	count = 0;
	for_each_sg(sg_table.sgl, sg, nents, i) {
		src_buf = sg_virt(sg);
		dev_dbg(dmadev->dev,
			"set src[%d, %d, %p] = %d\n", i, j, src_buf, count);

		for (j = 0; j < sg_dma_len(sg); j++)
			src_buf[j] = count++;
	}

	/* dma_map_sg cleans and invalidates the cache in arm64 when
	 * DMA_TO_DEVICE is selected for src. That's why, we need to do
	 * the mapping after the data is copied.
	 */
	map_count = dma_map_sg(dmadev->dev, sg_table.sgl, nents,
				DMA_TO_DEVICE);
	if (!map_count) {
		err =  -EINVAL;
		goto src_map_failed;
	}

	dest_dma = dma_map_single(dmadev->dev, dest_buf,
				size, DMA_FROM_DEVICE);

	err = dma_mapping_error(dmadev->dev, dest_dma);
	if (err)
		goto dest_map_failed;

	/* check scatter gather list contents */
	for_each_sg(sg_table.sgl, sg, map_count, i)
		dev_dbg(dmadev->dev,
			"[%d/%d] src va=%p, iova = %pa len:%d\n",
			i, map_count, sg_virt(sg), &sg_dma_address(sg),
			sg_dma_len(sg));

	dest_dma_it = dest_dma;
	for_each_sg(sg_table.sgl, sg, map_count, i) {
		src_buf = sg_virt(sg);
		src_dma = sg_dma_address(sg);
		dev_dbg(dmadev->dev, "src_dma: %pad dest_dma:%pad\n",
			&src_dma, &dest_dma_it);

		tx = dmadev->device_prep_dma_memcpy(dma_chanptr, dest_dma_it,
				src_dma, sg_dma_len(sg), flags);
		if (!tx) {
			dev_err(dmadev->dev,
				"Self-test sg failed, disabling\n");
			err = -ENODEV;
			goto prep_memcpy_failed;
		}

		tx->callback_param = &result;
		tx->callback = dma_selftest_complete;
		cookie = tx->tx_submit(tx);
		dest_dma_it += sg_dma_len(sg);
	}

	dmadev->device_issue_pending(dma_chanptr);

	/*
	 * It is assumed that the hardware can move the data within 1s
	 * and signal the OS of the completion
	 */
	ret = wait_event_timeout(result.wq,
		atomic_read(&result.counter) == (map_count),
				msecs_to_jiffies(10000));

	if (ret <= 0) {
		dev_err(dmadev->dev,
			"Self-test sg copy timed out, disabling\n");
		err = -ENODEV;
		goto tx_status;
	}
	dev_dbg(dmadev->dev,
		"Self-test complete signal received\n");

	if (dmadev->device_tx_status(dma_chanptr, cookie, NULL) !=
				DMA_COMPLETE) {
		dev_err(dmadev->dev,
			"Self-test sg status not complete, disabling\n");
		err = -ENODEV;
		goto tx_status;
	}

	dma_sync_single_for_cpu(dmadev->dev, dest_dma, size,
				DMA_FROM_DEVICE);

	count = 0;
	for_each_sg(sg_table.sgl, sg, map_count, i) {
		src_buf = sg_virt(sg);
		if (memcmp(src_buf, &dest_buf[count], sg_dma_len(sg)) == 0) {
			count += sg_dma_len(sg);
			continue;
		}

		for (j = 0; j < sg_dma_len(sg); j++) {
			if (src_buf[j] != dest_buf[count]) {
				dev_dbg(dmadev->dev,
				"[%d, %d] (%p) src :%x dest (%p):%x cnt:%d\n",
					i, j, &src_buf[j], src_buf[j],
					&dest_buf[count], dest_buf[count],
					count);
				dev_err(dmadev->dev,
				 "Self-test copy failed compare, disabling\n");
				err = -EFAULT;
				return err;
				goto compare_failed;
			}
			count++;
		}
	}

	/*
	 * do not release the channel
	 * we want to consume all the channels on self test
	 */
	free_channel = 0;

compare_failed:
tx_status:
prep_memcpy_failed:
	dma_unmap_single(dmadev->dev, dest_dma, size,
			 DMA_FROM_DEVICE);
dest_map_failed:
	dma_unmap_sg(dmadev->dev, sg_table.sgl, nents,
			DMA_TO_DEVICE);

src_map_failed:
	kfree(dest_buf);

dst_alloc_failed:
sg_buf_alloc_failed:
	for_each_sg(sg_table.sgl, sg, nents, i) {
		if (sg_virt(sg))
			kfree(sg_virt(sg));
	}
	sg_free_table(&sg_table);
sg_table_alloc_failed:
	if (free_channel)
		dmadev->device_free_chan_resources(dma_chanptr);

	return err;
}

/*
 * Perform a streaming transaction to verify the HW works.
 */
static int dma_selftest_streaming(struct dma_device *dmadev,
			struct dma_chan *dma_chanptr, u64 size,
			unsigned long flags)
{
	dma_addr_t src_dma, dest_dma;
	u8 *dest_buf, *src_buf;
	u32 i;
	dma_cookie_t cookie;
	struct dma_async_tx_descriptor *tx;
	int err = 0;
	int ret;
	bool free_channel = 1;
	struct test_result result;

	init_waitqueue_head(&result.wq);
	atomic_set(&result.counter, 0);
	result.dmadev = dmadev;

	if (!dma_chanptr)
		return -ENOMEM;

	if (dmadev->device_alloc_chan_resources(dma_chanptr) < 1)
		return -ENODEV;

	if (!dma_chanptr->device || !dmadev->dev) {
		dmadev->device_free_chan_resources(dma_chanptr);
		return -ENODEV;
	}

	src_buf = kmalloc(size, GFP_KERNEL);
	if (!src_buf) {
		err = -ENOMEM;
		goto src_alloc_failed;
	}

	dest_buf = kmalloc(size, GFP_KERNEL);
	if (!dest_buf) {
		err = -ENOMEM;
		goto dst_alloc_failed;
	}

	dev_dbg(dmadev->dev, "src: %p dest:%p\n", src_buf, dest_buf);

	/* Fill in src buffer */
	for (i = 0; i < size; i++)
		src_buf[i] = (u8)i;

	/* dma_map_single cleans and invalidates the cache in arm64 when
	 * DMA_TO_DEVICE is selected for src. That's why, we need to do
	 * the mapping after the data is copied.
	 */
	src_dma = dma_map_single(dmadev->dev, src_buf,
				 size, DMA_TO_DEVICE);

	err = dma_mapping_error(dmadev->dev, src_dma);
	if (err)
		goto src_map_failed;

	dest_dma = dma_map_single(dmadev->dev, dest_buf,
				size, DMA_FROM_DEVICE);

	err = dma_mapping_error(dmadev->dev, dest_dma);
	if (err)
		goto dest_map_failed;
	dev_dbg(dmadev->dev, "src_dma: %pad dest_dma:%pad\n", &src_dma,
		&dest_dma);
	tx = dmadev->device_prep_dma_memcpy(dma_chanptr, dest_dma, src_dma,
					size, flags);
	if (!tx) {
		dev_err(dmadev->dev,
			"Self-test streaming failed, disabling\n");
		err = -ENODEV;
		goto prep_memcpy_failed;
	}

	tx->callback_param = &result;
	tx->callback = dma_selftest_complete;
	cookie = tx->tx_submit(tx);
	dmadev->device_issue_pending(dma_chanptr);

	/*
	 * It is assumed that the hardware can move the data within 1s
	 * and signal the OS of the completion
	 */
	ret = wait_event_timeout(result.wq,
				atomic_read(&result.counter) == 1,
				msecs_to_jiffies(10000));

	if (ret <= 0) {
		dev_err(dmadev->dev,
			"Self-test copy timed out, disabling\n");
		err = -ENODEV;
		goto tx_status;
	}
	dev_dbg(dmadev->dev, "Self-test complete signal received\n");

	if (dmadev->device_tx_status(dma_chanptr, cookie, NULL) !=
				DMA_COMPLETE) {
		dev_err(dmadev->dev,
			"Self-test copy timed out, disabling\n");
		err = -ENODEV;
		goto tx_status;
	}

	dma_sync_single_for_cpu(dmadev->dev, dest_dma, size,
				DMA_FROM_DEVICE);

	if (memcmp(src_buf, dest_buf, size)) {
		for (i = 0; i < size/4; i++) {
			if (((u32 *)src_buf)[i] != ((u32 *)(dest_buf))[i]) {
				dev_dbg(dmadev->dev,
					"[%d] src data:%x dest data:%x\n",
					i, ((u32 *)src_buf)[i],
					((u32 *)(dest_buf))[i]);
				break;
			}
		}
		dev_err(dmadev->dev,
			"Self-test copy failed compare, disabling\n");
		err = -EFAULT;
		goto compare_failed;
	}

	/*
	 * do not release the channel
	 * we want to consume all the channels on self test
	 */
	free_channel = 0;

compare_failed:
tx_status:
prep_memcpy_failed:
	dma_unmap_single(dmadev->dev, dest_dma, size,
			 DMA_FROM_DEVICE);
dest_map_failed:
	dma_unmap_single(dmadev->dev, src_dma, size,
			DMA_TO_DEVICE);

src_map_failed:
	kfree(dest_buf);

dst_alloc_failed:
	kfree(src_buf);

src_alloc_failed:
	if (free_channel)
		dmadev->device_free_chan_resources(dma_chanptr);

	return err;
}

/*
 * Perform a coherent transaction to verify the HW works.
 */
static int dma_selftest_one_coherent(struct dma_device *dmadev,
			struct dma_chan *dma_chanptr, u64 size,
			unsigned long flags)
{
	dma_addr_t src_dma, dest_dma;
	u8 *dest_buf, *src_buf;
	u32 i;
	dma_cookie_t cookie;
	struct dma_async_tx_descriptor *tx;
	int err = 0;
	int ret;
	bool free_channel = true;
	struct test_result result;

	init_waitqueue_head(&result.wq);
	atomic_set(&result.counter, 0);
	result.dmadev = dmadev;

	if (!dma_chanptr)
		return -ENOMEM;

	if (dmadev->device_alloc_chan_resources(dma_chanptr) < 1)
		return -ENODEV;

	if (!dma_chanptr->device || !dmadev->dev) {
		dmadev->device_free_chan_resources(dma_chanptr);
		return -ENODEV;
	}

	src_buf = dma_alloc_coherent(dmadev->dev, size,
				&src_dma, GFP_KERNEL);
	if (!src_buf) {
		err = -ENOMEM;
		goto src_alloc_failed;
	}

	dest_buf = dma_alloc_coherent(dmadev->dev, size,
				&dest_dma, GFP_KERNEL);
	if (!dest_buf) {
		err = -ENOMEM;
		goto dst_alloc_failed;
	}

	dev_dbg(dmadev->dev, "src: %p dest:%p\n", src_buf, dest_buf);

	/* Fill in src buffer */
	for (i = 0; i < size; i++)
		src_buf[i] = (u8)i;

	dev_dbg(dmadev->dev, "src_dma: %pad dest_dma:%pad\n", &src_dma,
		&dest_dma);
	tx = dmadev->device_prep_dma_memcpy(dma_chanptr, dest_dma, src_dma,
					size,
					flags);
	if (!tx) {
		dev_err(dmadev->dev,
			"Self-test coherent failed, disabling\n");
		err = -ENODEV;
		goto prep_memcpy_failed;
	}

	tx->callback_param = &result;
	tx->callback = dma_selftest_complete;
	cookie = tx->tx_submit(tx);
	dmadev->device_issue_pending(dma_chanptr);

	/*
	 * It is assumed that the hardware can move the data within 1s
	 * and signal the OS of the completion
	 */
	ret = wait_event_timeout(result.wq,
				atomic_read(&result.counter) == 1,
				msecs_to_jiffies(10000));

	if (ret <= 0) {
		dev_err(dmadev->dev,
			"Self-test copy timed out, disabling\n");
		err = -ENODEV;
		goto tx_status;
	}
	dev_dbg(dmadev->dev, "Self-test complete signal received\n");

	if (dmadev->device_tx_status(dma_chanptr, cookie, NULL) !=
				DMA_COMPLETE) {
		dev_err(dmadev->dev,
			"Self-test copy timed out, disabling\n");
		err = -ENODEV;
		goto tx_status;
	}

	if (memcmp(src_buf, dest_buf, size)) {
		for (i = 0; i < size/4; i++) {
			if (((u32 *)src_buf)[i] != ((u32 *)(dest_buf))[i]) {
				dev_dbg(dmadev->dev,
					"[%d] src data:%x dest data:%x\n",
					i, ((u32 *)src_buf)[i],
					((u32 *)(dest_buf))[i]);
				break;
			}
		}
		dev_err(dmadev->dev,
			"Self-test copy failed compare, disabling\n");
		err = -EFAULT;
		goto compare_failed;
	}

	/*
	 * do not release the channel
	 * we want to consume all the channels on self test
	 */
	free_channel = 0;

compare_failed:
tx_status:
prep_memcpy_failed:
	dma_free_coherent(dmadev->dev, size, dest_buf, dest_dma);

dst_alloc_failed:
	dma_free_coherent(dmadev->dev, size, src_buf, src_dma);

src_alloc_failed:
	if (free_channel)
		dmadev->device_free_chan_resources(dma_chanptr);

	return err;
}

static int dma_selftest_all(struct dma_device *dmadev,
				bool req_coherent, bool req_sg)
{
	int rc = -ENODEV, i = 0;
	struct dma_chan **dmach_ptr = NULL;
	u32 max_channels = 0;
	u64 sizes[] = {PAGE_SIZE - 1, PAGE_SIZE, PAGE_SIZE + 1, 2801, 13295};
	int count = 0;
	u32 j;
	u64 size;
	int failed = 0;
	struct dma_chan *dmach = NULL;

	list_for_each_entry(dmach, &dmadev->channels,
			device_node) {
		max_channels++;
	}

	dmach_ptr = kcalloc(max_channels, sizeof(*dmach_ptr), GFP_KERNEL);
	if (!dmach_ptr) {
		rc = -ENOMEM;
		goto failed_exit;
	}

	for (j = 0; j < ARRAY_SIZE(sizes); j++) {
		size = sizes[j];
		count = 0;
		dev_dbg(dmadev->dev, "test start for size:%llx\n", size);
		list_for_each_entry(dmach, &dmadev->channels,
				device_node) {
			dmach_ptr[count] = dmach;
			if (req_coherent)
				rc = dma_selftest_one_coherent(dmadev,
					dmach, size,
					DMA_PREP_INTERRUPT | DMA_CTRL_ACK);
			else if (req_sg)
				rc = dma_selftest_sg(dmadev,
					dmach, size,
					DMA_PREP_INTERRUPT | DMA_CTRL_ACK);
			else
				rc = dma_selftest_streaming(dmadev,
					dmach, size,
					DMA_PREP_INTERRUPT | DMA_CTRL_ACK);
			if (rc) {
				failed = 1;
				break;
			}
			dev_dbg(dmadev->dev,
				"self test passed for ch:%d\n", count);
			count++;
		}

		/*
		 * free the channels where the test passed
		 * Channel resources are freed for a test that fails.
		 */
		for (i = 0; i < count; i++)
			dmadev->device_free_chan_resources(dmach_ptr[i]);

		if (failed)
			break;
	}

failed_exit:
	kfree(dmach_ptr);

	return rc;
}

static int dma_selftest_mapsngle(struct device *dev)
{
	u32 buf_size = 256;
	char *src;
	int ret = -ENOMEM;
	dma_addr_t dma_src;

	src = kmalloc(buf_size, GFP_KERNEL);
	if (!src)
		return -ENOMEM;

	strcpy(src, "hello world");

	dma_src = dma_map_single(dev, src, buf_size, DMA_TO_DEVICE);
	dev_dbg(dev, "mapsingle: src:%p src_dma:%pad\n", src, &dma_src);

	ret = dma_mapping_error(dev, dma_src);
	if (ret) {
		dev_err(dev, "dma_mapping_error with ret:%d\n", ret);
		ret = -ENOMEM;
	} else {
		if (strcmp(src, "hello world") != 0) {
			dev_err(dev, "memory content mismatch\n");
			ret = -EINVAL;
		} else
			dev_dbg(dev, "mapsingle:dma_map_single works\n");

		dma_unmap_single(dev, dma_src, buf_size, DMA_TO_DEVICE);
	}
	kfree(src);
	return ret;
}

/*
 * Self test all DMA channels.
 */
int dma_selftest_memcpy(struct dma_device *dmadev)
{
	int rc;

	dma_selftest_mapsngle(dmadev->dev);

	/* streaming test */
	rc = dma_selftest_all(dmadev, false, false);
	if (rc)
		return rc;
	dev_dbg(dmadev->dev, "streaming self test passed\n");

	/* coherent test */
	rc = dma_selftest_all(dmadev, true, false);
	if (rc)
		return rc;

	dev_dbg(dmadev->dev, "coherent self test passed\n");

	/* scatter gather test */
	rc = dma_selftest_all(dmadev, false, true);
	if (rc)
		return rc;

	dev_dbg(dmadev->dev, "scatter gather self test passed\n");
	return 0;
}
EXPORT_SYMBOL_GPL(dma_selftest_memcpy);
