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

/*
 * Balloon device works in 4K page units.  So each page is pointed to by
 * multiple balloon pages.  All memory counters in this driver are in balloon
 * page units.
 */
#define VIRTIO_BALLOON_PAGES_PER_PAGE (unsigned)(PAGE_SIZE >> VIRTIO_BALLOON_PFN_SHIFT)
#define VIRTIO_BALLOON_ARRAY_PFNS_MAX 256
#define OOM_VBALLOON_DEFAULT_PAGES 256
#define VIRTBALLOON_OOM_NOTIFY_PRIORITY 80

#define BALLOON_BMAP_SIZE	(8 * PAGE_SIZE)
#define PFNS_PER_BMAP		(BALLOON_BMAP_SIZE * BITS_PER_BYTE)
#define BALLOON_BMAP_COUNT	32

static int oom_pages = OOM_VBALLOON_DEFAULT_PAGES;
module_param(oom_pages, int, S_IRUSR | S_IWUSR);
MODULE_PARM_DESC(oom_pages, "pages to free on OOM");

#ifdef CONFIG_BALLOON_COMPACTION
static struct vfsmount *balloon_mnt;
#endif

struct virtio_balloon {
	struct virtio_device *vdev;
	struct virtqueue *inflate_vq, *deflate_vq, *stats_vq;

	/* The balloon servicing is delegated to a freezable workqueue. */
	struct work_struct update_balloon_stats_work;
	struct work_struct update_balloon_size_work;

	/* Prevent updating balloon when it is being canceled. */
	spinlock_t stop_update_lock;
	bool stop_update;

	/* Waiting for host to ack the pages we released. */
	wait_queue_head_t acked;

	/* Number of balloon pages we've told the Host we're not using. */
	unsigned int num_pages;
	/* Pointer to the response header. */
	void *resp_hdr;
	/* Pointer to the start address of response data. */
	unsigned long *resp_data;
	/* Pointer offset of the response data. */
	unsigned long resp_pos;
	/* Bitmap and bitmap count used to tell the host the pages */
	unsigned long *page_bitmap[BALLOON_BMAP_COUNT];
	/* Number of split page bitmaps */
	unsigned int nr_page_bmap;
	/* Used to record the processed pfn range */
	unsigned long min_pfn, max_pfn, start_pfn, end_pfn;
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

static inline void init_bmap_pfn_range(struct virtio_balloon *vb)
{
	vb->min_pfn = ULONG_MAX;
	vb->max_pfn = 0;
}

static inline void update_bmap_pfn_range(struct virtio_balloon *vb,
				 struct page *page)
{
	unsigned long balloon_pfn = page_to_balloon_pfn(page);

	vb->min_pfn = min(balloon_pfn, vb->min_pfn);
	vb->max_pfn = max(balloon_pfn, vb->max_pfn);
}

static void extend_page_bitmap(struct virtio_balloon *vb,
				unsigned long nr_pfn)
{
	int i, bmap_count;
	unsigned long bmap_len;

	bmap_len = ALIGN(nr_pfn, BITS_PER_LONG) / BITS_PER_BYTE;
	bmap_len = ALIGN(bmap_len, BALLOON_BMAP_SIZE);
	bmap_count = min((int)(bmap_len / BALLOON_BMAP_SIZE),
				 BALLOON_BMAP_COUNT);

	for (i = 1; i < bmap_count; i++) {
		vb->page_bitmap[i] = kmalloc(BALLOON_BMAP_SIZE, GFP_KERNEL);
		if (vb->page_bitmap[i])
			vb->nr_page_bmap++;
		else
			break;
	}
}

static void free_extended_page_bitmap(struct virtio_balloon *vb)
{
	int i, bmap_count = vb->nr_page_bmap;


	for (i = 1; i < bmap_count; i++) {
		kfree(vb->page_bitmap[i]);
		vb->page_bitmap[i] = NULL;
		vb->nr_page_bmap--;
	}
}

static void kfree_page_bitmap(struct virtio_balloon *vb)
{
	int i;

	for (i = 0; i < vb->nr_page_bmap; i++)
		kfree(vb->page_bitmap[i]);
}

static void clear_page_bitmap(struct virtio_balloon *vb)
{
	int i;

	for (i = 0; i < vb->nr_page_bmap; i++)
		memset(vb->page_bitmap[i], 0, BALLOON_BMAP_SIZE);
}

static unsigned long do_set_resp_bitmap(struct virtio_balloon *vb,
	unsigned long *bitmap,	unsigned long base_pfn,
	unsigned long pos, int nr_page)

{
	struct virtio_balloon_bmap_hdr *hdr;
	unsigned long end, new_pos, new_end, nr_left, proccessed = 0;

	new_pos = pos;
	new_end = end = pos + nr_page;

	if (pos % BITS_PER_LONG) {
		unsigned long pos_s;

		pos_s = rounddown(pos, BITS_PER_LONG);
		hdr = (struct virtio_balloon_bmap_hdr *)(vb->resp_data
							 + vb->resp_pos);
		hdr->head.start_pfn = base_pfn + pos_s;
		hdr->head.page_shift = PAGE_SHIFT;
		hdr->head.bmap_len = sizeof(unsigned long);
		hdr->bmap[0] = cpu_to_virtio64(vb->vdev,
				 bitmap[pos_s / BITS_PER_LONG]);
		vb->resp_pos += 2;
		if (pos_s + BITS_PER_LONG >= end)
			return roundup(end, BITS_PER_LONG) - pos;
		new_pos = roundup(pos, BITS_PER_LONG);
	}

	if (end % BITS_PER_LONG) {
		unsigned long pos_e;

		pos_e = roundup(end, BITS_PER_LONG);
		hdr = (struct virtio_balloon_bmap_hdr *)(vb->resp_data
							 + vb->resp_pos);
		hdr->head.start_pfn = base_pfn + pos_e - BITS_PER_LONG;
		hdr->head.page_shift = PAGE_SHIFT;
		hdr->head.bmap_len = sizeof(unsigned long);
		hdr->bmap[0] = bitmap[pos_e / BITS_PER_LONG - 1];
		vb->resp_pos += 2;
		if (new_pos + BITS_PER_LONG >= pos_e)
			return pos_e - pos;
		new_end = rounddown(end, BITS_PER_LONG);
	}

	nr_left = nr_page = new_end - new_pos;

	while (proccessed < nr_page) {
		int bulk, order;

		order = get_order(nr_left << PAGE_SHIFT);
		if ((1 << order) > nr_left)
			order--;
		hdr = (struct virtio_balloon_bmap_hdr *)(vb->resp_data
							 + vb->resp_pos);
		hdr->head.start_pfn = base_pfn + new_pos + proccessed;
		hdr->head.page_shift = order + PAGE_SHIFT;
		hdr->head.bmap_len = 0;
		bulk = 1 << order;
		nr_left -= bulk;
		proccessed += bulk;
		vb->resp_pos++;
	}

	return roundup(end, BITS_PER_LONG) - pos;
}

static void send_resp_data(struct virtio_balloon *vb, struct virtqueue *vq,
			bool busy_wait)
{
	struct scatterlist sg[2];
	struct virtio_balloon_resp_hdr *hdr = vb->resp_hdr;
	unsigned int len;

	len = hdr->data_len = vb->resp_pos * sizeof(unsigned long);
	sg_init_table(sg, 2);
	sg_set_buf(&sg[0], hdr, sizeof(struct virtio_balloon_resp_hdr));
	sg_set_buf(&sg[1], vb->resp_data, len);

	if (virtqueue_add_outbuf(vq, sg, 2, vb, GFP_KERNEL) == 0) {
		virtqueue_kick(vq);
		if (busy_wait)
			while (!virtqueue_get_buf(vq, &len)
				&& !virtqueue_is_broken(vq))
				cpu_relax();
		else
			wait_event(vb->acked, virtqueue_get_buf(vq, &len));
		vb->resp_pos = 0;
		free_extended_page_bitmap(vb);
	}
}

static void set_bulk_pages(struct virtio_balloon *vb, struct virtqueue *vq,
		unsigned long start_pfn, unsigned long *bitmap,
		unsigned long len, bool busy_wait)
{
	unsigned long pos = 0, end = len * BITS_PER_BYTE;

	while (pos < end) {
		unsigned long one = find_next_bit(bitmap, end, pos);

		if ((vb->resp_pos + 64) * sizeof(unsigned long) >
			 BALLOON_BMAP_SIZE)
			send_resp_data(vb, vq, busy_wait);
		if (one < end) {
			unsigned long pages, zero;

			zero = find_next_zero_bit(bitmap, end, one + 1);
			if (zero >= end)
				pages = end - one;
			else
				pages = zero - one;
			if (pages) {
				pages = do_set_resp_bitmap(vb, bitmap,
					 start_pfn, one, pages);
			}
			pos = one + pages;
		} else
			pos = one;
	}
}

static void tell_host(struct virtio_balloon *vb, struct virtqueue *vq)
{
	if (virtio_has_feature(vb->vdev, VIRTIO_BALLOON_F_PAGE_BITMAP)) {
		int nr_pfn, nr_used_bmap, i;
		unsigned long start_pfn, bmap_len;

		start_pfn = vb->start_pfn;
		nr_pfn = vb->end_pfn - start_pfn + 1;
		nr_pfn = roundup(nr_pfn, BITS_PER_LONG);
		nr_used_bmap = nr_pfn / PFNS_PER_BMAP;
		if (nr_pfn % PFNS_PER_BMAP)
			nr_used_bmap++;
		bmap_len = nr_pfn / BITS_PER_BYTE;

		for (i = 0; i < nr_used_bmap; i++) {
			unsigned int bmap_size = BALLOON_BMAP_SIZE;

			if (i + 1 == nr_used_bmap)
				bmap_size = bmap_len - BALLOON_BMAP_SIZE * i;
			set_bulk_pages(vb, vq, start_pfn + i * PFNS_PER_BMAP,
				 vb->page_bitmap[i], bmap_size, false);
		}
		if (vb->resp_pos > 0)
			send_resp_data(vb, vq, false);
	} else {
		struct scatterlist sg;
		unsigned int len;

		sg_init_one(&sg, vb->pfns, sizeof(vb->pfns[0]) * vb->num_pfns);

		/* We should always be able to add one buffer to an
		 * empty queue
		 */
		virtqueue_add_outbuf(vq, &sg, 1, vb, GFP_KERNEL);
		virtqueue_kick(vq);
		/* When host has read buffer, this completes via balloon_ack */
		wait_event(vb->acked, virtqueue_get_buf(vq, &len));
	}
}

static void set_page_pfns(struct virtio_balloon *vb,
			  __virtio32 pfns[], struct page *page)
{
	unsigned int i;

	/* Set balloon pfns pointing at this page.
	 * Note that the first pfn points at start of the page. */
	for (i = 0; i < VIRTIO_BALLOON_PAGES_PER_PAGE; i++)
		pfns[i] = cpu_to_virtio32(vb->vdev,
					  page_to_balloon_pfn(page) + i);
}

static void set_page_bitmap(struct virtio_balloon *vb,
			 struct list_head *pages, struct virtqueue *vq)
{
	unsigned long pfn, pfn_limit;
	struct page *page;
	bool found;
	int bmap_idx;

	vb->min_pfn = rounddown(vb->min_pfn, BITS_PER_LONG);
	vb->max_pfn = roundup(vb->max_pfn, BITS_PER_LONG);
	pfn_limit = PFNS_PER_BMAP * vb->nr_page_bmap;

	if (vb->nr_page_bmap == 1)
		extend_page_bitmap(vb, vb->max_pfn - vb->min_pfn + 1);
	for (pfn = vb->min_pfn; pfn < vb->max_pfn; pfn += pfn_limit) {
		unsigned long end_pfn;

		clear_page_bitmap(vb);
		vb->start_pfn = pfn;
		end_pfn = pfn;
		found = false;
		list_for_each_entry(page, pages, lru) {
			unsigned long pos, balloon_pfn;

			balloon_pfn = page_to_balloon_pfn(page);
			if (balloon_pfn < pfn || balloon_pfn >= pfn + pfn_limit)
				continue;
			bmap_idx = (balloon_pfn - pfn) / PFNS_PER_BMAP;
			pos = (balloon_pfn - pfn) % PFNS_PER_BMAP;
			set_bit(pos, vb->page_bitmap[bmap_idx]);
			if (balloon_pfn > end_pfn)
				end_pfn = balloon_pfn;
			found = true;
		}
		if (found) {
			vb->end_pfn = end_pfn;
			tell_host(vb, vq);
		}
	}
}

static unsigned int fill_balloon(struct virtio_balloon *vb, size_t num)
{
	struct balloon_dev_info *vb_dev_info = &vb->vb_dev_info;
	unsigned int num_allocated_pages;
	bool use_bmap = virtio_has_feature(vb->vdev,
				 VIRTIO_BALLOON_F_PAGE_BITMAP);

	if (use_bmap)
		init_bmap_pfn_range(vb);
	else
		/* We can only do one array worth at a time. */
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
		if (use_bmap)
			update_bmap_pfn_range(vb, page);
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
		if (use_bmap)
			set_page_bitmap(vb, &vb_dev_info->pages,
					vb->inflate_vq);
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

static unsigned int leak_balloon(struct virtio_balloon *vb, size_t num)
{
	unsigned int num_freed_pages;
	struct page *page;
	struct balloon_dev_info *vb_dev_info = &vb->vb_dev_info;
	LIST_HEAD(pages);
	bool use_bmap = virtio_has_feature(vb->vdev,
			 VIRTIO_BALLOON_F_PAGE_BITMAP);

	if (use_bmap)
		init_bmap_pfn_range(vb);
	else
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
		if (use_bmap)
			update_bmap_pfn_range(vb, page);
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
		if (use_bmap)
			set_page_bitmap(vb, &pages, vb->deflate_vq);
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

static void update_balloon_stats(struct virtio_balloon *vb)
{
	unsigned long events[NR_VM_EVENT_ITEMS];
	struct sysinfo i;
	int idx = 0;
	long available;

	all_vm_events(events);
	si_meminfo(&i);

	available = si_mem_available();

	update_stat(vb, idx++, VIRTIO_BALLOON_S_SWAP_IN,
				pages_to_bytes(events[PSWPIN]));
	update_stat(vb, idx++, VIRTIO_BALLOON_S_SWAP_OUT,
				pages_to_bytes(events[PSWPOUT]));
	update_stat(vb, idx++, VIRTIO_BALLOON_S_MAJFLT, events[PGMAJFAULT]);
	update_stat(vb, idx++, VIRTIO_BALLOON_S_MINFLT, events[PGFAULT]);
	update_stat(vb, idx++, VIRTIO_BALLOON_S_MEMFREE,
				pages_to_bytes(i.freeram));
	update_stat(vb, idx++, VIRTIO_BALLOON_S_MEMTOT,
				pages_to_bytes(i.totalram));
	update_stat(vb, idx++, VIRTIO_BALLOON_S_AVAIL,
				pages_to_bytes(available));
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
	unsigned int len;

	update_balloon_stats(vb);

	vq = vb->stats_vq;
	if (!virtqueue_get_buf(vq, &len))
		return;
	sg_init_one(&sg, vb->stats, sizeof(vb->stats));
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

static int init_vqs(struct virtio_balloon *vb)
{
	struct virtqueue *vqs[3];
	vq_callback_t *callbacks[] = { balloon_ack, balloon_ack, stats_request };
	static const char * const names[] = { "inflate", "deflate", "stats" };
	int err, nvqs;

	/*
	 * We expect two virtqueues: inflate and deflate, and
	 * optionally stat.
	 */
	nvqs = virtio_has_feature(vb->vdev, VIRTIO_BALLOON_F_STATS_VQ) ? 3 : 2;
	err = vb->vdev->config->find_vqs(vb->vdev, nvqs, vqs, callbacks, names);
	if (err)
		return err;

	vb->inflate_vq = vqs[0];
	vb->deflate_vq = vqs[1];
	if (virtio_has_feature(vb->vdev, VIRTIO_BALLOON_F_STATS_VQ)) {
		struct scatterlist sg;
		vb->stats_vq = vqs[2];

		/*
		 * Prime this virtqueue with one buffer so the hypervisor can
		 * use it to signal us later (it can't be broken yet!).
		 */
		sg_init_one(&sg, vb->stats, sizeof vb->stats);
		if (virtqueue_add_outbuf(vb->stats_vq, &sg, 1, vb, GFP_KERNEL)
		    < 0)
			BUG();
		virtqueue_kick(vb->stats_vq);
	}
	return 0;
}

#ifdef CONFIG_BALLOON_COMPACTION
static void tell_host_one_page(struct virtio_balloon *vb,
	struct virtqueue *vq, struct page *page)
{
	struct virtio_balloon_bmap_hdr *bmap_hdr;

	bmap_hdr = (struct virtio_balloon_bmap_hdr *)(vb->resp_data
							 + vb->resp_pos);
	bmap_hdr->head.start_pfn = page_to_pfn(page);
	bmap_hdr->head.page_shift = PAGE_SHIFT;
	bmap_hdr->head.bmap_len = 0;
	vb->resp_pos++;
	send_resp_data(vb, vq, false);
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
	unsigned long flags;
	bool use_bmap = virtio_has_feature(vb->vdev,
				 VIRTIO_BALLOON_F_PAGE_BITMAP);

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
	if (use_bmap)
		tell_host_one_page(vb, vb->inflate_vq, newpage);
	else {
		vb->num_pfns = VIRTIO_BALLOON_PAGES_PER_PAGE;
		set_page_pfns(vb, vb->pfns, newpage);
		tell_host(vb, vb->inflate_vq);
	}

	/* balloon's page migration 2nd step -- deflate "page" */
	balloon_page_delete(page);
	if (use_bmap)
		tell_host_one_page(vb, vb->deflate_vq, page);
	else {
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
	vb->resp_hdr = kzalloc(sizeof(struct virtio_balloon_resp_hdr),
				 GFP_KERNEL);
	/* Clear the feature bit if memory allocation fails */
	if (!vb->resp_hdr)
		__virtio_clear_bit(vdev, VIRTIO_BALLOON_F_PAGE_BITMAP);
	else {
		vb->page_bitmap[0] = kmalloc(BALLOON_BMAP_SIZE, GFP_KERNEL);
		if (!vb->page_bitmap[0]) {
			__virtio_clear_bit(vdev, VIRTIO_BALLOON_F_PAGE_BITMAP);
			kfree(vb->resp_hdr);
		} else {
			vb->nr_page_bmap = 1;
			vb->resp_data = kmalloc(BALLOON_BMAP_SIZE, GFP_KERNEL);
			if (!vb->resp_data) {
				__virtio_clear_bit(vdev,
						VIRTIO_BALLOON_F_PAGE_BITMAP);
				kfree(vb->page_bitmap[0]);
				kfree(vb->resp_hdr);
			}
		}
	}
	vb->resp_pos = 0;
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

	remove_common(vb);
	if (vb->vb_dev_info.inode)
		iput(vb->vb_dev_info.inode);
	kfree_page_bitmap(vb);
	kfree(vb->resp_hdr);
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
	VIRTIO_BALLOON_F_PAGE_BITMAP,
};

static struct virtio_driver virtio_balloon_driver = {
	.feature_table = features,
	.feature_table_size = ARRAY_SIZE(features),
	.driver.name =	KBUILD_MODNAME,
	.driver.owner =	THIS_MODULE,
	.id_table =	id_table,
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
