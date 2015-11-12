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
	struct dma_device *dma;
};

static void dma_selftest_complete(void *arg)
{
	struct test_result *result = arg;
	struct dma_device *dma = result->dma;

	atomic_inc(&result->counter);
	wake_up(&result->wq);
	dev_dbg(dma->dev, "self test transfer complete :%d\n",
		atomic_read(&result->counter));
}

/*
 * Perform a transaction to verify the HW works.
 */
static int dma_selftest_sg(struct dma_device *dma,
			struct dma_chan *chan, u64 size,
			unsigned long flags)
{
	struct dma_async_tx_descriptor *tx;
	struct sg_table sg_table;
	struct scatterlist *sg;
	struct test_result result;
	dma_addr_t src, dest, dest_it;
	u8 *src_buf, *dest_buf;
	unsigned int i, j;
	dma_cookie_t cookie;
	int err;
	int nents = 10, count;
	bool free_channel = true;
	int map_count;

	init_waitqueue_head(&result.wq);
	atomic_set(&result.counter, 0);
	result.dma = dma;

	if (!chan)
		return -ENOMEM;

	if (dma->device_alloc_chan_resources(chan) < 1)
		return -ENODEV;

	if (!chan->device || !dma->dev) {
		dma->device_free_chan_resources(chan);
		return -ENODEV;
	}

	err = sg_alloc_table(&sg_table, nents, GFP_KERNEL);
	if (err)
		goto sg_table_alloc_failed;

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

		dev_dbg(dma->dev, "set sg buf[%d] :%p\n", i, cpu_addr);
		sg_set_buf(sg, cpu_addr, alloc_sz);
	}

	dest_buf = kmalloc(round_up(size, nents), GFP_KERNEL);
	if (!dest_buf) {
		err = -ENOMEM;
		goto dst_alloc_failed;
	}
	dev_dbg(dma->dev, "dest:%p\n", dest_buf);

	/* Fill in src buffer */
	count = 0;
	for_each_sg(sg_table.sgl, sg, nents, i) {
		src_buf = sg_virt(sg);
		dev_dbg(dma->dev,
			"set src[%d, %p] = %d\n", i, src_buf, count);

		for (j = 0; j < sg_dma_len(sg); j++)
			src_buf[j] = count++;
	}

	/* dma_map_sg cleans and invalidates the cache in arm64 when
	 * DMA_TO_DEVICE is selected for src. That's why, we need to do
	 * the mapping after the data is copied.
	 */
	map_count = dma_map_sg(dma->dev, sg_table.sgl, nents, DMA_TO_DEVICE);
	if (!map_count) {
		err =  -EINVAL;
		goto src_map_failed;
	}

	dest = dma_map_single(dma->dev, dest_buf, size, DMA_FROM_DEVICE);

	err = dma_mapping_error(dma->dev, dest);
	if (err)
		goto dest_map_failed;

	/* check scatter gather list contents */
	for_each_sg(sg_table.sgl, sg, map_count, i)
		dev_dbg(dma->dev, "[%d/%d] src va=%p, iova = %pa len:%d\n",
			i, map_count, sg_virt(sg), &sg_dma_address(sg),
			sg_dma_len(sg));

	dest_it = dest;
	for_each_sg(sg_table.sgl, sg, map_count, i) {
		src_buf = sg_virt(sg);
		src = sg_dma_address(sg);
		dev_dbg(dma->dev, "src: %pad dest:%pad\n",
			&src, &dest_it);

		tx = dma->device_prep_dma_memcpy(chan, dest_it, src,
					sg_dma_len(sg), flags);
		if (!tx) {
			dev_err(dma->dev,
				"Self-test sg failed, disabling\n");
			err = -ENODEV;
			goto prep_memcpy_failed;
		}

		tx->callback_param = &result;
		tx->callback = dma_selftest_complete;
		cookie = tx->tx_submit(tx);
		dest_it += sg_dma_len(sg);
	}

	dma->device_issue_pending(chan);

	/*
	 * It is assumed that the hardware can move the data within 1s
	 * and signal the OS of the completion
	 */
	err = wait_event_timeout(result.wq,
				atomic_read(&result.counter) == (map_count),
				msecs_to_jiffies(10000));

	if (err <= 0) {
		dev_err(dma->dev, "Self-test sg copy timed out, disabling\n");
		err = -ENODEV;
		goto tx_status;
	}
	err = 0;
	dev_dbg(dma->dev,
		"Self-test complete signal received\n");

	if (dma->device_tx_status(chan, cookie, NULL) != DMA_COMPLETE) {
		dev_err(dma->dev,
			"Self-test sg status not complete, disabling\n");
		err = -ENODEV;
		goto tx_status;
	}

	dma_sync_single_for_cpu(dma->dev, dest, size, DMA_FROM_DEVICE);

	count = 0;
	for_each_sg(sg_table.sgl, sg, map_count, i) {
		src_buf = sg_virt(sg);
		if (memcmp(src_buf, &dest_buf[count], sg_dma_len(sg)) == 0) {
			count += sg_dma_len(sg);
			continue;
		}

		for (j = 0; j < sg_dma_len(sg); j++) {
			if (src_buf[j] != dest_buf[count]) {
				dev_dbg(dma->dev,
				"[%d, %d] (%p) src :%x dest (%p):%x cnt:%d\n",
					i, j, &src_buf[j], src_buf[j],
					&dest_buf[count], dest_buf[count],
					count);
				dev_err(dma->dev,
				 "Self-test copy failed compare, disabling\n");
				err = -EFAULT;
				goto compare_failed;
			}
			count++;
		}
	}

	/*
	 * do not release the channel
	 * we want to consume all the channels on self test
	 */
	free_channel = false;

compare_failed:
tx_status:
prep_memcpy_failed:
	dma_unmap_single(dma->dev, dest, size, DMA_FROM_DEVICE);
dest_map_failed:
	dma_unmap_sg(dma->dev, sg_table.sgl, nents, DMA_TO_DEVICE);

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
		dma->device_free_chan_resources(chan);

	return err;
}

/*
 * Perform a streaming transaction to verify the HW works.
 */
static int dma_selftest_streaming(struct dma_device *dma,
			struct dma_chan *chan, u64 size,
			unsigned long flags)
{
	struct dma_async_tx_descriptor *tx;
	struct test_result result;
	dma_addr_t src, dest;
	u8 *dest_buf, *src_buf;
	unsigned int i;
	dma_cookie_t cookie;
	int err;
	bool free_channel = true;

	init_waitqueue_head(&result.wq);
	atomic_set(&result.counter, 0);
	result.dma = dma;

	if (!chan)
		return -ENOMEM;

	if (dma->device_alloc_chan_resources(chan) < 1)
		return -ENODEV;

	if (!chan->device || !dma->dev) {
		dma->device_free_chan_resources(chan);
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

	dev_dbg(dma->dev, "src: %p dest:%p\n", src_buf, dest_buf);

	/* Fill in src buffer */
	for (i = 0; i < size; i++)
		src_buf[i] = (u8)i;

	/* dma_map_single cleans and invalidates the cache in arm64 when
	 * DMA_TO_DEVICE is selected for src. That's why, we need to do
	 * the mapping after the data is copied.
	 */
	src = dma_map_single(dma->dev, src_buf, size, DMA_TO_DEVICE);

	err = dma_mapping_error(dma->dev, src);
	if (err)
		goto src_map_failed;

	dest = dma_map_single(dma->dev, dest_buf, size, DMA_FROM_DEVICE);

	err = dma_mapping_error(dma->dev, dest);
	if (err)
		goto dest_map_failed;
	dev_dbg(dma->dev, "src: %pad dest:%pad\n", &src, &dest);
	tx = dma->device_prep_dma_memcpy(chan, dest, src,
					size, flags);
	if (!tx) {
		dev_err(dma->dev, "Self-test streaming failed, disabling\n");
		err = -ENODEV;
		goto prep_memcpy_failed;
	}

	tx->callback_param = &result;
	tx->callback = dma_selftest_complete;
	cookie = tx->tx_submit(tx);
	dma->device_issue_pending(chan);

	/*
	 * It is assumed that the hardware can move the data within 1s
	 * and signal the OS of the completion
	 */
	err = wait_event_timeout(result.wq,
				atomic_read(&result.counter) == 1,
				msecs_to_jiffies(10000));

	if (err <= 0) {
		dev_err(dma->dev, "Self-test copy timed out, disabling\n");
		err = -ENODEV;
		goto tx_status;
	}
	err = 0;
	dev_dbg(dma->dev, "Self-test complete signal received\n");

	if (dma->device_tx_status(chan, cookie, NULL) != DMA_COMPLETE) {
		dev_err(dma->dev,
			"Self-test copy timed out, disabling\n");
		err = -ENODEV;
		goto tx_status;
	}

	dma_sync_single_for_cpu(dma->dev, dest, size, DMA_FROM_DEVICE);

	if (memcmp(src_buf, dest_buf, size)) {
		for (i = 0; i < size/4; i++) {
			if (((u32 *)src_buf)[i] != ((u32 *)(dest_buf))[i]) {
				dev_dbg(dma->dev,
					"[%d] src data:%x dest data:%x\n",
					i, ((u32 *)src_buf)[i],
					((u32 *)(dest_buf))[i]);
				break;
			}
		}
		dev_err(dma->dev,
			"Self-test copy failed compare, disabling\n");
		err = -EFAULT;
		goto compare_failed;
	}

	/*
	 * do not release the channel
	 * we want to consume all the channels on self test
	 */
	free_channel = false;

compare_failed:
tx_status:
prep_memcpy_failed:
	dma_unmap_single(dma->dev, dest, size,
			 DMA_FROM_DEVICE);
dest_map_failed:
	dma_unmap_single(dma->dev, src, size,
			DMA_TO_DEVICE);

src_map_failed:
	kfree(dest_buf);

dst_alloc_failed:
	kfree(src_buf);

src_alloc_failed:
	if (free_channel)
		dma->device_free_chan_resources(chan);

	return err;
}

/*
 * Perform a coherent transaction to verify the HW works.
 */
static int dma_selftest_one_coherent(struct dma_device *dma,
			struct dma_chan *chan, u64 size, unsigned long flags)
{
	struct dma_async_tx_descriptor *tx;
	struct test_result result;
	dma_addr_t src, dest;
	u8 *dest_buf, *src_buf;
	unsigned int i;
	dma_cookie_t cookie;
	int err;
	bool free_channel = true;

	init_waitqueue_head(&result.wq);
	atomic_set(&result.counter, 0);
	result.dma = dma;

	if (!chan)
		return -ENOMEM;

	if (dma->device_alloc_chan_resources(chan) < 1)
		return -ENODEV;

	if (!chan->device || !dma->dev) {
		dma->device_free_chan_resources(chan);
		return -ENODEV;
	}

	src_buf = dma_alloc_coherent(dma->dev, size, &src, GFP_KERNEL);
	if (!src_buf) {
		err = -ENOMEM;
		goto src_alloc_failed;
	}

	dest_buf = dma_alloc_coherent(dma->dev, size, &dest, GFP_KERNEL);
	if (!dest_buf) {
		err = -ENOMEM;
		goto dst_alloc_failed;
	}

	dev_dbg(dma->dev, "src: %p dest:%p\n", src_buf, dest_buf);

	/* Fill in src buffer */
	for (i = 0; i < size; i++)
		src_buf[i] = (u8)i;

	dev_dbg(dma->dev, "src: %pad dest:%pad\n", &src, &dest);
	tx = dma->device_prep_dma_memcpy(chan, dest, src, size, flags);
	if (!tx) {
		dev_err(dma->dev, "Self-test coherent failed, disabling\n");
		err = -ENODEV;
		goto prep_memcpy_failed;
	}

	tx->callback_param = &result;
	tx->callback = dma_selftest_complete;
	cookie = tx->tx_submit(tx);
	dma->device_issue_pending(chan);

	/*
	 * It is assumed that the hardware can move the data within 1s
	 * and signal the OS of the completion
	 */
	err = wait_event_timeout(result.wq,
				atomic_read(&result.counter) == 1,
				msecs_to_jiffies(10000));

	if (err <= 0) {
		dev_err(dma->dev, "Self-test copy timed out, disabling\n");
		err = -ENODEV;
		goto tx_status;
	}
	err = 0;
	dev_dbg(dma->dev, "Self-test complete signal received\n");

	if (dma->device_tx_status(chan, cookie, NULL) != DMA_COMPLETE) {
		dev_err(dma->dev,
			"Self-test copy timed out, disabling\n");
		err = -ENODEV;
		goto tx_status;
	}

	if (memcmp(src_buf, dest_buf, size)) {
		for (i = 0; i < size/4; i++) {
			if (((u32 *)src_buf)[i] != ((u32 *)(dest_buf))[i]) {
				dev_dbg(dma->dev,
					"[%d] src data:%x dest data:%x\n",
					i, ((u32 *)src_buf)[i],
					((u32 *)(dest_buf))[i]);
				break;
			}
		}
		dev_err(dma->dev,
			"Self-test copy failed compare, disabling\n");
		err = -EFAULT;
		goto compare_failed;
	}

	/*
	 * do not release the channel
	 * we want to consume all the channels on self test
	 */
	free_channel = false;

compare_failed:
tx_status:
prep_memcpy_failed:
	dma_free_coherent(dma->dev, size, dest_buf, dest);

dst_alloc_failed:
	dma_free_coherent(dma->dev, size, src_buf, src);

src_alloc_failed:
	if (free_channel)
		dma->device_free_chan_resources(chan);

	return err;
}

static int dma_selftest_all(struct dma_device *dma,
				bool req_coherent, bool req_sg)
{
	struct dma_chan **dmach_ptr;
	struct dma_chan *dmach;
	unsigned int i, j;
	u32 max_channels = 0;
	u64 sizes[] = {PAGE_SIZE - 1, PAGE_SIZE, PAGE_SIZE + 1, 2801, 13295};
	int count;
	u64 size;
	int rc;
	int failed = 0;

	list_for_each_entry(dmach, &dma->channels, device_node) {
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
		dev_dbg(dma->dev, "test start for size:%llx\n", size);
		list_for_each_entry(dmach, &dma->channels, device_node) {
			dmach_ptr[count] = dmach;
			if (req_coherent)
				rc = dma_selftest_one_coherent(dma, dmach,
					size,
					DMA_PREP_INTERRUPT | DMA_CTRL_ACK);
			else if (req_sg)
				rc = dma_selftest_sg(dma, dmach, size,
					DMA_PREP_INTERRUPT | DMA_CTRL_ACK);
			else
				rc = dma_selftest_streaming(dma, dmach, size,
					DMA_PREP_INTERRUPT | DMA_CTRL_ACK);
			if (rc) {
				failed = 1;
				break;
			}
			dev_dbg(dma->dev,
				"self test passed for ch:%d\n", count);
			count++;
		}

		/*
		 * free the channels where the test passed
		 * Channel resources are freed for a test that fails.
		 */
		for (i = 0; i < count; i++)
			dma->device_free_chan_resources(dmach_ptr[i]);

		if (failed)
			break;
	}

	rc = 0;

failed_exit:
	kfree(dmach_ptr);

	return rc;
}

static int dma_selftest_mapsngle(struct device *dev)
{
	u32 buf_size = 256;
	dma_addr_t dma_src;
	char *src;
	int ret;

	src = kmalloc(buf_size, GFP_KERNEL);
	if (!src)
		return -ENOMEM;

	strcpy(src, "hello world");

	dma_src = dma_map_single(dev, src, buf_size, DMA_TO_DEVICE);
	dev_dbg(dev, "mapsingle: src:%p src:%pad\n", src, &dma_src);

	ret = dma_mapping_error(dev, dma_src);
	if (ret)
		dev_err(dev, "dma_mapping_error with ret:%d\n", ret);
	else {
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
int dma_selftest_memcpy(struct dma_device *dma)
{
	int rc;

	dma_selftest_mapsngle(dma->dev);

	/* streaming test */
	rc = dma_selftest_all(dma, false, false);
	if (rc)
		return rc;
	dev_dbg(dma->dev, "streaming self test passed\n");

	/* coherent test */
	rc = dma_selftest_all(dma, true, false);
	if (rc)
		return rc;

	dev_dbg(dma->dev, "coherent self test passed\n");

	/* scatter gather test */
	rc = dma_selftest_all(dma, false, true);
	if (rc)
		return rc;

	dev_dbg(dma->dev, "scatter gather self test passed\n");
	return 0;
}
EXPORT_SYMBOL_GPL(dma_selftest_memcpy);
