/*
 *
 * Copyright (c) 2009, Microsoft Corporation.
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
 * this program; if not, write to the Free Software Foundation, Inc., 59 Temple
 * Place - Suite 330, Boston, MA 02111-1307 USA.
 *
 * Authors:
 *   Haiyang Zhang <haiyangz@microsoft.com>
 *   Hank Janssen  <hjanssen@microsoft.com>
 *   K. Y. Srinivasan <kys@microsoft.com>
 *
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/hyperv.h>
#include <linux/uio.h>
#include <linux/vmalloc.h>
#include <linux/slab.h>

#include "hyperv_vmbus.h"

#define VMBUS_PKT_TRAILER	8

/*
 * When we write to the ring buffer, check if the host needs to
 * be signaled. Here is the details of this protocol:
 *
 *	1. The host guarantees that while it is draining the
 *	   ring buffer, it will set the interrupt_mask to
 *	   indicate it does not need to be interrupted when
 *	   new data is placed.
 *
 *	2. The host guarantees that it will completely drain
 *	   the ring buffer before exiting the read loop. Further,
 *	   once the ring buffer is empty, it will clear the
 *	   interrupt_mask and re-check to see if new data has
 *	   arrived.
 *
 * KYS: Oct. 30, 2016:
 * It looks like Windows hosts have logic to deal with DOS attacks that
 * can be triggered if it receives interrupts when it is not expecting
 * the interrupt. The host expects interrupts only when the ring
 * transitions from empty to non-empty (or full to non full on the guest
 * to host ring).
 * So, base the signaling decision solely on the ring state until the
 * host logic is fixed.
 */

static void hv_signal_on_write(u32 old_write, struct vmbus_channel *channel)
{
	struct hv_ring_buffer_info *rbi = &channel->outbound;

	virt_mb();
	if (READ_ONCE(rbi->ring_buffer->interrupt_mask))
		return;

	/* check interrupt_mask before read_index */
	virt_rmb();
	/*
	 * This is the only case we need to signal when the
	 * ring transitions from being empty to non-empty.
	 */
	if (old_write == READ_ONCE(rbi->ring_buffer->read_index))
		vmbus_setevent(channel);
}

/* Get various debug metrics for the specified ring buffer. */
void hv_ringbuffer_get_debuginfo(const struct hv_ring_buffer_info *ring_info,
				 struct hv_ring_buffer_debug_info *debug_info)
{
	u32 bytes_avail_towrite;
	u32 bytes_avail_toread;

	if (ring_info->ring_buffer) {
		hv_get_ringbuffer_availbytes(ring_info,
					&bytes_avail_toread,
					&bytes_avail_towrite);

		debug_info->bytes_avail_toread = bytes_avail_toread;
		debug_info->bytes_avail_towrite = bytes_avail_towrite;
		debug_info->current_read_index =
			ring_info->ring_buffer->read_index;
		debug_info->current_write_index =
			ring_info->ring_buffer->write_index;
		debug_info->current_interrupt_mask =
			ring_info->ring_buffer->interrupt_mask;
	}
}
EXPORT_SYMBOL_GPL(hv_ringbuffer_get_debuginfo);

/* Initialize the ring buffer. */
int hv_ringbuffer_init(struct hv_ring_buffer_info *ring_info,
		       struct page *pages, u32 page_cnt)
{
	int i;
	struct page **pages_wraparound;

	BUILD_BUG_ON((sizeof(struct hv_ring_buffer) != PAGE_SIZE));

	memset(ring_info, 0, sizeof(struct hv_ring_buffer_info));

	/*
	 * First page holds struct hv_ring_buffer, do wraparound mapping for
	 * the rest.
	 */
	pages_wraparound = kzalloc(sizeof(struct page *) * (page_cnt * 2 - 1),
				   GFP_KERNEL);
	if (!pages_wraparound)
		return -ENOMEM;

	pages_wraparound[0] = pages;
	for (i = 0; i < 2 * (page_cnt - 1); i++)
		pages_wraparound[i + 1] = &pages[i % (page_cnt - 1) + 1];

	ring_info->ring_buffer = (struct hv_ring_buffer *)
		vmap(pages_wraparound, page_cnt * 2 - 1, VM_MAP, PAGE_KERNEL);

	kfree(pages_wraparound);


	if (!ring_info->ring_buffer)
		return -ENOMEM;

	ring_info->ring_buffer->read_index =
		ring_info->ring_buffer->write_index = 0;

	/* Set the feature bit for enabling flow control. */
	ring_info->ring_buffer->feature_bits.value = 1;

	ring_info->ring_size = page_cnt << PAGE_SHIFT;
	ring_info->ring_datasize = ring_info->ring_size -
		sizeof(struct hv_ring_buffer);

	return 0;
}

/* Cleanup the ring buffer. */
void hv_ringbuffer_cleanup(struct hv_ring_buffer_info *ring_info)
{
	vunmap(ring_info->ring_buffer);
}

/*
 * Multiple producer lock-free ring buffer write
 *
 * There are two write locations: when no CPU is writing to ring both
 * are equal.
 *     ring_buffer_info->priv_write_index - next writer's tail offset
 *     ring_buffer->write_index - reader's tail offset
 *
 * The write goes through three stages:
 *  1. Reserve space in ring buffer for the new data.
 *     Writer atomically moves priv_write_index.
 *  2. Copy the new data into the ring.
 *  3. Update the tail of the ring (visible to host) that indicates
 *  next read location. Writer updates write_index
 *
 * This function can be safely called from softirq.
 */
int hv_ringbuffer_write(struct vmbus_channel *channel,
			const struct kvec *kv_list, u32 kv_count)
{
	struct hv_ring_buffer_info *outring = &channel->outbound;
	const u32 ring_size = outring->ring_datasize;
	void *ring_buffer = hv_get_ring_buffer(outring);
	u32 write_offset, totalbytes;
	u32 prev_location, next_write_location, write_location;
	unsigned long flags;
	u64 *prev_indices;
	unsigned int i;

	if (unlikely(channel->rescind))
		return -ENODEV;

	/* Compute total size of the request write */
	totalbytes = sizeof(u64);
	for (i = 0; i < kv_count; i++)
		totalbytes += kv_list[i].iov_len;

	/* Don't want to allow softirq to race with this. */
	local_irq_save(flags);

	/* Reserve space in ring */
	do {
		u32 read_location = READ_ONCE(outring->ring_buffer->read_index);
		u32 bytes_avail;

		write_location = READ_ONCE(outring->priv_write_index);

		bytes_avail = (write_location >= read_location)
			? ring_size - (write_location - read_location)
			: read_location - write_location;
		/*
		 * If insufficient space exists at this time
		 * it is up to caller to retry
		 */
		if (bytes_avail <= totalbytes) {
			local_irq_restore(flags);
			return -EAGAIN;
		}

		/* If device is being hot-removed fail. */
		if (unlikely(channel->rescind)) {
			local_irq_restore(flags);
			return -ENODEV;
		}

		next_write_location = write_location + totalbytes;
		if (next_write_location >= ring_size)
			next_write_location -= ring_size;

		/* Atomic update update the next write_index */
		prev_location = cmpxchg(&outring->priv_write_index,
					write_location, next_write_location);

		/* Loop until our update wins */
	} while (prev_location != write_location);

	/* Copy new data into place */
	write_offset = write_location;
	for (i = 0; i < kv_count; i++) {
		memcpy(ring_buffer + write_offset,
		       kv_list[i].iov_base, kv_list[i].iov_len);

		write_offset += kv_list[i].iov_len;
	}

	/* Set previous packet start */
	prev_indices = ring_buffer + write_offset;
	*prev_indices = (u64)write_location << 32;

	/* Issue a full memory barrier before updating the write index */
	virt_mb();

	/* Check in for our reservation. Wait for our turn to update host */
	while (cmpxchg(&outring->ring_buffer->write_index,
		       write_location, next_write_location)
	       != write_location) {
		cpu_relax();

		if (unlikely(channel->rescind)) {
			local_irq_restore(flags);
			return -ENODEV;
		}
	}

	hv_signal_on_write(write_location, channel);
	local_irq_restore(flags);

	return 0;
}

int hv_ringbuffer_read(struct vmbus_channel *channel,
		       void *buffer, u32 buflen, u32 *buffer_actual_len,
		       u64 *requestid, bool raw)
{
	struct vmpacket_descriptor *desc;
	u32 packetlen, offset;

	if (unlikely(buflen == 0))
		return -EINVAL;

	*buffer_actual_len = 0;
	*requestid = 0;

	/* Make sure there is something to read */
	desc = hv_pkt_iter_first(channel);
	if (desc == NULL) {
		/*
		 * No error is set when there is even no header, drivers are
		 * supposed to analyze buffer_actual_len.
		 */
		return 0;
	}

	offset = raw ? 0 : (desc->offset8 << 3);
	packetlen = (desc->len8 << 3) - offset;
	*buffer_actual_len = packetlen;
	*requestid = desc->trans_id;

	if (unlikely(packetlen > buflen))
		return -ENOBUFS;

	/* since ring is double mapped, only one copy is necessary */
	memcpy(buffer, (const char *)desc + offset, packetlen);

	/* Advance ring index to next packet descriptor */
	__hv_pkt_iter_next(channel, desc);

	/* Notify host of update */
	hv_pkt_iter_close(channel);

	return 0;
}

/*
 * Determine number of bytes available in ring buffer after
 * the current iterator (priv_read_index) location.
 *
 * This is similar to hv_get_bytes_to_read but with private
 * read index instead.
 */
static u32 hv_pkt_iter_avail(const struct hv_ring_buffer_info *rbi)
{
	u32 priv_read_loc = rbi->priv_read_index;
	u32 write_loc = READ_ONCE(rbi->ring_buffer->write_index);

	if (write_loc >= priv_read_loc)
		return write_loc - priv_read_loc;
	else
		return (rbi->ring_datasize - priv_read_loc) + write_loc;
}

/*
 * Get first vmbus packet from ring buffer after read_index
 *
 * If ring buffer is empty, returns NULL and no other action needed.
 */
struct vmpacket_descriptor *hv_pkt_iter_first(struct vmbus_channel *channel)
{
	struct hv_ring_buffer_info *rbi = &channel->inbound;

	/* set state for later hv_signal_on_read() */
	rbi->cached_read_index = rbi->ring_buffer->read_index;

	if (hv_pkt_iter_avail(rbi) < sizeof(struct vmpacket_descriptor))
		return NULL;

	return hv_get_ring_buffer(rbi) + rbi->priv_read_index;
}
EXPORT_SYMBOL_GPL(hv_pkt_iter_first);

/*
 * Get next vmbus packet from ring buffer.
 *
 * Advances the current location (priv_read_index) and checks for more
 * data. If the end of the ring buffer is reached, then return NULL.
 */
struct vmpacket_descriptor *
__hv_pkt_iter_next(struct vmbus_channel *channel,
		   const struct vmpacket_descriptor *desc)
{
	struct hv_ring_buffer_info *rbi = &channel->inbound;
	u32 packetlen = desc->len8 << 3;
	u32 dsize = rbi->ring_datasize;

	/* bump offset to next potential packet */
	rbi->priv_read_index += packetlen + VMBUS_PKT_TRAILER;
	if (rbi->priv_read_index >= dsize)
		rbi->priv_read_index -= dsize;

	/* more data? */
	if (hv_pkt_iter_avail(rbi) < sizeof(struct vmpacket_descriptor))
		return NULL;
	else
		return hv_get_ring_buffer(rbi) + rbi->priv_read_index;
}
EXPORT_SYMBOL_GPL(__hv_pkt_iter_next);

/*
 * Update host ring buffer after iterating over packets.
 */
void hv_pkt_iter_close(struct vmbus_channel *channel)
{
	struct hv_ring_buffer_info *rbi = &channel->inbound;

	/*
	 * Make sure all reads are done before we update the read index since
	 * the writer may start writing to the read area once the read index
	 * is updated.
	 */
	virt_rmb();
	rbi->ring_buffer->read_index = rbi->priv_read_index;

	hv_signal_on_read(channel);
}
EXPORT_SYMBOL_GPL(hv_pkt_iter_close);
