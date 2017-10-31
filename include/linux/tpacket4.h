/*
 *  tpacket v4
 *  Copyright(c) 2017 Intel Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 */

#ifndef _LINUX_TPACKET4_H
#define _LINUX_TPACKET4_H

#define TP4_UMEM_MIN_FRAME_SIZE 2048
#define TP4_KERNEL_HEADROOM 256 /* Headrom for XDP */

#define TP4A_FRAME_COMPLETED TP4_DESC_KERNEL

enum tp4_validation {
	TP4_VALIDATION_NONE,	/* No validation is performed */
	TP4_VALIDATION_IDX,	/* Only address to packet buffer is validated */
	TP4_VALIDATION_DESC	/* Full descriptor is validated */
};

struct tp4_umem {
	struct pid *pid;
	struct page **pgs;
	unsigned int npgs;
	size_t size;
	unsigned long address;
	unsigned int frame_size;
	unsigned int frame_size_log2;
	unsigned int nframes;
	unsigned int nfpplog2; /* num frames per page in log2 */
	unsigned int data_headroom;
};

struct tp4_dma_info {
	dma_addr_t dma;
	struct page *page;
};

struct tp4_queue {
	struct tpacket4_desc *ring;

	unsigned int used_idx;
	unsigned int last_avail_idx;
	unsigned int ring_mask;
	unsigned int num_free;

	struct tp4_umem *umem;
	struct tp4_dma_info *dma_info;
	enum dma_data_direction direction;
};

/**
 * struct tp4_packet_array - An array of packets/frames
 *
 * @tp4q: the tp4q associated with this packet array. Flushes and
 *	  populates will operate on this.
 * @dev: pointer to the netdevice the queue should be associated with
 * @direction: the direction of the DMA channel that is set up.
 * @validation: type of validation performed on populate
 * @start: the first packet that has not been processed
 * @curr: the packet that is currently being processed
 * @end: the last packet in the array
 * @mask: convenience variable for internal operations on the array
 * @items: the actual descriptors to frames/packets that are in the array
 **/
struct tp4_packet_array {
	struct tp4_queue *tp4q;
	struct device *dev;
	enum dma_data_direction direction;
	enum tp4_validation validation;
	u32 start;
	u32 curr;
	u32 end;
	u32 mask;
	struct tpacket4_desc items[0];
};

/**
 * struct tp4_frame_set - A view of a packet array consisting of
 *                        one or more frames
 *
 * @pkt_arr: the packet array this frame set is located in
 * @start: the first frame that has not been processed
 * @curr: the frame that is currently being processed
 * @end: the last frame in the frame set
 *
 * This frame set can either be one or more frames or a single packet
 * consisting of one or more frames. tp4f_ functions with packet in the
 * name return a frame set representing a packet, while the other
 * tp4f_ functions return one or more frames not taking into account if
 * they consitute a packet or not.
 **/
struct tp4_frame_set {
	struct tp4_packet_array *pkt_arr;
	u32 start;
	u32 curr;
	u32 end;
};

enum tp4_netdev_command {
	/* Enable the AF_PACKET V4 zerocopy support. When this is enabled,
	 * packets will arrive to the socket without being copied resulting
	 * in better performance. Note that this also means that no packets
	 * are sent to the kernel stack after this feature has been enabled.
	 */
	TP4_ENABLE,
	/* Disables the PACKET_ZEROCOPY support. */
	TP4_DISABLE,
};

/**
 * struct tp4_netdev_parms - TP4 netdev parameters for configuration
 *
 * @command: netdev command, currently enable or disable
 * @rx_opaque: an opaque pointer to the rx queue
 * @tx_opaque: an opaque pointer to the tx queue
 * @data_ready: function to be called when data is ready in poll mode
 * @data_ready_opauqe: opaque parameter returned with data_ready
 * @write_space: called when data needs to be transmitted in poll mode
 * @write_space_opaque: opaque parameter returned with write_space
 * @error_report: called when there is an error
 * @error_report_opaque: opaque parameter returned in error_report
 * @queue_pair: the queue_pair associated with this zero-copy operation
 **/
struct tp4_netdev_parms {
	enum tp4_netdev_command command;
	void *rx_opaque;
	void *tx_opaque;
	void (*data_ready)(void *);
	void *data_ready_opaque;
	void (*write_space)(void *);
	void *write_space_opaque;
	void (*error_report)(void *, int);
	void *error_report_opaque;
	int queue_pair;
};

/*************** V4 QUEUE OPERATIONS *******************************/

/**
 * tp4q_init - Initializas a tp4 queue
 *
 * @q: Pointer to the tp4 queue structure to be initialized
 * @nentries: Number of descriptor entries in the queue
 * @umem: Pointer to the umem / packet buffer associated with this queue
 * @buffer: Pointer to the memory region where the descriptors will reside
 **/
static inline void tp4q_init(struct tp4_queue *q, unsigned int nentries,
			     struct tp4_umem *umem,
			     struct tpacket4_desc *buffer)
{
	q->ring = buffer;
	q->used_idx = 0;
	q->last_avail_idx = 0;
	q->ring_mask = nentries - 1;
	q->num_free = 0;
	q->umem = umem;
}

/**
 * tp4q_umem_new - Creates a new umem (packet buffer)
 *
 * @addr: The address to the umem
 * @size: The size of the umem
 * @frame_size: The size of each frame, between 2K and PAGE_SIZE
 * @data_headroom: The desired data headroom before start of the packet
 *
 * Returns a pointer to the new umem or NULL for failure
 **/
static inline struct tp4_umem *tp4q_umem_new(unsigned long addr, size_t size,
					     unsigned int frame_size,
					     unsigned int data_headroom)
{
	struct tp4_umem *umem;
	unsigned int nframes;

	if (frame_size < TP4_UMEM_MIN_FRAME_SIZE || frame_size > PAGE_SIZE) {
		/* Strictly speaking we could support this, if:
		 * - huge pages, or*
		 * - using an IOMMU, or
		 * - making sure the memory area is consecutive
		 * but for now, we simply say "computer says no".
		 */
		return ERR_PTR(-EINVAL);
	}

	if (!is_power_of_2(frame_size))
		return ERR_PTR(-EINVAL);

	if (!PAGE_ALIGNED(addr)) {
		/* Memory area has to be page size aligned. For
		 * simplicity, this might change.
		 */
		return ERR_PTR(-EINVAL);
	}

	if ((addr + size) < addr)
		return ERR_PTR(-EINVAL);

	nframes = size / frame_size;
	if (nframes == 0)
		return ERR_PTR(-EINVAL);

	data_headroom =	ALIGN(data_headroom, 64);

	if (frame_size - data_headroom - TP4_KERNEL_HEADROOM < 0)
		return ERR_PTR(-EINVAL);

	umem = kzalloc(sizeof(*umem), GFP_KERNEL);
	if (!umem)
		return ERR_PTR(-ENOMEM);

	umem->pid = get_task_pid(current, PIDTYPE_PID);
	umem->size = size;
	umem->address = addr;
	umem->frame_size = frame_size;
	umem->frame_size_log2 = ilog2(frame_size);
	umem->nframes = nframes;
	umem->nfpplog2 = ilog2(PAGE_SIZE / frame_size);
	umem->data_headroom = data_headroom;

	return umem;
}

/**
 * tp4q_set_error - Sets an errno on the descriptor
 *
 * @desc: Pointer to the descriptor to be manipulated
 * @errno: The errno number to write to the descriptor
 **/
static inline void tp4q_set_error(struct tpacket4_desc *desc,
				  int errno)
{
	desc->error = errno;
}

/**
 * tp4q_set_offset - Sets the data offset for the descriptor
 *
 * @desc: Pointer to the descriptor to be manipulated
 * @offset: The data offset to write to the descriptor
 **/
static inline void tp4q_set_offset(struct tpacket4_desc *desc,
				   u16 offset)
{
	desc->offset = offset;
}

/**
 * tp4q_is_free - Is there a free entry on the queue?
 *
 * @q: Pointer to the tp4 queue to examine
 *
 * Returns true if there is a free entry, otherwise false
 **/
static inline int tp4q_is_free(struct tp4_queue *q)
{
	unsigned int idx = q->used_idx & q->ring_mask;
	unsigned int prev_idx;

	if (!idx)
		prev_idx = q->ring_mask;
	else
		prev_idx = idx - 1;

	/* previous frame is already consumed by userspace
	 * meaning ring is free
	 */
	if (q->ring[prev_idx].flags & TP4_DESC_KERNEL)
		return 1;

	/* there is some data that userspace can read immediately */
	return 0;
}

/**
 * tp4q_get_data_headroom - How much data headroom does the queue have
 *
 * @q: Pointer to the tp4 queue to examine
 *
 * Returns the amount of data headroom that has been configured for the
 * queue
 **/
static inline unsigned int tp4q_get_data_headroom(struct tp4_queue *q)
{
	return q->umem->data_headroom + TP4_KERNEL_HEADROOM;
}

/**
 * tp4q_is_valid_entry - Is the entry valid?
 *
 * @q: Pointer to the tp4 queue the descriptor resides in
 * @desc: Pointer to the descriptor to examine
 * @validation: The type of validation to perform
 *
 * Returns true if the entry is a valid, otherwise false
 **/
static inline bool tp4q_is_valid_entry(struct tp4_queue *q,
				       struct tpacket4_desc *d,
				       enum tp4_validation validation)
{
	if (validation == TP4_VALIDATION_NONE)
		return true;

	if (unlikely(d->idx >= q->umem->nframes)) {
		tp4q_set_error(d, EBADF);
		return false;
	}
	if (validation == TP4_VALIDATION_IDX) {
		tp4q_set_offset(d, tp4q_get_data_headroom(q));
		return true;
	}

	/* TP4_VALIDATION_DESC */
	if (unlikely(d->len > q->umem->frame_size ||
		     d->len == 0 ||
		     d->offset > q->umem->frame_size ||
		     d->offset + d->len > q->umem->frame_size)) {
		tp4q_set_error(d, EBADF);
		return false;
	}

	return true;
}

/**
 * tp4q_nb_avail - Returns the number of available entries
 *
 * @q: Pointer to the tp4 queue to examine
 * @dcnt: Max number of entries to check
 *
 * Returns the the number of entries available in the queue up to dcnt
 **/
static inline int tp4q_nb_avail(struct tp4_queue *q, int dcnt)
{
	unsigned int idx, last_avail_idx = q->last_avail_idx;
	int i, entries = 0;

	for (i = 0; i < dcnt; i++) {
		idx = (last_avail_idx++) & q->ring_mask;
		if (!(q->ring[idx].flags & TP4_DESC_KERNEL))
			break;
		entries++;
	}

	return entries;
}

/**
 * tp4q_enqueue - Enqueue entries to a tp4 queue
 *
 * @q: Pointer to the tp4 queue the descriptor resides in
 * @d: Pointer to the descriptor to examine
 * @dcnt: Max number of entries to dequeue
 *
 * Returns 0 for success or an errno at failure
 **/
static inline int tp4q_enqueue(struct tp4_queue *q,
			       const struct tpacket4_desc *d, int dcnt)
{
	unsigned int used_idx = q->used_idx;
	int i;

	if (q->num_free < dcnt)
		return -ENOSPC;

	q->num_free -= dcnt;

	for (i = 0; i < dcnt; i++) {
		unsigned int idx = (used_idx++) & q->ring_mask;

		q->ring[idx].idx = d[i].idx;
		q->ring[idx].len = d[i].len;
		q->ring[idx].offset = d[i].offset;
		q->ring[idx].error = d[i].error;
	}

	/* Order flags and data */
	smp_wmb();

	for (i = dcnt - 1; i >= 0; i--) {
		unsigned int idx = (q->used_idx + i) & q->ring_mask;

		q->ring[idx].flags = d[i].flags & ~TP4_DESC_KERNEL;
	}
	q->used_idx += dcnt;

	return 0;
}

/**
 * tp4q_enqueue_from_array - Enqueue entries from packet array to tp4 queue
 *
 * @a: Pointer to the packet array to enqueue from
 * @dcnt: Max number of entries to enqueue
 *
 * Returns 0 for success or an errno at failure
 **/
static inline int tp4q_enqueue_from_array(struct tp4_packet_array *a,
					  u32 dcnt)
{
	struct tp4_queue *q = a->tp4q;
	unsigned int used_idx = q->used_idx;
	struct tpacket4_desc *d = a->items;
	int i;

	if (q->num_free < dcnt)
		return -ENOSPC;

	q->num_free -= dcnt;

	for (i = 0; i < dcnt; i++) {
		unsigned int idx = (used_idx++) & q->ring_mask;
		unsigned int didx = (a->start + i) & a->mask;

		q->ring[idx].idx = d[didx].idx;
		q->ring[idx].len = d[didx].len;
		q->ring[idx].offset = d[didx].offset;
		q->ring[idx].error = d[didx].error;
	}

	/* Order flags and data */
	smp_wmb();

	for (i = dcnt - 1; i >= 0; i--) {
		unsigned int idx = (q->used_idx + i) & q->ring_mask;
		unsigned int didx = (a->start + i) & a->mask;

		q->ring[idx].flags = d[didx].flags & ~TP4_DESC_KERNEL;
	}
	q->used_idx += dcnt;

	return 0;
}

/**
 * tp4q_enqueue_completed_from_array - Enqueue only completed entries
 *				       from packet array
 *
 * @a: Pointer to the packet array to enqueue from
 * @dcnt: Max number of entries to enqueue
 *
 * Returns the number of entries successfully enqueued or a negative errno
 * at failure.
 **/
static inline int tp4q_enqueue_completed_from_array(struct tp4_packet_array *a,
						    u32 dcnt)
{
	struct tp4_queue *q = a->tp4q;
	unsigned int used_idx = q->used_idx;
	struct tpacket4_desc *d = a->items;
	int i, j;

	if (q->num_free < dcnt)
		return -ENOSPC;

	for (i = 0; i < dcnt; i++) {
		unsigned int didx = (a->start + i) & a->mask;

		if (d[didx].flags & TP4A_FRAME_COMPLETED) {
			unsigned int idx = (used_idx++) & q->ring_mask;

			q->ring[idx].idx = d[didx].idx;
			q->ring[idx].len = d[didx].len;
			q->ring[idx].offset = d[didx].offset;
			q->ring[idx].error = d[didx].error;
		} else {
			break;
		}
	}

	if (i == 0)
		return 0;

	/* Order flags and data */
	smp_wmb();

	for (j = i - 1; j >= 0; j--) {
		unsigned int idx = (q->used_idx + j) & q->ring_mask;
		unsigned int didx = (a->start + j) & a->mask;

		q->ring[idx].flags = d[didx].flags & ~TP4_DESC_KERNEL;
	}
	q->num_free -= i;
	q->used_idx += i;

	return i;
}

/**
 * tp4q_dequeue_to_array - Dequeue entries from tp4 queue to packet array
 *
 * @a: Pointer to the packet array to dequeue from
 * @dcnt: Max number of entries to dequeue
 *
 * Returns the number of entries dequeued. Non valid entries will be
 * discarded.
 **/
static inline int tp4q_dequeue_to_array(struct tp4_packet_array *a, u32 dcnt)
{
	struct tpacket4_desc *d = a->items;
	int i, entries, valid_entries = 0;
	struct tp4_queue *q = a->tp4q;
	u32 start = a->end;

	entries = tp4q_nb_avail(q, dcnt);
	q->num_free += entries;

	/* Order flags and data */
	smp_rmb();

	for (i = 0; i < entries; i++) {
		unsigned int d_idx = start & a->mask;
		unsigned int idx;

		idx = (q->last_avail_idx++) & q->ring_mask;
		d[d_idx] = q->ring[idx];
		if (!tp4q_is_valid_entry(q, &d[d_idx], a->validation)) {
			WARN_ON_ONCE(tp4q_enqueue(a->tp4q, &d[d_idx], 1));
			continue;
		}

		start++;
		valid_entries++;
	}
	return valid_entries;
}

/**
 * tp4q_disable - Disable a tp4 queue
 *
 * @dev: Pointer to the netdevice the queue is connected to
 * @q: Pointer to the tp4 queue to disable
 **/
static inline void tp4q_disable(struct device *dev,
				struct tp4_queue *q)
{
	int i;

	if (q->dma_info) {
		/* Unmap DMA */
		for (i = 0; i < q->umem->npgs; i++)
			dma_unmap_page(dev, q->dma_info[i].dma, PAGE_SIZE,
				       q->direction);

		kfree(q->dma_info);
		q->dma_info = NULL;
	}
}

/**
 * tp4q_enable - Enable a tp4 queue
 *
 * @dev: Pointer to the netdevice the queue should be associated with
 * @q: Pointer to the tp4 queue to enable
 * @direction: The direction of the DMA channel that is set up.
 *
 * Returns 0 for success or a negative errno for failure
 **/
static inline int tp4q_enable(struct device *dev,
			      struct tp4_queue *q,
			      enum dma_data_direction direction)
{
	int i, j;

	/* DMA map all the buffers in bufs up front, and sync prior
	 * kicking userspace. Is this sane? Strictly user land owns
	 * the buffer until they show up on the avail queue. However,
	 * mapping should be ok.
	 */
	if (direction != DMA_NONE) {
		q->dma_info = kcalloc(q->umem->npgs, sizeof(*q->dma_info),
				      GFP_KERNEL);
		if (!q->dma_info)
			return -ENOMEM;

		for (i = 0; i < q->umem->npgs; i++) {
			dma_addr_t dma;

			dma = dma_map_page(dev, q->umem->pgs[i], 0,
					   PAGE_SIZE, direction);
			if (dma_mapping_error(dev, dma)) {
				for (j = 0; j < i; j++)
					dma_unmap_page(dev,
						       q->dma_info[j].dma,
						       PAGE_SIZE, direction);
				kfree(q->dma_info);
				q->dma_info = NULL;
				return -EBUSY;
			}

			q->dma_info[i].page = q->umem->pgs[i];
			q->dma_info[i].dma = dma;
		}
	} else {
		q->dma_info = NULL;
	}

	q->direction = direction;
	return 0;
}

/**
 * tp4q_get_page_offset - Get offset into page frame resides at
 *
 * @q: Pointer to the tp4 queue that this frame resides in
 * @addr: Index of this frame in the packet buffer / umem
 * @pg: Returns a pointer to the page of this frame
 * @off: Returns the offset to the page of this frame
 **/
static inline void tp4q_get_page_offset(struct tp4_queue *q, u64 addr,
				       u64 *pg, u64 *off)
{
	*pg = addr >> q->umem->nfpplog2;
	*off = (addr - (*pg << q->umem->nfpplog2))
	       << q->umem->frame_size_log2;
}

/**
 * tp4q_max_data_size - Get the max packet size supported by a queue
 *
 * @q: Pointer to the tp4 queue to examine
 *
 * Returns the max packet size supported by the queue
 **/
static inline unsigned int tp4q_max_data_size(struct tp4_queue *q)
{
	return q->umem->frame_size - q->umem->data_headroom -
		TP4_KERNEL_HEADROOM;
}

/**
 * tp4q_get_data - Gets a pointer to the start of the packet
 *
 * @q: Pointer to the tp4 queue to examine
 * @desc: Pointer to descriptor of the packet
 *
 * Returns a pointer to the start of the packet the descriptor is pointing
 * to
 **/
static inline void *tp4q_get_data(struct tp4_queue *q,
				  struct tpacket4_desc *desc)
{
	u64 pg, off;
	u8 *pkt;

	tp4q_get_page_offset(q, desc->idx, &pg, &off);
	pkt = page_address(q->umem->pgs[pg]);
	return (u8 *)(pkt + off) + desc->offset;
}

/**
 * tp4q_get_dma_addr - Get kernel dma address of page
 *
 * @q: Pointer to the tp4 queue that this frame resides in
 * @pg: Pointer to the page of this frame
 *
 * Returns the dma address associated with the page
 **/
static inline dma_addr_t tp4q_get_dma_addr(struct tp4_queue *q, u64 pg)
{
	return q->dma_info[pg].dma;
}

/**
 * tp4q_get_desc - Get descriptor associated with frame
 *
 * @p: Pointer to the packet to examine
 *
 * Returns the descriptor of the current frame of packet p
 **/
static inline struct tpacket4_desc *tp4q_get_desc(struct tp4_frame_set *p)
{
	return &p->pkt_arr->items[p->curr & p->pkt_arr->mask];
}

/*************** FRAME OPERATIONS *******************************/
/* A frame is always just one frame of size frame_size.
 * A frame set is one or more frames.
 **/

/**
 * tp4f_reset - Start to traverse the frames in the set from the beginning
 * @p: pointer to frame set
 **/
static inline void tp4f_reset(struct tp4_frame_set *p)
{
	p->curr = p->start;
}

/**
 * tp4f_next_frame - Go to next frame in frame set
 * @p: pointer to frame set
 *
 * Returns true if there is another frame in the frame set.
 * Advances curr pointer.
 **/
static inline bool tp4f_next_frame(struct tp4_frame_set *p)
{
	if (p->curr + 1 == p->end)
		return false;

	p->curr++;
	return true;
}

/**
 * tp4f_get_frame_id - Get packet buffer id of frame
 * @p: pointer to frame set
 *
 * Returns the id of the packet buffer of the current frame
 **/
static inline u64 tp4f_get_frame_id(struct tp4_frame_set *p)
{
	return p->pkt_arr->items[p->curr & p->pkt_arr->mask].idx;
}

/**
 * tp4f_get_frame_len - Get length of data in current frame
 * @p: pointer to frame set
 *
 * Returns the length of data in the packet buffer of the current frame
 **/
static inline u32 tp4f_get_frame_len(struct tp4_frame_set *p)
{
	return p->pkt_arr->items[p->curr & p->pkt_arr->mask].len;
}

/**
 * tp4f_get_data_offset - Get offset of packet data in packet buffer
 * @p: pointer to frame set
 *
 * Returns the offset to the data in the packet buffer of the current
 * frame
 **/
static inline u32 tp4f_get_data_offset(struct tp4_frame_set *p)
{
	return p->pkt_arr->items[p->curr & p->pkt_arr->mask].offset;
}

/**
 * tp4f_set_error - Set an error on the current frame
 * @p: pointer to frame set
 * @errno: the errno to be assigned
 **/
static inline void tp4f_set_error(struct tp4_frame_set *p, int errno)
{
	p->pkt_arr->items[p->curr & p->pkt_arr->mask].error = errno;
}

/**
 * tp4f_is_last_frame - Is this the last frame of the frame set
 * @p: pointer to frame set
 *
 * Returns true if this is the last frame of the frame set, otherwise 0
 **/
static inline bool tp4f_is_last_frame(struct tp4_frame_set *p)
{
	return p->curr + 1 == p->end;
}

/**
 * tp4f_num_frames - Number of frames in a frame set
 * @p: pointer to frame set
 *
 * Returns the number of frames this frame set consists of
 **/
static inline u32 tp4f_num_frames(struct tp4_frame_set *p)
{
	return p->end - p->start;
}

/**
 * tp4f_get_data - Gets a pointer to the frame the frame set is on
 * @p: pointer to the frame set
 *
 * Returns a pointer to the data of the frame that the frame set is
 * pointing to. Note that there might be configured headroom before this
 **/
static inline void *tp4f_get_data(struct tp4_frame_set *p)
{
	return tp4q_get_data(p->pkt_arr->tp4q, tp4q_get_desc(p));
}

/**
 * tp4f_set_frame - Sets the properties of a frame
 * @p: pointer to frame
 * @len: the length in bytes of the data in the frame
 * @offset: offset to start of data in frame
 * @is_eop: Set if this is the last frame of the packet
 **/
static inline void tp4f_set_frame(struct tp4_frame_set *p, u32 len, u16 offset,
				  bool is_eop)
{
	struct tpacket4_desc *d =
		&p->pkt_arr->items[p->curr & p->pkt_arr->mask];

	d->len = len;
	d->offset = offset;
	if (!is_eop)
		d->flags |= TP4_PKT_CONT;
}

/**
 * tp4f_set_frame_no_offset - Sets the properties of a frame
 * @p: pointer to frame
 * @len: the length in bytes of the data in the frame
 * @is_eop: Set if this is the last frame of the packet
 **/
static inline void tp4f_set_frame_no_offset(struct tp4_frame_set *p,
					    u32 len, bool is_eop)
{
	struct tpacket4_desc *d =
		&p->pkt_arr->items[p->curr & p->pkt_arr->mask];

	d->len = len;
	if (!is_eop)
		d->flags |= TP4_PKT_CONT;
}

/**
 * tp4f_get_dma - Returns DMA address of the frame
 * @f: pointer to frame
 *
 * Returns the DMA address of the frame
 **/
static inline dma_addr_t tp4f_get_dma(struct tp4_frame_set *f)
{
	struct tp4_queue *tp4q = f->pkt_arr->tp4q;
	dma_addr_t dma;
	u64 pg, off;

	tp4q_get_page_offset(tp4q, tp4f_get_frame_id(f), &pg, &off);
	dma = tp4q_get_dma_addr(tp4q, pg);

	return dma + off + tp4f_get_data_offset(f);
}

/*************** PACKET OPERATIONS *******************************/
/* A packet consists of one or more frames. Both frames and packets
 * are represented by a tp4_frame_set. The only difference is that
 * packet functions look at the EOP flag.
 **/

/**
 * tp4f_get_packet_len - Length of packet
 * @p: pointer to packet
 *
 * Returns the length of the packet in bytes.
 * Resets curr pointer of packet.
 **/
static inline u32 tp4f_get_packet_len(struct tp4_frame_set *p)
{
	u32 len = 0;

	tp4f_reset(p);

	do {
		len += tp4f_get_frame_len(p);
	} while (tp4f_next_frame(p));

	return len;
}

/**
 * tp4f_packet_completed - Mark packet as completed
 * @p: pointer to packet
 *
 * Resets curr pointer of packet.
 **/
static inline void tp4f_packet_completed(struct tp4_frame_set *p)
{
	tp4f_reset(p);

	do {
		p->pkt_arr->items[p->curr & p->pkt_arr->mask].flags |=
			TP4A_FRAME_COMPLETED;
	} while (tp4f_next_frame(p));
}

/**************** PACKET_ARRAY FUNCTIONS ********************************/

static inline struct tp4_packet_array *__tp4a_new(
	struct tp4_queue *tp4q,
	struct device *dev,
	enum dma_data_direction direction,
	enum tp4_validation validation,
	size_t elems)
{
	struct tp4_packet_array *arr;
	int err;

	if (!is_power_of_2(elems))
		return NULL;

	arr = kzalloc(sizeof(*arr) + elems * sizeof(struct tpacket4_desc),
		      GFP_KERNEL);
	if (!arr)
		return NULL;

	err = tp4q_enable(dev, tp4q, direction);
	if (err) {
		kfree(arr);
		return NULL;
	}

	arr->tp4q = tp4q;
	arr->dev = dev;
	arr->direction = direction;
	arr->validation = validation;
	arr->mask = elems - 1;
	return arr;
}

/**
 * tp4a_rx_new - Create new packet array for ingress
 * @rx_opaque: opaque from tp4_netdev_params
 * @elems: number of elements in the packet array
 * @dev: device or NULL
 *
 * Returns a reference to the new packet array or NULL for failure
 **/
static inline struct tp4_packet_array *tp4a_rx_new(void *rx_opaque,
						   size_t elems,
						   struct device *dev)
{
	enum dma_data_direction direction = dev ? DMA_FROM_DEVICE : DMA_NONE;

	return __tp4a_new(rx_opaque, dev, direction, TP4_VALIDATION_IDX,
			  elems);
}

/**
 * tp4a_tx_new - Create new packet array for egress
 * @tx_opaque: opaque from tp4_netdev_params
 * @elems: number of elements in the packet array
 * @dev: device or NULL
 *
 * Returns a reference to the new packet array or NULL for failure
 **/
static inline struct tp4_packet_array *tp4a_tx_new(void *tx_opaque,
						   size_t elems,
						   struct device *dev)
{
	enum dma_data_direction direction = dev ? DMA_TO_DEVICE : DMA_NONE;

	return __tp4a_new(tx_opaque, dev, direction, TP4_VALIDATION_DESC,
			  elems);
}

/**
 * tp4a_get_flushable_frame_set - Create a frame set of the flushable region
 * @a: pointer to packet array
 * @p: frame set
 *
 * Returns true for success and false for failure
 **/
static inline bool tp4a_get_flushable_frame_set(struct tp4_packet_array *a,
						struct tp4_frame_set *p)
{
	u32 avail = a->curr - a->start;

	if (avail == 0)
		return false; /* empty */

	p->pkt_arr = a;
	p->start = a->start;
	p->curr = a->start;
	p->end = a->curr;

	return true;
}

/**
 * tp4a_next_frame - Get next frame in array and advance curr pointer
 * @a: pointer to packet array
 * @p: supplied pointer to packet structure that is filled in by function
 *
 * Returns true if there is a frame, false otherwise. Frame returned in *p.
 **/
static inline bool tp4a_next_frame(struct tp4_packet_array *a,
				   struct tp4_frame_set *p)
{
	u32 avail = a->end - a->curr;

	if (avail == 0)
		return false; /* empty */

	p->pkt_arr = a;
	p->start = a->curr;
	p->curr = a->curr;
	p->end = ++a->curr;

	return true;
}

/**
 * tp4a_flush - Flush processed packets to associated tp4q
 * @a: pointer to packet array
 *
 * Returns 0 for success and -1 for failure
 **/
static inline int tp4a_flush(struct tp4_packet_array *a)
{
	u32 avail = a->curr - a->start;
	int ret;

	if (avail == 0)
		return 0; /* nothing to flush */

	ret = tp4q_enqueue_from_array(a, avail);
	if (ret < 0)
		return -1;

	a->start = a->curr;

	return 0;
}

/**
 * tp4a_free - Destroy packet array
 * @a: pointer to packet array
 **/
static inline void tp4a_free(struct tp4_packet_array *a)
{
	struct tp4_frame_set f;

	if (a) {
		/* Flush all outstanding requests. */
		if (tp4a_get_flushable_frame_set(a, &f)) {
			do {
				tp4f_set_frame(&f, 0, 0, true);
			} while (tp4f_next_frame(&f));
		}

		WARN_ON_ONCE(tp4a_flush(a));

		tp4q_disable(a->dev, a->tp4q);
	}

	kfree(a);
}

/**
 * tp4a_get_data_headroom - Returns the data headroom configured for the array
 * @a: pointer to packet array
 *
 * Returns the data headroom configured for the array
 **/
static inline unsigned int tp4a_get_data_headroom(struct tp4_packet_array *a)
{
	return tp4q_get_data_headroom(a->tp4q);
}

/**
 * tp4a_max_data_size - Get the max packet size supported for the array
 * @a: pointer to packet array
 *
 * Returns the maximum size of data that can be put in a frame when headroom
 * has been accounted for.
 **/
static inline unsigned int tp4a_max_data_size(struct tp4_packet_array *a)
{
	return tp4q_max_data_size(a->tp4q);

}

/**
 * tp4a_has_same_umem - Checks if two packet arrays have the same umem
 * @a1: pointer to packet array
 * @a2: pointer to packet array
 *
 * Returns true if arrays have the same umem, false otherwise
 **/
static inline bool tp4a_has_same_umem(struct tp4_packet_array *a1,
				      struct tp4_packet_array *a2)
{
	return (a1->tp4q->umem == a2->tp4q->umem) ? true : false;
}

/**
 * tp4a_next_packet - Get next packet in array and advance curr pointer
 * @a: pointer to packet array
 * @p: supplied pointer to packet structure that is filled in by function
 *
 * Returns true if there is a packet, false otherwise. Packet returned in *p.
 **/
static inline bool tp4a_next_packet(struct tp4_packet_array *a,
				    struct tp4_frame_set *p)
{
	u32 avail = a->end - a->curr;

	if (avail == 0)
		return false; /* empty */

	p->pkt_arr = a;
	p->start = a->curr;
	p->curr = a->curr;
	p->end = a->curr;

	/* XXX Sanity check for too-many-frames packets? */
	while (a->items[p->end++ & a->mask].flags & TP4_PKT_CONT) {
		avail--;
		if (avail == 0)
			return false;
	}

	a->curr += (p->end - p->start);
	return true;
}

/**
 * tp4a_flush_n - Flush n processed packets to associated tp4q
 * @a: pointer to packet array
 * @n: number of items to flush
 *
 * Returns 0 for success and -1 for failure
 **/
static inline int tp4a_flush_n(struct tp4_packet_array *a, unsigned int n)
{
	u32 avail = a->curr - a->start;
	int ret;

	if (avail == 0 || n == 0)
		return 0; /* nothing to flush */

	avail = (n > avail) ? avail : n; /* XXX trust user? remove? */

	ret = tp4q_enqueue_from_array(a, avail);
	if (ret < 0)
		return -1;

	a->start += avail;
	return 0;
}

/**
 * tp4a_flush_completed - Flushes only frames marked as completed
 * @a: pointer to packet array
 *
 * Returns 0 for success and -1 for failure
 **/
static inline int tp4a_flush_completed(struct tp4_packet_array *a)
{
	u32 avail = a->curr - a->start;
	int ret;

	if (avail == 0)
		return 0; /* nothing to flush */

	ret = tp4q_enqueue_completed_from_array(a, avail);
	if (ret < 0)
		return -1;

	a->start += ret;
	return 0;
}

/**
 * tp4a_populate - Populate an array with packets from associated tp4q
 * @a: pointer to packet array
 **/
static inline void tp4a_populate(struct tp4_packet_array *a)
{
	u32 cnt, free = a->mask + 1 - (a->end - a->start);

	if (free == 0)
		return; /* no space! */

	cnt = tp4q_dequeue_to_array(a, free);
	a->end += cnt;
}

/**
 * tp4a_next_frame_populate - Get next frame and populate array if empty
 * @a: pointer to packet array
 * @p: supplied pointer to packet structure that is filled in by function
 *
 * Returns true if there is a frame, false otherwise. Frame returned in *p.
 **/
static inline bool tp4a_next_frame_populate(struct tp4_packet_array *a,
					    struct tp4_frame_set *p)
{
	bool more_frames;

	more_frames = tp4a_next_frame(a, p);
	if (!more_frames) {
		tp4a_populate(a);
		more_frames = tp4a_next_frame(a, p);
	}

	return more_frames;
}

/**
 * tp4a_add_packet - Adds a packet into a packet array without copying data
 * @a: pointer to packet array to insert the packet into
 * @pkt: pointer to packet to insert
 * @len: returns the length in bytes of data added according to descriptor
 *
 * Note that this function does not copy the data. Instead it copies
 * the address that points to the packet buffer.
 *
 * Returns 0 for success and -1 for failure
 **/
static inline int tp4a_add_packet(struct tp4_packet_array *a,
				  struct tp4_frame_set *p, u32 *len)
{
	u32 free = a->end - a->curr;
	u32 nframes = p->end - p->start;

	if (nframes > free)
		return -1;

	tp4f_reset(p);
	*len = 0;

	do {
		int frame_len = tp4f_get_frame_len(p);
		int idx = a->curr & a->mask;

		a->items[idx].idx = tp4f_get_frame_id(p);
		a->items[idx].len = frame_len;
		a->items[idx].offset = tp4f_get_data_offset(p);
		a->items[idx].flags = tp4f_is_last_frame(p) ?
						   0 : TP4_PKT_CONT;
		a->items[idx].error = 0;

		a->curr++;
		*len += frame_len;
	} while (tp4f_next_frame(p));

	return 0;
}

/**
 * tp4a_copy_packet - Copies a packet with data into a packet array
 * @a: pointer to packet array to insert the packet into
 * @pkt: pointer to packet to insert and copy
 * @len: returns the length in bytes of data copied
 *
 * Puts the packet where curr is pointing
 *
 * Returns 0 for success and -1 for failure
 **/
static inline int tp4a_copy_packet(struct tp4_packet_array *a,
				   struct tp4_frame_set *p, int *len)
{
	u32 free = a->end - a->curr;
	u32 nframes = p->end - p->start;

	if (nframes > free)
		return -1;

	tp4f_reset(p);
	*len = 0;

	do {
		int frame_len = tp4f_get_frame_len(p);
		int idx = a->curr & a->mask;

		a->items[idx].len = frame_len;
		a->items[idx].offset = tp4f_get_data_offset(p);
		a->items[idx].flags = tp4f_is_last_frame(p) ?
						   0 : TP4_PKT_CONT;
		a->items[idx].error = 0;

		memcpy(tp4q_get_data(a->tp4q, &a->items[idx]),
		       tp4f_get_data(p), frame_len);
		a->curr++;
		*len += frame_len;
	} while (tp4f_next_frame(p));

	return 0;
}

/**
 * tp4a_copy - Copy a packet array
 * @dst: pointer to destination packet array
 * @src: pointer to source packet array
 * @len: returns the length in bytes of all packets copied
 *
 * Returns number of packets copied
 **/
static inline int tp4a_copy(struct tp4_packet_array *dst,
			    struct tp4_packet_array *src, int *len)
{
	int npackets = 0;

	*len = 0;
	for (;;) {
		struct tp4_frame_set src_pkt;
		int pkt_len;

		if (!tp4a_next_packet(src, &src_pkt))
			break;

		if (tp4a_has_same_umem(src, dst)) {
			if (tp4a_add_packet(dst, &src_pkt, &pkt_len))
				break;
		} else {
			if (tp4a_copy_packet(dst, &src_pkt, &pkt_len))
				break;
		}

		npackets++;
		*len += pkt_len;
	}

	return npackets;
}

/**
 * tp4a_return_packet - Return packet to the packet array
 *
 * @a: pointer to packet array
 * @p: pointer to the packet to return
 **/
static inline void tp4a_return_packet(struct tp4_packet_array *a,
				      struct tp4_frame_set *p)
{
	a->curr = p->start;
}

#endif /* _LINUX_TPACKET4_H */
