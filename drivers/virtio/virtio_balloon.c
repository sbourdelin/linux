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

/*
 * Balloon device works in 4K page units.  So each page is pointed to by
 * multiple balloon pages.  All memory counters in this driver are in balloon
 * page units.
 */
#define VIRTIO_BALLOON_PAGES_PER_PAGE (unsigned)(PAGE_SIZE >> VIRTIO_BALLOON_PFN_SHIFT)
#define VIRTIO_BALLOON_ARRAY_PFNS_MAX 256
#define OOM_VBALLOON_DEFAULT_PAGES 256
#define VIRTBALLOON_OOM_NOTIFY_PRIORITY 80

/* The order used to allocate a buffer to load free page hints */
#define VIRTIO_BALLOON_HINT_BUF_ORDER (MAX_ORDER - 1)
/* The number of pages a hint buffer has */
#define VIRTIO_BALLOON_HINT_BUF_PAGES (1 << VIRTIO_BALLOON_HINT_BUF_ORDER)
/* The size of a hint buffer in bytes */
#define VIRTIO_BALLOON_HINT_BUF_SIZE (VIRTIO_BALLOON_HINT_BUF_PAGES << \
				      PAGE_SHIFT)

static int oom_pages = OOM_VBALLOON_DEFAULT_PAGES;
module_param(oom_pages, int, S_IRUSR | S_IWUSR);
MODULE_PARM_DESC(oom_pages, "pages to free on OOM");

#ifdef CONFIG_BALLOON_COMPACTION
static struct vfsmount *balloon_mnt;
#endif

enum virtio_balloon_vq {
	VIRTIO_BALLOON_VQ_INFLATE,
	VIRTIO_BALLOON_VQ_DEFLATE,
	VIRTIO_BALLOON_VQ_STATS,
	VIRTIO_BALLOON_VQ_FREE_PAGE,
	VIRTIO_BALLOON_VQ_MAX
};

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

	/* Command buffers to start and stop the reporting of hints to host */
	struct virtio_balloon_free_page_hints_cmd cmd_start;
	struct virtio_balloon_free_page_hints_cmd cmd_stop;

	/* The cmd id received from host */
	u32 cmd_id_received;
	/* The cmd id that is actively in use */
	u32 cmd_id_active;

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

static unsigned fill_balloon(struct virtio_balloon *vb, size_t num)
{
	unsigned num_allocated_pages;
	unsigned num_pfns;
	struct page *page;
	LIST_HEAD(pages);

	/* We can only do one array worth at a time. */
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

		set_page_pfns(vb, vb->pfns + vb->num_pfns, page);
		vb->num_pages += VIRTIO_BALLOON_PAGES_PER_PAGE;
		if (!virtio_has_feature(vb->vdev,
					VIRTIO_BALLOON_F_DEFLATE_ON_OOM))
			adjust_managed_page_count(page, -1);
		vb->num_pfns += VIRTIO_BALLOON_PAGES_PER_PAGE;
	}

	num_allocated_pages = vb->num_pfns;
	/* Did we get any? */
	if (vb->num_pfns != 0)
		tell_host(vb, vb->inflate_vq);
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

	/* We can only do one array worth at a time. */
	num = min(num, ARRAY_SIZE(vb->pfns));

	mutex_lock(&vb->balloon_lock);
	/* We can't release more pages than taken */
	num = min(num, (size_t)vb->num_pages);
	for (vb->num_pfns = 0; vb->num_pfns < num;
	     vb->num_pfns += VIRTIO_BALLOON_PAGES_PER_PAGE) {
		page = balloon_page_dequeue(vb_dev_info);
		if (!page)
			break;
		set_page_pfns(vb, vb->pfns + vb->num_pfns, page);
		list_add(&page->lru, &pages);
		vb->num_pages -= VIRTIO_BALLOON_PAGES_PER_PAGE;
	}

	num_freed_pages = vb->num_pfns;
	/*
	 * Note that if
	 * virtio_has_feature(vdev, VIRTIO_BALLOON_F_MUST_TELL_HOST);
	 * is true, we *have* to do it in this order
	 */
	if (vb->num_pfns != 0)
		tell_host(vb, vb->deflate_vq);
	release_pages_balloon(vb, &pages);
	mutex_unlock(&vb->balloon_lock);
	return num_freed_pages;
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
	unsigned long caches;

	all_vm_events(events);
	si_meminfo(&i);

	available = si_mem_available();
	caches = global_node_page_state(NR_FILE_PAGES);

#ifdef CONFIG_VM_EVENT_COUNTERS
	update_stat(vb, idx++, VIRTIO_BALLOON_S_SWAP_IN,
				pages_to_bytes(events[PSWPIN]));
	update_stat(vb, idx++, VIRTIO_BALLOON_S_SWAP_OUT,
				pages_to_bytes(events[PSWPOUT]));
	update_stat(vb, idx++, VIRTIO_BALLOON_S_MAJFLT, events[PGMAJFAULT]);
	update_stat(vb, idx++, VIRTIO_BALLOON_S_MINFLT, events[PGFAULT]);
#ifdef CONFIG_HUGETLB_PAGE
	update_stat(vb, idx++, VIRTIO_BALLOON_S_HTLB_PGALLOC,
		    events[HTLB_BUDDY_PGALLOC]);
	update_stat(vb, idx++, VIRTIO_BALLOON_S_HTLB_PGFAIL,
		    events[HTLB_BUDDY_PGALLOC_FAIL]);
#endif
#endif
	update_stat(vb, idx++, VIRTIO_BALLOON_S_MEMFREE,
				pages_to_bytes(i.freeram));
	update_stat(vb, idx++, VIRTIO_BALLOON_S_MEMTOT,
				pages_to_bytes(i.totalram));
	update_stat(vb, idx++, VIRTIO_BALLOON_S_AVAIL,
				pages_to_bytes(available));
	update_stat(vb, idx++, VIRTIO_BALLOON_S_CACHES,
				pages_to_bytes(caches));

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
	s64 diff = towards_target(vb);

	if (diff) {
		spin_lock_irqsave(&vb->stop_update_lock, flags);
		if (!vb->stop_update)
			queue_work(system_freezable_wq,
				   &vb->update_balloon_size_work);
		spin_unlock_irqrestore(&vb->stop_update_lock, flags);
	}

	if (virtio_has_feature(vdev, VIRTIO_BALLOON_F_FREE_PAGE_HINT)) {
		virtio_cread(vdev, struct virtio_balloon_config,
			     free_page_report_cmd_id, &vb->cmd_id_received);
		if (vb->cmd_id_received !=
		    VIRTIO_BALLOON_FREE_PAGE_REPORT_STOP_ID &&
		    vb->cmd_id_received != vb->cmd_id_active) {
			spin_lock_irqsave(&vb->stop_update_lock, flags);
			if (!vb->stop_update)
				queue_work(vb->balloon_wq,
					   &vb->report_free_page_work);
			spin_unlock_irqrestore(&vb->stop_update_lock, flags);
		}
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

static void virtio_balloon_free_used_hint_buf(struct virtqueue *vq)
{
	unsigned int len;
	void *buf;
	struct virtio_balloon *vb = vq->vdev->priv;

	do {
		buf = virtqueue_get_buf(vq, &len);
		if (buf == &vb->cmd_start || buf == &vb->cmd_stop)
			continue;
		free_pages((unsigned long)buf, VIRTIO_BALLOON_HINT_BUF_ORDER);
	} while (buf);
}

static int init_vqs(struct virtio_balloon *vb)
{
	struct virtqueue *vqs[VIRTIO_BALLOON_VQ_MAX];
	vq_callback_t *callbacks[VIRTIO_BALLOON_VQ_MAX];
	const char *names[VIRTIO_BALLOON_VQ_MAX];
	int err;

	/*
	 * Inflateq and deflateq are used unconditionally. The names[]
	 * will be NULL if the related feature is not enabled, which will
	 * cause no allocation for the corresponding virtqueue in find_vqs.
	 */
	callbacks[VIRTIO_BALLOON_VQ_INFLATE] = balloon_ack;
	names[VIRTIO_BALLOON_VQ_INFLATE] = "inflate";
	callbacks[VIRTIO_BALLOON_VQ_DEFLATE] = balloon_ack;
	names[VIRTIO_BALLOON_VQ_DEFLATE] = "deflate";
	names[VIRTIO_BALLOON_VQ_STATS] = NULL;
	names[VIRTIO_BALLOON_VQ_FREE_PAGE] = NULL;

	if (virtio_has_feature(vb->vdev, VIRTIO_BALLOON_F_STATS_VQ)) {
		names[VIRTIO_BALLOON_VQ_STATS] = "stats";
		callbacks[VIRTIO_BALLOON_VQ_STATS] = stats_request;
	}

	if (virtio_has_feature(vb->vdev, VIRTIO_BALLOON_F_FREE_PAGE_HINT)) {
		names[VIRTIO_BALLOON_VQ_FREE_PAGE] = "free_page_vq";
		callbacks[VIRTIO_BALLOON_VQ_FREE_PAGE] =
					virtio_balloon_free_used_hint_buf;
	}

	err = vb->vdev->config->find_vqs(vb->vdev, VIRTIO_BALLOON_VQ_MAX,
					 vqs, callbacks, names, NULL, NULL);
	if (err)
		return err;

	vb->inflate_vq = vqs[VIRTIO_BALLOON_VQ_INFLATE];
	vb->deflate_vq = vqs[VIRTIO_BALLOON_VQ_DEFLATE];
	if (virtio_has_feature(vb->vdev, VIRTIO_BALLOON_F_STATS_VQ)) {
		struct scatterlist sg;
		unsigned int num_stats;
		vb->stats_vq = vqs[VIRTIO_BALLOON_VQ_STATS];

		/*
		 * Prime this virtqueue with one buffer so the hypervisor can
		 * use it to signal us later (it can't be broken yet!).
		 */
		num_stats = update_balloon_stats(vb);

		sg_init_one(&sg, vb->stats, sizeof(vb->stats[0]) * num_stats);
		err = virtqueue_add_outbuf(vb->stats_vq, &sg, 1, vb,
					   GFP_KERNEL);
		if (err) {
			dev_warn(&vb->vdev->dev, "%s: add stat_vq failed\n",
				 __func__);
			return err;
		}
		virtqueue_kick(vb->stats_vq);
	}

	if (virtio_has_feature(vb->vdev, VIRTIO_BALLOON_F_FREE_PAGE_HINT))
		vb->free_page_vq = vqs[VIRTIO_BALLOON_VQ_FREE_PAGE];

	return 0;
}

static int send_start_cmd_id(struct virtio_balloon *vb)
{
	struct scatterlist sg;
	struct virtqueue *vq = vb->free_page_vq;
	int err;

	virtio_balloon_free_used_hint_buf(vq);

	vb->cmd_start.id = cpu_to_virtio32(vb->vdev, vb->cmd_id_active);
	vb->cmd_start.size = cpu_to_virtio32(vb->vdev,
					     MAX_ORDER_NR_PAGES * PAGE_SIZE);
	sg_init_one(&sg, &vb->cmd_start,
		    sizeof(struct virtio_balloon_free_page_hints_cmd));

	err = virtqueue_add_outbuf(vq, &sg, 1, &vb->cmd_start, GFP_KERNEL);
	if (!err)
		virtqueue_kick(vq);
	return err;
}

static int send_stop_cmd_id(struct virtio_balloon *vb)
{
	struct scatterlist sg;
	struct virtqueue *vq = vb->free_page_vq;
	int err;

	virtio_balloon_free_used_hint_buf(vq);

	vb->cmd_stop.id = cpu_to_virtio32(vb->vdev,
				VIRTIO_BALLOON_FREE_PAGE_REPORT_STOP_ID);
	vb->cmd_stop.size = 0;
	sg_init_one(&sg, &vb->cmd_stop,
		    sizeof(struct virtio_balloon_free_page_hints_cmd));
	err = virtqueue_add_outbuf(vq, &sg, 1, &vb->cmd_stop, GFP_KERNEL);
	if (!err)
		virtqueue_kick(vq);
	return err;
}

static int send_hint_buf(struct virtio_balloon *vb, void *buf,
			 unsigned int size)
{
	int err;
	struct scatterlist sg;
	struct virtqueue *vq = vb->free_page_vq;

	virtio_balloon_free_used_hint_buf(vq);

	/*
	 * If a stop id or a new cmd id was just received from host,
	 * stop the reporting, return -EINTR to indicate an active stop.
	 */
	if (vb->cmd_id_received != vb->cmd_id_active)
		return -EINTR;

	/* There is always one entry reserved for the cmd id to use. */
	if (vq->num_free < 2)
		return -ENOSPC;

	sg_init_one(&sg, buf, size);
	err = virtqueue_add_inbuf(vq, &sg, 1, buf, GFP_KERNEL);
	if (!err)
		virtqueue_kick(vq);
	return err;
}

static void virtio_balloon_free_hint_bufs(struct list_head *pages)
{
	struct page *page, *next;

	list_for_each_entry_safe(page, next, pages, lru) {
		__free_pages(page, VIRTIO_BALLOON_HINT_BUF_ORDER);
		list_del(&page->lru);
	}
}

/*
 * virtio_balloon_send_hints - send buffers of hints to host
 * @vb: the virtio_balloon struct
 * @pages: the list of page blocks used as buffers
 * @hint_num: the total number of hints
 *
 * Send buffers of hints to host. This begins by sending a start cmd, which
 * contains a cmd id received from host and the free page block size in bytes
 * of each hint. At the end, a stop cmd is sent to host to indicate the end
 * of this reporting. If host actively requests to stop the reporting, free
 * the buffers that have not been sent.
 */
static void virtio_balloon_send_hints(struct virtio_balloon *vb,
				      struct list_head *pages,
				      unsigned long hint_num)
{
	struct page *page, *next;
	void *buf;
	unsigned int buf_size, hint_size;
	int err;

	/* Start by sending the received cmd id to host with an outbuf. */
	err = send_start_cmd_id(vb);
	if (unlikely(err))
		goto out_free;

	list_for_each_entry_safe(page, next, pages, lru) {
		/* We've sent all the hints. */
		if (!hint_num)
			break;
		hint_size = hint_num * sizeof(__le64);
		buf = page_address(page);
		buf_size = hint_size > VIRTIO_BALLOON_HINT_BUF_SIZE ?
			   VIRTIO_BALLOON_HINT_BUF_SIZE : hint_size;
		hint_num -= buf_size / sizeof(__le64);
		err = send_hint_buf(vb, buf, buf_size);
		/*
		 * If host actively stops the reporting or no space to add more
		 * hint bufs, just stop adding hints and continue to add the
		 * stop cmd. Other device errors need to bail out with an error
		 * message.
		 */
		if (unlikely(err == -EINTR || err == -ENOSPC))
			break;
		else if (unlikely(err))
			goto out_free;
		/*
		 * Remove the buffer from the list only when it has been given
		 * to host. Otherwise, it will stay on the list and will be
		 * freed via virtio_balloon_free_hint_bufs.
		 */
		list_del(&page->lru);
	}

	/* End by sending a stop id to host with an outbuf. */
	err = send_stop_cmd_id(vb);
out_free:
	if (err)
		dev_err(&vb->vdev->dev, "%s: err = %d\n", __func__, err);
	/* Free all the buffers that are not sent to host. */
	virtio_balloon_free_hint_bufs(pages);
}

/*
 * Allocate a list of buffers to load free page hints. Those buffers are
 * allocated based on the estimation of the max number of free page blocks
 * that the system may have, so that they are sufficient to store all the
 * free page addresses.
 *
 * Return 0 on success, otherwise false.
 */
static int virtio_balloon_alloc_hint_bufs(struct list_head *pages)
{
	struct page *page;
	unsigned long max_entries, entries_per_page, entries_per_buf,
		      max_buf_num;
	int i;

	max_entries = max_free_page_blocks(VIRTIO_BALLOON_HINT_BUF_ORDER);
	entries_per_page = PAGE_SIZE / sizeof(__le64);
	entries_per_buf = entries_per_page * VIRTIO_BALLOON_HINT_BUF_PAGES;
	max_buf_num = max_entries / entries_per_buf +
		      !!(max_entries % entries_per_buf);

	for (i = 0; i < max_buf_num; i++) {
		page = alloc_pages(__GFP_ATOMIC | __GFP_NOMEMALLOC,
				   VIRTIO_BALLOON_HINT_BUF_ORDER);
		if (!page) {
			/*
			 * If any one of the buffers fails to be allocated, it
			 * implies that the free list that we are interested
			 * in is empty, and there is no need to continue the
			 * reporting. So just free what's allocated and return
			 * -ENOMEM.
			 */
			virtio_balloon_free_hint_bufs(pages);
			return -ENOMEM;
		}
		list_add(&page->lru, pages);
	}

	return 0;
}

/*
 * virtio_balloon_load_hints - load free page hints into buffers
 * @vb: the virtio_balloon struct
 * @pages: the list of page blocks used as buffers
 *
 * Only free pages blocks of MAX_ORDER - 1 are loaded into the buffers.
 * Each buffer size is MAX_ORDER_NR_PAGES * PAGE_SIZE (e.g. 4MB on x86).
 * Failing to allocate such a buffer essentially implies that no such free
 * page blocks could be reported.
 *
 * Return the total number of hints loaded into the buffers.
 */
static unsigned long virtio_balloon_load_hints(struct virtio_balloon *vb,
					       struct list_head *pages)
{
	unsigned long loaded_hints = 0;
	int ret;

	do {
		ret = virtio_balloon_alloc_hint_bufs(pages);
		if (ret)
			return 0;

		ret = get_from_free_page_list(VIRTIO_BALLOON_HINT_BUF_ORDER,
					pages, VIRTIO_BALLOON_HINT_BUF_SIZE,
					&loaded_hints);
		/*
		 * Retry in the case that memory is onlined quickly, which
		 * causes the allocated buffers to be insufficient to store
		 * all the free page addresses. Free the hint buffers before
		 * retry.
		 */
		if (unlikely(ret == -ENOSPC))
			virtio_balloon_free_hint_bufs(pages);
	} while (ret == -ENOSPC);

	return loaded_hints;
}

static void report_free_page_func(struct work_struct *work)
{
	struct virtio_balloon *vb;
	unsigned long loaded_hints = 0;
	LIST_HEAD(pages);

	vb = container_of(work, struct virtio_balloon, report_free_page_work);
	vb->cmd_id_active = vb->cmd_id_received;

	loaded_hints = virtio_balloon_load_hints(vb, &pages);
	if (loaded_hints)
		virtio_balloon_send_hints(vb, &pages, loaded_hints);
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
	vb->num_pfns = VIRTIO_BALLOON_PAGES_PER_PAGE;
	set_page_pfns(vb, vb->pfns, newpage);
	tell_host(vb, vb->inflate_vq);

	/* balloon's page migration 2nd step -- deflate "page" */
	balloon_page_delete(page);
	vb->num_pfns = VIRTIO_BALLOON_PAGES_PER_PAGE;
	set_page_pfns(vb, vb->pfns, page);
	tell_host(vb, vb->deflate_vq);

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

	if (virtio_has_feature(vdev, VIRTIO_BALLOON_F_FREE_PAGE_HINT)) {
		/*
		 * There is always one entry reserved for cmd id, so the ring
		 * size needs to be at least two to report free page hints.
		 */
		if (virtqueue_get_vring_size(vb->free_page_vq) < 2) {
			err = -ENOSPC;
			goto out_del_vqs;
		}
		vb->balloon_wq = alloc_workqueue("balloon-wq",
					WQ_FREEZABLE | WQ_CPU_INTENSIVE, 0);
		if (!vb->balloon_wq) {
			err = -ENOMEM;
			goto out_del_vqs;
		}
		INIT_WORK(&vb->report_free_page_work, report_free_page_func);
		vb->cmd_id_received = VIRTIO_BALLOON_FREE_PAGE_REPORT_STOP_ID;
		vb->cmd_id_active = VIRTIO_BALLOON_FREE_PAGE_REPORT_STOP_ID;
	}

	vb->nb.notifier_call = virtballoon_oom_notify;
	vb->nb.priority = VIRTBALLOON_OOM_NOTIFY_PRIORITY;
	err = register_oom_notifier(&vb->nb);
	if (err < 0)
		goto out_del_balloon_wq;

#ifdef CONFIG_BALLOON_COMPACTION
	balloon_mnt = kern_mount(&balloon_fs);
	if (IS_ERR(balloon_mnt)) {
		err = PTR_ERR(balloon_mnt);
		unregister_oom_notifier(&vb->nb);
		goto out_del_balloon_wq;
	}

	vb->vb_dev_info.migratepage = virtballoon_migratepage;
	vb->vb_dev_info.inode = alloc_anon_inode(balloon_mnt->mnt_sb);
	if (IS_ERR(vb->vb_dev_info.inode)) {
		err = PTR_ERR(vb->vb_dev_info.inode);
		kern_unmount(balloon_mnt);
		unregister_oom_notifier(&vb->nb);
		vb->vb_dev_info.inode = NULL;
		goto out_del_balloon_wq;
	}
	vb->vb_dev_info.inode->i_mapping->a_ops = &balloon_aops;
#endif

	virtio_device_ready(vdev);

	if (towards_target(vb))
		virtballoon_changed(vdev);
	return 0;

out_del_balloon_wq:
	if (virtio_has_feature(vdev, VIRTIO_BALLOON_F_FREE_PAGE_HINT))
		destroy_workqueue(vb->balloon_wq);
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

	if (virtio_has_feature(vdev, VIRTIO_BALLOON_F_FREE_PAGE_HINT)) {
		cancel_work_sync(&vb->report_free_page_work);
		destroy_workqueue(vb->balloon_wq);
	}

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
	VIRTIO_BALLOON_F_FREE_PAGE_HINT,
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
