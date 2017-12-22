/*
 * Virtio balloon implementation, inspired by Dor Laor and Marcelo
 * Tosatti's implementations.
 *
 *  Copyright 2008 Rusty Russell IBM Corporation
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include <linux/virtio.h>
#include <linux/virtio_balloon.h>
#include <linux/swap.h>
#include <linux/workqueue.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/balloon_compaction.h>
#include <linux/oom.h>
#include <linux/wait.h>
#include <linux/mm.h>
#include <linux/mount.h>
#include <linux/magic.h>
#include <linux/xbitmap.h>
#include <asm/page.h>

/*
 * Balloon device works in 4K page units.  So each page is pointed to by
 * multiple balloon pages.  All memory counters in this driver are in balloon
 * page units.
 */
#define VIRTIO_BALLOON_PAGES_PER_PAGE (unsigned)(PAGE_SIZE >> VIRTIO_BALLOON_PFN_SHIFT)
#define VIRTIO_BALLOON_ARRAY_PFNS_MAX 256
#define OOM_VBALLOON_DEFAULT_PAGES 256
#define VIRTBALLOON_OOM_NOTIFY_PRIORITY 80

static int oom_pages = OOM_VBALLOON_DEFAULT_PAGES;
module_param(oom_pages, int, S_IRUSR | S_IWUSR);
MODULE_PARM_DESC(oom_pages, "pages to free on OOM");

#ifdef CONFIG_BALLOON_COMPACTION
static struct vfsmount *balloon_mnt;
#endif

struct virtio_balloon {
	struct virtio_device *vdev;
	struct virtqueue *inflate_vq, *deflate_vq, *stats_vq, *free_page_vq;

	/* Balloon's own wq for cpu-intensive work items */
	struct workqueue_struct *balloon_wq;
	/* The free page reporting work item submitted to the balloon wq */
	struct work_struct report_free_page_work;

	/* The balloon servicing is delegated to a freezable workqueue. */
	struct work_struct update_balloon_stats_work;
	struct work_struct update_balloon_size_work;

	/* Prevent updating balloon when it is being canceled. */
	spinlock_t stop_update_lock;
	bool stop_update;

	/* Start to report free pages */
	bool report_free_page;
	/* Stores the cmd id given by host to start the free page reporting */
	uint32_t start_cmd_id;
	/* Stores STOP_ID as a sign to tell host that the reporting is done */
	uint32_t stop_cmd_id;

	/* Waiting for host to ack the pages we released. */
	wait_queue_head_t acked;

	/* Number of balloon pages we've told the Host we're not using. */
	unsigned int num_pages;
	/*
	 * The pages we've told the Host we're not using are enqueued
	 * at vb_dev_info->pages list.
	 * Each page on this list adds VIRTIO_BALLOON_PAGES_PER_PAGE
	 * to num_pages above.
	 */
	struct balloon_dev_info vb_dev_info;

	/* Synchronize access/update to this struct virtio_balloon elements */
	struct mutex balloon_lock;

	/* The xbitmap used to record balloon pages */
	struct xb page_xb;

	/* The array of pfns we tell the Host about. */
	unsigned int num_pfns;
	__virtio32 pfns[VIRTIO_BALLOON_ARRAY_PFNS_MAX];

	/* Memory statistics */
	struct virtio_balloon_stat stats[VIRTIO_BALLOON_S_NR];

	/* To register callback in oom notifier call chain */
	struct notifier_block nb;
};

static struct virtio_device_id id_table[] = {
	{ VIRTIO_ID_BALLOON, VIRTIO_DEV_ANY_ID },
	{ 0 },
};

static u32 page_to_balloon_pfn(struct page *page)
{
	unsigned long pfn = page_to_pfn(page);

	BUILD_BUG_ON(PAGE_SHIFT < VIRTIO_BALLOON_PFN_SHIFT);
	/* Convert pfn from Linux page size to balloon page size. */
	return pfn * VIRTIO_BALLOON_PAGES_PER_PAGE;
}

static void balloon_ack(struct virtqueue *vq)
{
	struct virtio_balloon *vb = vq->vdev->priv;

	wake_up(&vb->acked);
}

static void tell_host(struct virtio_balloon *vb, struct virtqueue *vq)
{
	struct scatterlist sg;
	unsigned int len;

	sg_init_one(&sg, vb->pfns, sizeof(vb->pfns[0]) * vb->num_pfns);

	/* We should always be able to add one buffer to an empty queue. */
	virtqueue_add_outbuf(vq, &sg, 1, vb, GFP_KERNEL);
	virtqueue_kick(vq);

	/* When host has read buffer, this completes via balloon_ack */
	wait_event(vb->acked, virtqueue_get_buf(vq, &len));

}

static void set_page_pfns(struct virtio_balloon *vb,
			  __virtio32 pfns[], struct page *page)
{
	unsigned int i;

	/*
	 * Set balloon pfns pointing at this page.
	 * Note that the first pfn points at start of the page.
	 */
	for (i = 0; i < VIRTIO_BALLOON_PAGES_PER_PAGE; i++)
		pfns[i] = cpu_to_virtio32(vb->vdev,
					  page_to_balloon_pfn(page) + i);
}

static void kick_and_wait(struct virtqueue *vq, wait_queue_head_t wq_head)
{
	unsigned int len;

	virtqueue_kick(vq);
	wait_event(wq_head, virtqueue_get_buf(vq, &len));
}

static void add_one_sg(struct virtqueue *vq, unsigned long pfn, uint32_t len)
{
	struct scatterlist sg;
	unsigned int unused;
	int err;

	sg_init_table(&sg, 1);
	sg_set_page(&sg, pfn_to_page(pfn), len, 0);

	/* Detach all the used buffers from the vq */
	while (virtqueue_get_buf(vq, &unused))
		;

	err = virtqueue_add_inbuf(vq, &sg, 1, vq, GFP_KERNEL);
	/*
	 * This is expected to never fail: there is always at least 1 entry
	 * available on the vq, because when the vq is full the worker thread
	 * that adds the sg will be put into sleep until at least 1 entry is
	 * available to use.
	 */
	BUG_ON(err);
}

static void batch_balloon_page_sg(struct virtio_balloon *vb,
				  struct virtqueue *vq,
				  unsigned long pfn,
				  uint32_t len)
{
	add_one_sg(vq, pfn, len);

	/* Batch till the vq is full */
	if (!vq->num_free)
		kick_and_wait(vq, vb->acked);
}

static void batch_free_page_sg(struct virtqueue *vq,
			       unsigned long pfn,
			       uint32_t len)
{
	add_one_sg(vq, pfn, len);

	/* Batch till the vq is full */
	if (!vq->num_free)
		virtqueue_kick(vq);
}

static void send_cmd_id(struct virtio_balloon *vb, void *addr)
{
	struct scatterlist sg;
	int err;

	sg_init_one(&sg, addr, sizeof(uint32_t));
	err = virtqueue_add_outbuf(vb->free_page_vq, &sg, 1, vb, GFP_KERNEL);
	BUG_ON(err);
	virtqueue_kick(vb->free_page_vq);
}

/*
 * Send balloon pages in sgs to host. The balloon pages are recorded in the
 * page xbitmap. Each bit in the bitmap corresponds to a page of PAGE_SIZE.
 * The page xbitmap is searched for continuous "1" bits, which correspond
 * to continuous pages, to chunk into sgs.
 *
 * @page_xb_start and @page_xb_end form the range of bits in the xbitmap that
 * need to be searched.
 */
static void tell_host_sgs(struct virtio_balloon *vb,
			  struct virtqueue *vq,
			  unsigned long page_xb_start,
			  unsigned long page_xb_end)
{
	unsigned long pfn_start, pfn_end;
	uint32_t max_len = round_down(UINT_MAX, PAGE_SIZE);
	uint64_t len;

	pfn_start = page_xb_start;
	while (pfn_start < page_xb_end) {
		pfn_start = xb_find_set(&vb->page_xb, page_xb_end + 1,
					pfn_start);
		if (pfn_start == page_xb_end + 1)
			break;
		pfn_end = xb_find_zero(&vb->page_xb, page_xb_end + 1,
				       pfn_start);
		len = (pfn_end - pfn_start) << PAGE_SHIFT;
		while (len > max_len) {
			batch_balloon_page_sg(vb, vq, pfn_start, max_len);
			pfn_start += max_len >> PAGE_SHIFT;
			len -= max_len;
		}
		batch_balloon_page_sg(vb, vq, pfn_start, (uint32_t)len);
		pfn_start = pfn_end + 1;
	}

	/*
	 * The last few sgs may not reach the batch size, but need a kick to
	 * notify the device to handle them.
	 */
	if (vq->num_free != virtqueue_get_vring_size(vq))
		kick_and_wait(vq, vb->acked);

	xb_clear_bit_range(&vb->page_xb, page_xb_start, page_xb_end);
}

static inline int xb_set_page(struct virtio_balloon *vb,
			       struct page *page,
			       unsigned long *pfn_min,
			       unsigned long *pfn_max)
{
	unsigned long pfn = page_to_pfn(page);
	int ret;

	*pfn_min = min(pfn, *pfn_min);
	*pfn_max = max(pfn, *pfn_max);

	do {
		if (xb_preload(GFP_NOWAIT | __GFP_NOWARN) < 0)
			return -ENOMEM;

		ret = xb_set_bit(&vb->page_xb, pfn);
		xb_preload_end();
	} while (unlikely(ret == -EAGAIN));

	return ret;
}

static unsigned fill_balloon(struct virtio_balloon *vb, size_t num)
{
	unsigned num_allocated_pages;
	unsigned num_pfns;
	struct page *page;
	LIST_HEAD(pages);
	bool use_sg = virtio_has_feature(vb->vdev, VIRTIO_BALLOON_F_SG);
	unsigned long pfn_max = 0, pfn_min = ULONG_MAX;

	/* We can only do one array worth at a time. */
	if (!use_sg)
		num = min(num, ARRAY_SIZE(vb->pfns));

	for (num_pfns = 0; num_pfns < num;
	     num_pfns += VIRTIO_BALLOON_PAGES_PER_PAGE) {
		struct page *page = balloon_page_alloc();

		if (!page) {
			dev_info_ratelimited(&vb->vdev->dev,
					     "Out of puff! Can't get %u pages\n",
					     VIRTIO_BALLOON_PAGES_PER_PAGE);
			/* Sleep for at least 1/5 of a second before retry. */
			msleep(200);
			break;
		}

		balloon_page_push(&pages, page);
	}

	mutex_lock(&vb->balloon_lock);

	vb->num_pfns = 0;

	while ((page = balloon_page_pop(&pages))) {
		balloon_page_enqueue(&vb->vb_dev_info, page);
		if (use_sg) {
			if (xb_set_page(vb, page, &pfn_min, &pfn_max) < 0) {
				__free_page(page);
				continue;
			}
		} else {
			set_page_pfns(vb, vb->pfns + vb->num_pfns, page);
		}

		vb->num_pages += VIRTIO_BALLOON_PAGES_PER_PAGE;
		if (!virtio_has_feature(vb->vdev,
					VIRTIO_BALLOON_F_DEFLATE_ON_OOM))
			adjust_managed_page_count(page, -1);
		vb->num_pfns += VIRTIO_BALLOON_PAGES_PER_PAGE;
	}

	num_allocated_pages = vb->num_pfns;
	/* Did we get any? */
	if (vb->num_pfns) {
		if (use_sg)
			tell_host_sgs(vb, vb->inflate_vq, pfn_min, pfn_max);
		else
			tell_host(vb, vb->inflate_vq);
	}
	mutex_unlock(&vb->balloon_lock);

	return num_allocated_pages;
}

static void release_pages_balloon(struct virtio_balloon *vb,
				 struct list_head *pages)
{
	struct page *page, *next;

	list_for_each_entry_safe(page, next, pages, lru) {
		if (!virtio_has_feature(vb->vdev,
					VIRTIO_BALLOON_F_DEFLATE_ON_OOM))
			adjust_managed_page_count(page, 1);
		list_del(&page->lru);
		put_page(page); /* balloon reference */
	}
}

static unsigned leak_balloon(struct virtio_balloon *vb, size_t num)
{
	unsigned num_freed_pages;
	struct page *page;
	struct balloon_dev_info *vb_dev_info = &vb->vb_dev_info;
	LIST_HEAD(pages);
	bool use_sg = virtio_has_feature(vb->vdev, VIRTIO_BALLOON_F_SG);
	unsigned long pfn_max = 0, pfn_min = ULONG_MAX;

	/* Traditionally, we can only do one array worth at a time. */
	if (!use_sg)
		num = min(num, ARRAY_SIZE(vb->pfns));

	mutex_lock(&vb->balloon_lock);
	/* We can't release more pages than taken */
	num = min(num, (size_t)vb->num_pages);
	for (vb->num_pfns = 0; vb->num_pfns < num;
	     vb->num_pfns += VIRTIO_BALLOON_PAGES_PER_PAGE) {
		page = balloon_page_dequeue(vb_dev_info);
		if (!page)
			break;
		if (use_sg) {
			if (xb_set_page(vb, page, &pfn_min, &pfn_max) < 0) {
				balloon_page_enqueue(&vb->vb_dev_info, page);
				break;
			}
		} else {
			set_page_pfns(vb, vb->pfns + vb->num_pfns, page);
		}
		list_add(&page->lru, &pages);
		vb->num_pages -= VIRTIO_BALLOON_PAGES_PER_PAGE;
	}

	num_freed_pages = vb->num_pfns;
	/*
	 * Note that if
	 * virtio_has_feature(vdev, VIRTIO_BALLOON_F_MUST_TELL_HOST);
	 * is true, we *have* to do it in this order
	 */
	if (vb->num_pfns) {
		if (use_sg)
			tell_host_sgs(vb, vb->deflate_vq, pfn_min, pfn_max);
		else
			tell_host(vb, vb->deflate_vq);
	}
	release_pages_balloon(vb, &pages);
	mutex_unlock(&vb->balloon_lock);
	return num_freed_pages;
}

/*
 * The regular leak_balloon() with VIRTIO_BALLOON_F_SG needs memory allocation
 * for xbitmap, which is not suitable for the oom case. This function does not
 * use xbitmap to chunk pages, so it can be used by oom notifier to deflate
 * pages when VIRTIO_BALLOON_F_SG is negotiated.
 */
static unsigned int leak_balloon_sg_oom(struct virtio_balloon *vb)
{
	unsigned int n;
	struct page *page;
	struct balloon_dev_info *vb_dev_info = &vb->vb_dev_info;
	struct virtqueue *vq = vb->deflate_vq;
	LIST_HEAD(pages);

	mutex_lock(&vb->balloon_lock);
	for (n = 0; n < oom_pages; n++) {
		page = balloon_page_dequeue(vb_dev_info);
		if (!page)
			break;

		list_add(&page->lru, &pages);
		vb->num_pages -= VIRTIO_BALLOON_PAGES_PER_PAGE;
		batch_balloon_page_sg(vb, vb->deflate_vq, page_to_pfn(page),
				      PAGE_SIZE);
		release_pages_balloon(vb, &pages);
	}

	/*
	 * The last few sgs may not reach the batch size, but need a kick to
	 * notify the device to handle them.
	 */
	if (vq->num_free != virtqueue_get_vring_size(vq))
		kick_and_wait(vq, vb->acked);
	mutex_unlock(&vb->balloon_lock);

	return n;
}

static inline void update_stat(struct virtio_balloon *vb, int idx,
			       u16 tag, u64 val)
{
	BUG_ON(idx >= VIRTIO_BALLOON_S_NR);
	vb->stats[idx].tag = cpu_to_virtio16(vb->vdev, tag);
	vb->stats[idx].val = cpu_to_virtio64(vb->vdev, val);
}

#define pages_to_bytes(x) ((u64)(x) << PAGE_SHIFT)

static unsigned int update_balloon_stats(struct virtio_balloon *vb)
{
	unsigned long events[NR_VM_EVENT_ITEMS];
	struct sysinfo i;
	unsigned int idx = 0;
	long available;

	all_vm_events(events);
	si_meminfo(&i);

	available = si_mem_available();

#ifdef CONFIG_VM_EVENT_COUNTERS
	update_stat(vb, idx++, VIRTIO_BALLOON_S_SWAP_IN,
				pages_to_bytes(events[PSWPIN]));
	update_stat(vb, idx++, VIRTIO_BALLOON_S_SWAP_OUT,
				pages_to_bytes(events[PSWPOUT]));
	update_stat(vb, idx++, VIRTIO_BALLOON_S_MAJFLT, events[PGMAJFAULT]);
	update_stat(vb, idx++, VIRTIO_BALLOON_S_MINFLT, events[PGFAULT]);
#endif
	update_stat(vb, idx++, VIRTIO_BALLOON_S_MEMFREE,
				pages_to_bytes(i.freeram));
	update_stat(vb, idx++, VIRTIO_BALLOON_S_MEMTOT,
				pages_to_bytes(i.totalram));
	update_stat(vb, idx++, VIRTIO_BALLOON_S_AVAIL,
				pages_to_bytes(available));

	return idx;
}

/*
 * While most virtqueues communicate guest-initiated requests to the hypervisor,
 * the stats queue operates in reverse.  The driver initializes the virtqueue
 * with a single buffer.  From that point forward, all conversations consist of
 * a hypervisor request (a call to this function) which directs us to refill
 * the virtqueue with a fresh stats buffer.  Since stats collection can sleep,
 * we delegate the job to a freezable workqueue that will do the actual work via
 * stats_handle_request().
 */
static void stats_request(struct virtqueue *vq)
{
	struct virtio_balloon *vb = vq->vdev->priv;

	spin_lock(&vb->stop_update_lock);
	if (!vb->stop_update)
		queue_work(system_freezable_wq, &vb->update_balloon_stats_work);
	spin_unlock(&vb->stop_update_lock);
}

static void stats_handle_request(struct virtio_balloon *vb)
{
	struct virtqueue *vq;
	struct scatterlist sg;
	unsigned int len, num_stats;

	num_stats = update_balloon_stats(vb);

	vq = vb->stats_vq;
	if (!virtqueue_get_buf(vq, &len))
		return;
	sg_init_one(&sg, vb->stats, sizeof(vb->stats[0]) * num_stats);
	virtqueue_add_outbuf(vq, &sg, 1, vb, GFP_KERNEL);
	virtqueue_kick(vq);
}

static inline s64 towards_target(struct virtio_balloon *vb)
{
	s64 target;
	u32 num_pages;

	virtio_cread(vb->vdev, struct virtio_balloon_config, num_pages,
		     &num_pages);

	/* Legacy balloon config space is LE, unlike all other devices. */
	if (!virtio_has_feature(vb->vdev, VIRTIO_F_VERSION_1))
		num_pages = le32_to_cpu((__force __le32)num_pages);

	target = num_pages;
	return target - vb->num_pages;
}

static void virtballoon_changed(struct virtio_device *vdev)
{
	struct virtio_balloon *vb = vdev->priv;
	unsigned long flags;
	__u32 cmd_id;
	s64 diff = towards_target(vb);

	if (diff) {
		spin_lock_irqsave(&vb->stop_update_lock, flags);
		if (!vb->stop_update)
			queue_work(system_freezable_wq,
				   &vb->update_balloon_size_work);
		spin_unlock_irqrestore(&vb->stop_update_lock, flags);
	}

	virtio_cread(vb->vdev, struct virtio_balloon_config,
		     free_page_report_cmd_id, &cmd_id);
	if (cmd_id == VIRTIO_BALLOON_FREE_PAGE_REPORT_STOP_ID) {
		WRITE_ONCE(vb->report_free_page, false);
	} else if (cmd_id != vb->start_cmd_id) {
		/*
		 * Host requests to start the reporting by sending a new cmd
		 * id.
		 */
		WRITE_ONCE(vb->report_free_page, true);
		vb->start_cmd_id = cmd_id;
		queue_work(vb->balloon_wq, &vb->report_free_page_work);
	}
}

static void update_balloon_size(struct virtio_balloon *vb)
{
	u32 actual = vb->num_pages;

	/* Legacy balloon config space is LE, unlike all other devices. */
	if (!virtio_has_feature(vb->vdev, VIRTIO_F_VERSION_1))
		actual = (__force u32)cpu_to_le32(actual);

	virtio_cwrite(vb->vdev, struct virtio_balloon_config, actual,
		      &actual);
}

/*
 * virtballoon_oom_notify - release pages when system is under severe
 *			    memory pressure (called from out_of_memory())
 * @self : notifier block struct
 * @dummy: not used
 * @parm : returned - number of freed pages
 *
 * The balancing of memory by use of the virtio balloon should not cause
 * the termination of processes while there are pages in the balloon.
 * If virtio balloon manages to release some memory, it will make the
 * system return and retry the allocation that forced the OOM killer
 * to run.
 */
static int virtballoon_oom_notify(struct notifier_block *self,
				  unsigned long dummy, void *parm)
{
	struct virtio_balloon *vb;
	unsigned long *freed;
	unsigned num_freed_pages;

	vb = container_of(self, struct virtio_balloon, nb);
	if (!virtio_has_feature(vb->vdev, VIRTIO_BALLOON_F_DEFLATE_ON_OOM))
		return NOTIFY_OK;

	freed = parm;
	if (virtio_has_feature(vb->vdev, VIRTIO_BALLOON_F_SG))
		num_freed_pages = leak_balloon_sg_oom(vb);
	else
		num_freed_pages = leak_balloon(vb, oom_pages);
	update_balloon_size(vb);
	*freed += num_freed_pages;

	return NOTIFY_OK;
}

static void update_balloon_stats_func(struct work_struct *work)
{
	struct virtio_balloon *vb;

	vb = container_of(work, struct virtio_balloon,
			  update_balloon_stats_work);
	stats_handle_request(vb);
}

static void update_balloon_size_func(struct work_struct *work)
{
	struct virtio_balloon *vb;
	s64 diff;

	vb = container_of(work, struct virtio_balloon,
			  update_balloon_size_work);
	diff = towards_target(vb);

	if (diff > 0)
		diff -= fill_balloon(vb, diff);
	else if (diff < 0)
		diff += leak_balloon(vb, -diff);
	update_balloon_size(vb);

	if (diff)
		queue_work(system_freezable_wq, work);
}

static int init_vqs(struct virtio_balloon *vb)
{
	struct virtqueue **vqs;
	vq_callback_t **callbacks;
	const char **names;
	struct scatterlist sg;
	int i, nvqs, err = -ENOMEM;

	/* Inflateq and deflateq are used unconditionally */
	nvqs = 2;
	if (virtio_has_feature(vb->vdev, VIRTIO_BALLOON_F_STATS_VQ))
		nvqs++;
	if (virtio_has_feature(vb->vdev, VIRTIO_BALLOON_F_FREE_PAGE_VQ))
		nvqs++;

	/* Allocate space for find_vqs parameters */
	vqs = kcalloc(nvqs, sizeof(*vqs), GFP_KERNEL);
	if (!vqs)
		goto err_vq;
	callbacks = kmalloc_array(nvqs, sizeof(*callbacks), GFP_KERNEL);
	if (!callbacks)
		goto err_callback;
	names = kmalloc_array(nvqs, sizeof(*names), GFP_KERNEL);
	if (!names)
		goto err_names;

	callbacks[0] = balloon_ack;
	names[0] = "inflate";
	callbacks[1] = balloon_ack;
	names[1] = "deflate";

	i = 2;
	if (virtio_has_feature(vb->vdev, VIRTIO_BALLOON_F_STATS_VQ)) {
		callbacks[i] = stats_request;
		names[i] = "stats";
		i++;
	}

	if (virtio_has_feature(vb->vdev, VIRTIO_BALLOON_F_FREE_PAGE_VQ)) {
		callbacks[i] = NULL;
		names[i] = "free_page_vq";
	}

	err = vb->vdev->config->find_vqs(vb->vdev, nvqs, vqs, callbacks, names,
					 NULL, NULL);
	if (err)
		goto err_find;

	vb->inflate_vq = vqs[0];
	vb->deflate_vq = vqs[1];
	i = 2;
	if (virtio_has_feature(vb->vdev, VIRTIO_BALLOON_F_STATS_VQ)) {
		vb->stats_vq = vqs[i++];
		/*
		 * Prime this virtqueue with one buffer so the hypervisor can
		 * use it to signal us later (it can't be broken yet!).
		 */
		sg_init_one(&sg, vb->stats, sizeof(vb->stats));
		if (virtqueue_add_outbuf(vb->stats_vq, &sg, 1, vb, GFP_KERNEL)
		    < 0) {
			dev_warn(&vb->vdev->dev, "%s: add stat_vq failed\n",
				 __func__);
			goto err_find;
		}
		virtqueue_kick(vb->stats_vq);
	}

	if (virtio_has_feature(vb->vdev, VIRTIO_BALLOON_F_FREE_PAGE_VQ))
		vb->free_page_vq = vqs[i];

	kfree(names);
	kfree(callbacks);
	kfree(vqs);
	return 0;

err_find:
	kfree(names);
err_names:
	kfree(callbacks);
err_callback:
	kfree(vqs);
err_vq:
	return err;
}

static bool virtio_balloon_send_free_pages(void *opaque, unsigned long pfn,
					   unsigned long nr_pages)
{
	struct virtio_balloon *vb = (struct virtio_balloon *)opaque;
	uint32_t len = nr_pages << PAGE_SHIFT;

	if (!READ_ONCE(vb->report_free_page))
		return false;

	batch_free_page_sg(vb->free_page_vq, pfn, len);

	return true;
}

static void report_free_page(struct work_struct *work)
{
	struct virtio_balloon *vb;

	vb = container_of(work, struct virtio_balloon, report_free_page_work);
	/* Start by sending the obtained cmd id to the host with an outbuf */
	send_cmd_id(vb, &vb->start_cmd_id);
	walk_free_mem_block(vb, 0, &virtio_balloon_send_free_pages);
	/*
	 * End by sending the stop id to the host with an outbuf. Use the
	 * non-batching mode here to trigger a kick after adding the stop id.
	 */
	send_cmd_id(vb, &vb->stop_cmd_id);
}

#ifdef CONFIG_BALLOON_COMPACTION
/*
 * virtballoon_migratepage - perform the balloon page migration on behalf of
 *			     a compation thread.     (called under page lock)
 * @vb_dev_info: the balloon device
 * @newpage: page that will replace the isolated page after migration finishes.
 * @page   : the isolated (old) page that is about to be migrated to newpage.
 * @mode   : compaction mode -- not used for balloon page migration.
 *
 * After a ballooned page gets isolated by compaction procedures, this is the
 * function that performs the page migration on behalf of a compaction thread
 * The page migration for virtio balloon is done in a simple swap fashion which
 * follows these two macro steps:
 *  1) insert newpage into vb->pages list and update the host about it;
 *  2) update the host about the old page removed from vb->pages list;
 *
 * This function preforms the balloon page migration task.
 * Called through balloon_mapping->a_ops->migratepage
 */
static int virtballoon_migratepage(struct balloon_dev_info *vb_dev_info,
		struct page *newpage, struct page *page, enum migrate_mode mode)
{
	struct virtio_balloon *vb = container_of(vb_dev_info,
			struct virtio_balloon, vb_dev_info);
	bool use_sg = virtio_has_feature(vb->vdev, VIRTIO_BALLOON_F_SG);
	unsigned long flags;

	/*
	 * In order to avoid lock contention while migrating pages concurrently
	 * to leak_balloon() or fill_balloon() we just give up the balloon_lock
	 * this turn, as it is easier to retry the page migration later.
	 * This also prevents fill_balloon() getting stuck into a mutex
	 * recursion in the case it ends up triggering memory compaction
	 * while it is attempting to inflate the ballon.
	 */
	if (!mutex_trylock(&vb->balloon_lock))
		return -EAGAIN;

	get_page(newpage); /* balloon reference */

	/* balloon's page migration 1st step  -- inflate "newpage" */
	spin_lock_irqsave(&vb_dev_info->pages_lock, flags);
	balloon_page_insert(vb_dev_info, newpage);
	vb_dev_info->isolated_pages--;
	__count_vm_event(BALLOON_MIGRATE);
	spin_unlock_irqrestore(&vb_dev_info->pages_lock, flags);
	if (use_sg) {
		add_one_sg(vb->inflate_vq, page_to_pfn(newpage), PAGE_SIZE);
		kick_and_wait(vb->inflate_vq, vb->acked);
	} else {
		vb->num_pfns = VIRTIO_BALLOON_PAGES_PER_PAGE;
		set_page_pfns(vb, vb->pfns, newpage);
		tell_host(vb, vb->inflate_vq);
	}
	/* balloon's page migration 2nd step -- deflate "page" */
	balloon_page_delete(page);
	if (use_sg) {
		add_one_sg(vb->deflate_vq, page_to_pfn(page), PAGE_SIZE);
		kick_and_wait(vb->deflate_vq, vb->acked);
	} else {
		vb->num_pfns = VIRTIO_BALLOON_PAGES_PER_PAGE;
		set_page_pfns(vb, vb->pfns, page);
		tell_host(vb, vb->deflate_vq);
	}
	mutex_unlock(&vb->balloon_lock);

	put_page(page); /* balloon reference */

	return MIGRATEPAGE_SUCCESS;
}

static struct dentry *balloon_mount(struct file_system_type *fs_type,
		int flags, const char *dev_name, void *data)
{
	static const struct dentry_operations ops = {
		.d_dname = simple_dname,
	};

	return mount_pseudo(fs_type, "balloon-kvm:", NULL, &ops,
				BALLOON_KVM_MAGIC);
}

static struct file_system_type balloon_fs = {
	.name           = "balloon-kvm",
	.mount          = balloon_mount,
	.kill_sb        = kill_anon_super,
};

#endif /* CONFIG_BALLOON_COMPACTION */

static int virtballoon_probe(struct virtio_device *vdev)
{
	struct virtio_balloon *vb;
	__u32 poison_val;
	int err;

	if (!vdev->config->get) {
		dev_err(&vdev->dev, "%s failure: config access disabled\n",
			__func__);
		return -EINVAL;
	}

	vdev->priv = vb = kmalloc(sizeof(*vb), GFP_KERNEL);
	if (!vb) {
		err = -ENOMEM;
		goto out;
	}

	INIT_WORK(&vb->update_balloon_stats_work, update_balloon_stats_func);
	INIT_WORK(&vb->update_balloon_size_work, update_balloon_size_func);
	spin_lock_init(&vb->stop_update_lock);
	vb->stop_update = false;
	vb->num_pages = 0;
	mutex_init(&vb->balloon_lock);
	init_waitqueue_head(&vb->acked);
	vb->vdev = vdev;

	balloon_devinfo_init(&vb->vb_dev_info);

	err = init_vqs(vb);
	if (err)
		goto out_free_vb;

	if (virtio_has_feature(vdev, VIRTIO_BALLOON_F_SG))
		xb_init(&vb->page_xb);

	if (virtio_has_feature(vdev, VIRTIO_BALLOON_F_FREE_PAGE_VQ)) {
		vb->balloon_wq = alloc_workqueue("balloon-wq",
					WQ_FREEZABLE | WQ_CPU_INTENSIVE, 0);
		INIT_WORK(&vb->report_free_page_work, report_free_page);
		vb->stop_cmd_id = VIRTIO_BALLOON_FREE_PAGE_REPORT_STOP_ID;
		if (IS_ENABLED(CONFIG_PAGE_POISONING_NO_SANITY) ||
		    !page_poisoning_enabled())
			poison_val = 0;
		else
			poison_val = PAGE_POISON;
		virtio_cwrite(vb->vdev, struct virtio_balloon_config,
			      poison_val, &poison_val);
	}

	vb->nb.notifier_call = virtballoon_oom_notify;
	vb->nb.priority = VIRTBALLOON_OOM_NOTIFY_PRIORITY;
	err = register_oom_notifier(&vb->nb);
	if (err < 0)
		goto out_del_vqs;

#ifdef CONFIG_BALLOON_COMPACTION
	balloon_mnt = kern_mount(&balloon_fs);
	if (IS_ERR(balloon_mnt)) {
		err = PTR_ERR(balloon_mnt);
		unregister_oom_notifier(&vb->nb);
		goto out_del_vqs;
	}

	vb->vb_dev_info.migratepage = virtballoon_migratepage;
	vb->vb_dev_info.inode = alloc_anon_inode(balloon_mnt->mnt_sb);
	if (IS_ERR(vb->vb_dev_info.inode)) {
		err = PTR_ERR(vb->vb_dev_info.inode);
		kern_unmount(balloon_mnt);
		unregister_oom_notifier(&vb->nb);
		vb->vb_dev_info.inode = NULL;
		goto out_del_vqs;
	}
	vb->vb_dev_info.inode->i_mapping->a_ops = &balloon_aops;
#endif

	virtio_device_ready(vdev);

	if (towards_target(vb))
		virtballoon_changed(vdev);
	return 0;

out_del_vqs:
	vdev->config->del_vqs(vdev);
out_free_vb:
	kfree(vb);
out:
	return err;
}

static void remove_common(struct virtio_balloon *vb)
{
	/* There might be pages left in the balloon: free them. */
	while (vb->num_pages)
		leak_balloon(vb, vb->num_pages);
	update_balloon_size(vb);

	/* Now we reset the device so we can clean up the queues. */
	vb->vdev->config->reset(vb->vdev);

	vb->vdev->config->del_vqs(vb->vdev);
}

static void virtballoon_remove(struct virtio_device *vdev)
{
	struct virtio_balloon *vb = vdev->priv;

	unregister_oom_notifier(&vb->nb);

	spin_lock_irq(&vb->stop_update_lock);
	vb->stop_update = true;
	spin_unlock_irq(&vb->stop_update_lock);
	cancel_work_sync(&vb->update_balloon_size_work);
	cancel_work_sync(&vb->update_balloon_stats_work);
	cancel_work_sync(&vb->report_free_page_work);

	remove_common(vb);
#ifdef CONFIG_BALLOON_COMPACTION
	if (vb->vb_dev_info.inode)
		iput(vb->vb_dev_info.inode);

	kern_unmount(balloon_mnt);
#endif
	kfree(vb);
}

#ifdef CONFIG_PM_SLEEP
static int virtballoon_freeze(struct virtio_device *vdev)
{
	struct virtio_balloon *vb = vdev->priv;

	/*
	 * The workqueue is already frozen by the PM core before this
	 * function is called.
	 */
	remove_common(vb);
	return 0;
}

static int virtballoon_restore(struct virtio_device *vdev)
{
	struct virtio_balloon *vb = vdev->priv;
	int ret;

	ret = init_vqs(vdev->priv);
	if (ret)
		return ret;

	virtio_device_ready(vdev);

	if (towards_target(vb))
		virtballoon_changed(vdev);
	update_balloon_size(vb);
	return 0;
}
#endif

static int virtballoon_validate(struct virtio_device *vdev)
{
	__virtio_clear_bit(vdev, VIRTIO_F_IOMMU_PLATFORM);
	return 0;
}

static unsigned int features[] = {
	VIRTIO_BALLOON_F_MUST_TELL_HOST,
	VIRTIO_BALLOON_F_STATS_VQ,
	VIRTIO_BALLOON_F_DEFLATE_ON_OOM,
	VIRTIO_BALLOON_F_SG,
	VIRTIO_BALLOON_F_FREE_PAGE_VQ,
};

static struct virtio_driver virtio_balloon_driver = {
	.feature_table = features,
	.feature_table_size = ARRAY_SIZE(features),
	.driver.name =	KBUILD_MODNAME,
	.driver.owner =	THIS_MODULE,
	.id_table =	id_table,
	.validate =	virtballoon_validate,
	.probe =	virtballoon_probe,
	.remove =	virtballoon_remove,
	.config_changed = virtballoon_changed,
#ifdef CONFIG_PM_SLEEP
	.freeze	=	virtballoon_freeze,
	.restore =	virtballoon_restore,
#endif
};

module_virtio_driver(virtio_balloon_driver);
MODULE_DEVICE_TABLE(virtio, id_table);
MODULE_DESCRIPTION("Virtio balloon driver");
MODULE_LICENSE("GPL");
