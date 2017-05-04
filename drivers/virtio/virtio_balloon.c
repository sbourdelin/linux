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

/* The size of one page_bmap used to record inflated/deflated pages. */
#define VIRTIO_BALLOON_PAGE_BMAP_SIZE	(8 * PAGE_SIZE)
/*
 * Callulates how many pfns can a page_bmap record. A bit corresponds to a
 * page of PAGE_SIZE.
 */
#define VIRTIO_BALLOON_PFNS_PER_PAGE_BMAP \
	(VIRTIO_BALLOON_PAGE_BMAP_SIZE * BITS_PER_BYTE)

/* The number of page_bmap to allocate by default. */
#define VIRTIO_BALLOON_PAGE_BMAP_DEFAULT_NUM	1
/* The maximum number of page_bmap that can be allocated. */
#define VIRTIO_BALLOON_PAGE_BMAP_MAX_NUM	32

/* Types of pages to chunk */
#define PAGE_CHUNK_TYPE_BALLOON	0	/* Chunk of inflate/deflate pages */
#define PAGE_CHUNK_TYPE_UNUSED	1	/* Chunk of unused pages */

static int oom_pages = OOM_VBALLOON_DEFAULT_PAGES;
module_param(oom_pages, int, S_IRUSR | S_IWUSR);
MODULE_PARM_DESC(oom_pages, "pages to free on OOM");

#ifdef CONFIG_BALLOON_COMPACTION
static struct vfsmount *balloon_mnt;
#endif

/* Maximum number of page chunks */
#define VIRTIO_BALLOON_MAX_PAGE_CHUNKS ((8 * PAGE_SIZE - \
				sizeof(struct virtio_balloon_miscq_msg)) / \
				sizeof(struct virtio_balloon_page_chunk_entry))

struct virtio_balloon {
	struct virtio_device *vdev;
	struct virtqueue *inflate_vq, *deflate_vq, *stats_vq, *miscq;

	/* The balloon servicing is delegated to a freezable workqueue. */
	struct work_struct update_balloon_stats_work;
	struct work_struct update_balloon_size_work;
	struct work_struct miscq_handle_work;

	/* Prevent updating balloon when it is being canceled. */
	spinlock_t stop_update_lock;
	bool stop_update;

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

	/* Miscq msg buffer for the REPORT_UNUSED_PAGES cmd */
	struct virtio_balloon_miscq_msg *miscq_msg_rup;

	/* Buffer for chunks of ballooned pages. */
	struct virtio_balloon_page_chunk *balloon_page_chunk;

	/* Bitmap used to record pages. */
	unsigned long *page_bmap[VIRTIO_BALLOON_PAGE_BMAP_MAX_NUM];

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

/* Update pfn_max and pfn_min according to the pfn of page */
static inline void update_pfn_range(struct virtio_balloon *vb,
				    struct page *page,
				    unsigned long *pfn_min,
				    unsigned long *pfn_max)
{
	unsigned long pfn = page_to_pfn(page);

	*pfn_min = min(pfn, *pfn_min);
	*pfn_max = max(pfn, *pfn_max);
}

static unsigned int extend_page_bmap_size(struct virtio_balloon *vb,
					  unsigned long pfn_num)
{
	unsigned int i, bmap_num, allocated_bmap_num;
	unsigned long bmap_len;

	allocated_bmap_num = VIRTIO_BALLOON_PAGE_BMAP_DEFAULT_NUM;
	bmap_len = ALIGN(pfn_num, BITS_PER_LONG) / BITS_PER_BYTE;
	bmap_len = roundup(bmap_len, VIRTIO_BALLOON_PAGE_BMAP_SIZE);
	/*
	 * VIRTIO_BALLOON_PAGE_BMAP_SIZE is the size of one page_bmap, so
	 * divide it to calculate how many page_bmap that we need.
	 */
	bmap_num = (unsigned int)(bmap_len / VIRTIO_BALLOON_PAGE_BMAP_SIZE);
	/* The number of page_bmap to allocate should not exceed the max */
	bmap_num = min_t(unsigned int, VIRTIO_BALLOON_PAGE_BMAP_MAX_NUM,
			 bmap_num);

	for (i = VIRTIO_BALLOON_PAGE_BMAP_DEFAULT_NUM; i < bmap_num; i++) {
		vb->page_bmap[i] = kmalloc(VIRTIO_BALLOON_PAGE_BMAP_SIZE,
					   GFP_KERNEL);
		if (vb->page_bmap[i])
			allocated_bmap_num++;
		else
			break;
	}

	return allocated_bmap_num;
}

static void free_extended_page_bmap(struct virtio_balloon *vb,
				    unsigned int page_bmap_num)
{
	unsigned int i;

	for (i = VIRTIO_BALLOON_PAGE_BMAP_DEFAULT_NUM; i < page_bmap_num;
	     i++) {
		kfree(vb->page_bmap[i]);
		vb->page_bmap[i] = NULL;
		page_bmap_num--;
	}
}

static void clear_page_bmap(struct virtio_balloon *vb,
			    unsigned int page_bmap_num)
{
	int i;

	for (i = 0; i < page_bmap_num; i++)
		memset(vb->page_bmap[i], 0, VIRTIO_BALLOON_PAGE_BMAP_SIZE);
}

static void send_page_chunks(struct virtio_balloon *vb, struct virtqueue *vq,
			     int type, bool busy_wait)
{
	struct scatterlist sg;
	struct virtio_balloon_page_chunk *chunk;
	void *msg_buf;
	unsigned int msg_len;
	uint64_t chunk_num = 0;

	switch (type) {
	case PAGE_CHUNK_TYPE_BALLOON:
		chunk = vb->balloon_page_chunk;
		chunk_num = le64_to_cpu(chunk->chunk_num);
		msg_buf = vb->balloon_page_chunk;
		msg_len = sizeof(struct virtio_balloon_page_chunk) +
			  sizeof(struct virtio_balloon_page_chunk_entry) *
			  chunk_num;
		break;
	case PAGE_CHUNK_TYPE_UNUSED:
		chunk = &vb->miscq_msg_rup->payload.chunk;
		chunk_num = le64_to_cpu(chunk->chunk_num);
		msg_buf = vb->miscq_msg_rup;
		msg_len = sizeof(struct virtio_balloon_miscq_msg) +
			  sizeof(struct virtio_balloon_page_chunk_entry) *
			  chunk_num;
		break;
	default:
		dev_warn(&vb->vdev->dev, "%s: chunk %d of unknown pages\n",
			 __func__, type);
		return;
	}

	sg_init_one(&sg, msg_buf, msg_len);
	if (!virtqueue_add_outbuf(vq, &sg, 1, vb, GFP_KERNEL)) {
		virtqueue_kick(vq);
		if (busy_wait)
			while (!virtqueue_get_buf(vq, &msg_len) &&
			       !virtqueue_is_broken(vq))
				cpu_relax();
		else
			wait_event(vb->acked, virtqueue_get_buf(vq, &msg_len));
		/*
		 * Now, the chunks have been delivered to the host.
		 * Reset the filed in the structure that records the number of
		 * added chunks, so that new added chunks can be re-counted.
		 */
		chunk->chunk_num = 0;
	}
}

/* Add a chunk entry to the buffer. */
static void add_one_chunk(struct virtio_balloon *vb, struct virtqueue *vq,
			  int type, u64 base, u64 size)
{
	struct virtio_balloon_page_chunk *chunk;
	struct virtio_balloon_page_chunk_entry *entry;
	uint64_t chunk_num;

	switch (type) {
	case PAGE_CHUNK_TYPE_BALLOON:
		chunk = vb->balloon_page_chunk;
		chunk_num = le64_to_cpu(vb->balloon_page_chunk->chunk_num);
		break;
	case PAGE_CHUNK_TYPE_UNUSED:
		chunk = &vb->miscq_msg_rup->payload.chunk;
		chunk_num =
		le64_to_cpu(vb->miscq_msg_rup->payload.chunk.chunk_num);
		break;
	default:
		dev_warn(&vb->vdev->dev, "%s: chunk %d of unknown pages\n",
			 __func__, type);
		return;
	}
	entry = &chunk->entry[chunk_num];
	entry->base = cpu_to_le64(base << VIRTIO_BALLOON_CHUNK_BASE_SHIFT);
	entry->size = cpu_to_le64(size << VIRTIO_BALLOON_CHUNK_SIZE_SHIFT);
	chunk->chunk_num = cpu_to_le64(++chunk_num);
	if (chunk_num == VIRTIO_BALLOON_MAX_PAGE_CHUNKS)
		send_page_chunks(vb, vq, type, 0);
}

static void convert_bmap_to_chunks(struct virtio_balloon *vb,
				   struct virtqueue *vq,
				   unsigned long *bmap,
				   unsigned long pfn_start,
				   unsigned long size)
{
	unsigned long next_one, next_zero, chunk_size, pos = 0;

	while (pos < size) {
		next_one = find_next_bit(bmap, size, pos);
		/*
		 * No "1" bit found, which means that there is no pfn
		 * recorded in the rest of this bmap.
		 */
		if (next_one == size)
			break;
		next_zero = find_next_zero_bit(bmap, size, next_one + 1);
		/*
		 * A bit in page_bmap corresponds to a page of PAGE_SIZE.
		 * Convert it to be pages of 4KB balloon page size when
		 * adding it to a chunk.
		 */
		chunk_size = (next_zero - next_one) *
			     VIRTIO_BALLOON_PAGES_PER_PAGE;
		if (chunk_size) {
			add_one_chunk(vb, vq, PAGE_CHUNK_TYPE_BALLOON,
				      pfn_start + next_one, chunk_size);
			pos += next_zero + 1;
		}
	}
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

static void tell_host_from_page_bmap(struct virtio_balloon *vb,
				     struct virtqueue *vq,
				     unsigned long pfn_start,
				     unsigned long pfn_end,
				     unsigned int page_bmap_num)
{
	unsigned long i, pfn_num;

	for (i = 0; i < page_bmap_num; i++) {
		/*
		 * For the last page_bmap, only the remaining number of pfns
		 * need to be searched rather than the entire page_bmap.
		 */
		if (i + 1 == page_bmap_num)
			pfn_num = (pfn_end - pfn_start) %
				  VIRTIO_BALLOON_PFNS_PER_PAGE_BMAP;
		else
			pfn_num = VIRTIO_BALLOON_PFNS_PER_PAGE_BMAP;

		convert_bmap_to_chunks(vb, vq, vb->page_bmap[i], pfn_start +
				       i * VIRTIO_BALLOON_PFNS_PER_PAGE_BMAP,
				       pfn_num);
	}
	if (le64_to_cpu(vb->balloon_page_chunk->chunk_num) > 0)
		send_page_chunks(vb, vq, PAGE_CHUNK_TYPE_BALLOON, 0);
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

/*
 * Send ballooned pages in chunks to host.
 * The ballooned pages are recorded in page bitmaps. Each bit in a bitmap
 * corresponds to a page of PAGE_SIZE. The page bitmaps are searched for
 * continuous "1" bits, which correspond to continuous pages, to chunk.
 * When packing those continuous pages into chunks, pages are converted into
 * 4KB balloon pages.
 *
 * pfn_max and pfn_min form the range of pfns that need to use page bitmaps to
 * record. If the range is too large to be recorded into the allocated page
 * bitmaps, the page bitmaps are used multiple times to record the entire
 * range of pfns.
 */
static void tell_host_page_chunks(struct virtio_balloon *vb,
				  struct list_head *pages,
				  struct virtqueue *vq,
				  unsigned long pfn_max,
				  unsigned long pfn_min)
{
	/*
	 * The pfn_start and pfn_end form the range of pfns that the allocated
	 * page_bmap can record in each round.
	 */
	unsigned long pfn_start, pfn_end;
	/* Total number of allocated page_bmap */
	unsigned int page_bmap_num;
	struct page *page;
	bool found;

	/*
	 * In the case that one page_bmap is not sufficient to record the pfn
	 * range, page_bmap will be extended by allocating more numbers of
	 * page_bmap.
	 */
	page_bmap_num = extend_page_bmap_size(vb, pfn_max - pfn_min + 1);

	/* Start from the beginning of the whole pfn range */
	pfn_start = pfn_min;
	while (pfn_start < pfn_max) {
		pfn_end = pfn_start +
			  VIRTIO_BALLOON_PFNS_PER_PAGE_BMAP * page_bmap_num;
		pfn_end = pfn_end < pfn_max ? pfn_end : pfn_max;
		clear_page_bmap(vb, page_bmap_num);
		found = false;

		list_for_each_entry(page, pages, lru) {
			unsigned long bmap_idx, bmap_pos, this_pfn;

			this_pfn = page_to_pfn(page);
			if (this_pfn < pfn_start || this_pfn > pfn_end)
				continue;
			bmap_idx = (this_pfn - pfn_start) /
				   VIRTIO_BALLOON_PFNS_PER_PAGE_BMAP;
			bmap_pos = (this_pfn - pfn_start) %
				   VIRTIO_BALLOON_PFNS_PER_PAGE_BMAP;
			set_bit(bmap_pos, vb->page_bmap[bmap_idx]);

			found = true;
		}
		if (found)
			tell_host_from_page_bmap(vb, vq, pfn_start, pfn_end,
						 page_bmap_num);
		/*
		 * Start the next round when pfn_start and pfn_end couldn't
		 * cover the whole pfn range given by pfn_max and pfn_min.
		 */
		pfn_start = pfn_end;
	}
	free_extended_page_bmap(vb, page_bmap_num);
}

static unsigned fill_balloon(struct virtio_balloon *vb, size_t num)
{
	struct balloon_dev_info *vb_dev_info = &vb->vb_dev_info;
	unsigned num_allocated_pages;
	bool chunking = virtio_has_feature(vb->vdev,
					   VIRTIO_BALLOON_F_PAGE_CHUNKS);
	unsigned long pfn_max = 0, pfn_min = ULONG_MAX;

	/* We can only do one array worth at a time. */
	if (!chunking)
		num = min(num, ARRAY_SIZE(vb->pfns));

	mutex_lock(&vb->balloon_lock);
	for (vb->num_pfns = 0; vb->num_pfns < num;
	     vb->num_pfns += VIRTIO_BALLOON_PAGES_PER_PAGE) {
		struct page *page = balloon_page_enqueue(vb_dev_info);

		if (!page) {
			dev_info_ratelimited(&vb->vdev->dev,
					     "Out of puff! Can't get %u pages\n",
					     VIRTIO_BALLOON_PAGES_PER_PAGE);
			/* Sleep for at least 1/5 of a second before retry. */
			msleep(200);
			break;
		}
		if (chunking)
			update_pfn_range(vb, page, &pfn_max, &pfn_min);
		else
			set_page_pfns(vb, vb->pfns + vb->num_pfns, page);
		vb->num_pages += VIRTIO_BALLOON_PAGES_PER_PAGE;
		if (!virtio_has_feature(vb->vdev,
					VIRTIO_BALLOON_F_DEFLATE_ON_OOM))
			adjust_managed_page_count(page, -1);
	}

	num_allocated_pages = vb->num_pfns;
	/* Did we get any? */
	if (vb->num_pfns != 0) {
		if (chunking)
			tell_host_page_chunks(vb, &vb_dev_info->pages,
					      vb->inflate_vq,
					      pfn_max, pfn_min);
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
	bool chunking = virtio_has_feature(vb->vdev,
					   VIRTIO_BALLOON_F_PAGE_CHUNKS);
	unsigned long pfn_max = 0, pfn_min = ULONG_MAX;

	/* Traditionally, we can only do one array worth at a time. */
	if (!chunking)
		num = min(num, ARRAY_SIZE(vb->pfns));

	mutex_lock(&vb->balloon_lock);
	/* We can't release more pages than taken */
	num = min(num, (size_t)vb->num_pages);
	for (vb->num_pfns = 0; vb->num_pfns < num;
	     vb->num_pfns += VIRTIO_BALLOON_PAGES_PER_PAGE) {
		page = balloon_page_dequeue(vb_dev_info);
		if (!page)
			break;
		if (chunking)
			update_pfn_range(vb, page, &pfn_max, &pfn_min);
		else
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
	if (vb->num_pfns != 0) {
		if (chunking)
			tell_host_page_chunks(vb, &pages, vb->deflate_vq,
					      pfn_max, pfn_min);
		else
			tell_host(vb, vb->deflate_vq);
	}
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

static void virtballoon_changed(struct virtio_device *vdev)
{
	struct virtio_balloon *vb = vdev->priv;
	unsigned long flags;

	spin_lock_irqsave(&vb->stop_update_lock, flags);
	if (!vb->stop_update)
		queue_work(system_freezable_wq, &vb->update_balloon_size_work);
	spin_unlock_irqrestore(&vb->stop_update_lock, flags);
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

/* Add a message buffer for the host to fill in a request */
static void miscq_msg_inbuf_add(struct virtio_balloon *vb,
			      struct virtio_balloon_miscq_msg *req_buf)
{
	struct scatterlist sg_in;

	sg_init_one(&sg_in, req_buf, sizeof(struct virtio_balloon_miscq_msg));
	if (virtqueue_add_inbuf(vb->miscq, &sg_in, 1, req_buf, GFP_KERNEL)
	    < 0) {
		__virtio_clear_bit(vb->vdev,
				   VIRTIO_BALLOON_F_MISC_VQ);
		dev_warn(&vb->vdev->dev, "%s: add miscq msg buf err\n",
			 __func__);
		return;
	}
	virtqueue_kick(vb->miscq);
}

static void miscq_report_unused_pages(struct virtio_balloon *vb)
{
	struct virtio_balloon_miscq_msg *msg = vb->miscq_msg_rup;
	struct virtqueue *vq = vb->miscq;
	int ret = 0;
	unsigned int order = 0, migratetype = 0;
	struct zone *zone = NULL;
	struct page *page = NULL;
	u64 pfn;

	msg->cmd = cpu_to_le32(VIRTIO_BALLOON_MISCQ_CMD_REPORT_UNUSED_PAGES);
	msg->flags = 0;

	for_each_populated_zone(zone) {
		for (order = MAX_ORDER - 1; order > 0; order--) {
			for (migratetype = 0; migratetype < MIGRATE_TYPES;
			     migratetype++) {
				do {
					ret = report_unused_page_block(zone,
						order, migratetype, &page);
					if (!ret) {
						pfn = (u64)page_to_pfn(page);
						add_one_chunk(vb, vq,
							PAGE_CHUNK_TYPE_UNUSED,
							pfn,
							(u64)(1 << order) *
						VIRTIO_BALLOON_PAGES_PER_PAGE);
					}
				} while (!ret);
			}
		}
	}
	/* Set the cmd completion flag */
	msg->flags |= cpu_to_le32(VIRTIO_BALLOON_MISCQ_F_COMPLETION);
	send_page_chunks(vb, vq, PAGE_CHUNK_TYPE_UNUSED, true);
}

static void miscq_handle_func(struct work_struct *work)
{
	struct virtio_balloon *vb;
	struct virtio_balloon_miscq_msg *msg;
	unsigned int len;

	vb = container_of(work, struct virtio_balloon,
			  miscq_handle_work);
	msg = virtqueue_get_buf(vb->miscq, &len);
	if (!msg || len != sizeof(struct virtio_balloon_miscq_msg)) {
		dev_warn(&vb->vdev->dev, "%s: invalid miscq msg len\n",
			 __func__);
		miscq_msg_inbuf_add(vb, vb->miscq_msg_rup);
		return;
	}
	switch (msg->cmd) {
	case VIRTIO_BALLOON_MISCQ_CMD_REPORT_UNUSED_PAGES:
		miscq_report_unused_pages(vb);
		break;
	default:
		dev_warn(&vb->vdev->dev, "%s: miscq cmd %d not supported\n",
			 __func__, msg->cmd);
	}
	miscq_msg_inbuf_add(vb, vb->miscq_msg_rup);
}

static void miscq_request(struct virtqueue *vq)
{
	struct virtio_balloon *vb = vq->vdev->priv;

	queue_work(system_freezable_wq, &vb->miscq_handle_work);
}

static int init_vqs(struct virtio_balloon *vb)
{
	struct virtqueue **vqs;
	vq_callback_t **callbacks;
	const char **names;
	int err = -ENOMEM;
	int i, nvqs;

	 /* Inflateq and deflateq are used unconditionally */
	nvqs = 2;

	if (virtio_has_feature(vb->vdev, VIRTIO_BALLOON_F_STATS_VQ))
		nvqs++;
	if (virtio_has_feature(vb->vdev, VIRTIO_BALLOON_F_MISC_VQ))
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

	if (virtio_has_feature(vb->vdev,
				      VIRTIO_BALLOON_F_MISC_VQ)) {
		callbacks[i] = miscq_request;
		names[i] = "miscq";
	}

	err = vb->vdev->config->find_vqs(vb->vdev, nvqs, vqs, callbacks,
					 names, NULL);
	if (err)
		goto err_find;

	vb->inflate_vq = vqs[0];
	vb->deflate_vq = vqs[1];
	i = 2;
	if (virtio_has_feature(vb->vdev, VIRTIO_BALLOON_F_STATS_VQ)) {
		struct scatterlist sg;

		vb->stats_vq = vqs[i++];
		/*
		 * Prime this virtqueue with one buffer so the hypervisor can
		 * use it to signal us later (it can't be broken yet!).
		 */
		sg_init_one(&sg, vb->stats, sizeof(vb->stats));
		if (virtqueue_add_outbuf(vb->stats_vq, &sg, 1, vb, GFP_KERNEL)
		    < 0)
			BUG();
		virtqueue_kick(vb->stats_vq);
	}

	if (virtio_has_feature(vb->vdev, VIRTIO_BALLOON_F_MISC_VQ)) {
		vb->miscq = vqs[i];
		/*
		 * Add the msg buf for the REPORT_UNUSED_PAGES request.
		 * The request is handled one in-flight each time. So, just
		 * use the response buffer, msicq_msg_rup, for the host to
		 * fill in a request.
		 */
		miscq_msg_inbuf_add(vb, vb->miscq_msg_rup);
	}

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

#ifdef CONFIG_BALLOON_COMPACTION

static void tell_host_one_page(struct virtio_balloon *vb,
			       struct virtqueue *vq, struct page *page)
{
	add_one_chunk(vb, vq, PAGE_CHUNK_TYPE_BALLOON, page_to_pfn(page),
		      VIRTIO_BALLOON_PAGES_PER_PAGE);
}

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
	bool chunking = virtio_has_feature(vb->vdev,
					   VIRTIO_BALLOON_F_PAGE_CHUNKS);
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
	if (chunking) {
		tell_host_one_page(vb, vb->inflate_vq, newpage);
	} else {
		vb->num_pfns = VIRTIO_BALLOON_PAGES_PER_PAGE;
		set_page_pfns(vb, vb->pfns, newpage);
		tell_host(vb, vb->inflate_vq);
	}
	/* balloon's page migration 2nd step -- deflate "page" */
	balloon_page_delete(page);
	if (chunking) {
		tell_host_one_page(vb, vb->deflate_vq, page);
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

static void free_page_bmap(struct virtio_balloon *vb)
{
	int i;

	for (i = 0; i < VIRTIO_BALLOON_PAGE_BMAP_DEFAULT_NUM; i++) {
		kfree(vb->page_bmap[i]);
		vb->page_bmap[i] = NULL;
	}
}

static int balloon_page_chunk_init(struct virtio_balloon *vb)
{
	int i;

	vb->balloon_page_chunk = kmalloc(sizeof(__le64) +
			sizeof(struct virtio_balloon_page_chunk_entry) *
			VIRTIO_BALLOON_MAX_PAGE_CHUNKS, GFP_KERNEL);
	if (!vb->balloon_page_chunk)
		goto err_page_chunk;

	/*
	 * The default number of page_bmaps are allocated. More may be
	 * allocated on demand.
	 */
	for (i = 0; i < VIRTIO_BALLOON_PAGE_BMAP_DEFAULT_NUM; i++) {
		vb->page_bmap[i] = kmalloc(VIRTIO_BALLOON_PAGE_BMAP_SIZE,
					   GFP_KERNEL);
		if (!vb->page_bmap[i])
			goto err_page_bmap;
	}

	return 0;
err_page_bmap:
	free_page_bmap(vb);
	kfree(vb->balloon_page_chunk);
	vb->balloon_page_chunk = NULL;
err_page_chunk:
	__virtio_clear_bit(vb->vdev, VIRTIO_BALLOON_F_PAGE_CHUNKS);
	dev_warn(&vb->vdev->dev, "%s: failed\n", __func__);
	return -ENOMEM;
}

static int miscq_init(struct virtio_balloon *vb)
{
	vb->miscq_msg_rup = kmalloc(sizeof(struct virtio_balloon_miscq_msg) +
			     sizeof(struct virtio_balloon_page_chunk_entry) *
			     VIRTIO_BALLOON_MAX_PAGE_CHUNKS, GFP_KERNEL);
	if (!vb->miscq_msg_rup) {
		__virtio_clear_bit(vb->vdev, VIRTIO_BALLOON_F_MISC_VQ);
		dev_warn(&vb->vdev->dev, "%s: failed\n", __func__);
		return -ENOMEM;
	}

	INIT_WORK(&vb->miscq_handle_work, miscq_handle_func);

	return 0;
}

static int virtballoon_validate(struct virtio_device *vdev)
{
	struct virtio_balloon *vb = NULL;
	int err;

	vdev->priv = vb = kmalloc(sizeof(*vb), GFP_KERNEL);
	if (!vb) {
		err = -ENOMEM;
		goto err_vb;
	}

	if (virtio_has_feature(vdev, VIRTIO_BALLOON_F_PAGE_CHUNKS)) {
		err = balloon_page_chunk_init(vb);
		if (err < 0)
			goto err_page_chunk;
	}

	if (virtio_has_feature(vdev, VIRTIO_BALLOON_F_MISC_VQ)) {
		err = miscq_init(vb);
		if (err < 0)
			goto err_miscq_rup;
	}

	return 0;
err_miscq_rup:
	free_page_bmap(vb);
	kfree(vb->balloon_page_chunk);
err_page_chunk:
	kfree(vb);
err_vb:
	return err;
}

static int virtballoon_probe(struct virtio_device *vdev)
{
	struct virtio_balloon *vb = vdev->priv;
	int err;

	if (!vdev->config->get) {
		dev_err(&vdev->dev, "%s failure: config access disabled\n",
			__func__);
		return -EINVAL;
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
	cancel_work_sync(&vb->miscq_handle_work);

	remove_common(vb);
	free_page_bmap(vb);
	kfree(vb->balloon_page_chunk);
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

static unsigned int features[] = {
	VIRTIO_BALLOON_F_MUST_TELL_HOST,
	VIRTIO_BALLOON_F_STATS_VQ,
	VIRTIO_BALLOON_F_DEFLATE_ON_OOM,
	VIRTIO_BALLOON_F_PAGE_CHUNKS,
	VIRTIO_BALLOON_F_MISC_VQ,
};

static struct virtio_driver virtio_balloon_driver = {
	.feature_table = features,
	.feature_table_size = ARRAY_SIZE(features),
	.driver.name =	KBUILD_MODNAME,
	.driver.owner =	THIS_MODULE,
	.id_table =	id_table,
	.probe =	virtballoon_probe,
	.remove =	virtballoon_remove,
	.validate =	virtballoon_validate,
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
