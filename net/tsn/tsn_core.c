/*
 *   TSN Core main part of TSN driver
 *
 *   Copyright (C) 2015- Henrik Austad <haustad@cisco.com>
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 */

#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/random.h>
#include <linux/rtmutex.h>
#include <linux/hashtable.h>
#include <linux/netdevice.h>
#include <linux/net.h>
#include <linux/dma-mapping.h>
#include <net/sock.h>
#include <net/net_namespace.h>
#include <linux/hrtimer.h>
#include <linux/configfs.h>

#define CREATE_TRACE_POINTS
#include <trace/events/tsn.h>
#include "tsn_internal.h"

static struct tsn_list tlist;
static int in_debug;
static int on_cpu = -1;

#define TLINK_HASH_BITS 8
DEFINE_HASHTABLE(tlinks, TLINK_HASH_BITS);

static LIST_HEAD(tsn_shim_ops);

/* Called with link->lock held */
static inline size_t _get_low_water(struct tsn_link *link)
{
	/* use max_payload_size and give a rough estimate of how many
	 * bytes that would be for low_water_ms
	 */
	int low_water_ms = 20;
	int numframes = low_water_ms * 8;

	if (link->class_a)
		numframes *= 2;
	return link->max_payload_size * numframes;
}

/* Called with link->lock held */
static inline size_t _get_high_water(struct tsn_link *link)
{
	size_t low_water =  _get_low_water(link);

	return max(link->used_buffer_size - low_water, low_water);
}

/**
 * _tsn_set_buffer - register a memory region to use as the buffer
 *
 * This is used when we are operating in !external_buffer mode.
 *
 * TSN expects a ring-buffer and will update pointers to keep track of
 * where we are. When the buffer is refilled, head and tail will be
 * updated accordingly.
 *
 * @param link		the link that should hold the buffer
 * @param buffer	the new buffer
 * @param bufsize	size of new buffer.
 *
 * @returns 0 on success, negative on error
 *
 * Must be called with tsn_lock() held.
 */
static int _tsn_set_buffer(struct tsn_link *link, void *buffer, size_t bufsize)
{
	if (link->buffer) {
		pr_err("%s: Cannot add buffer, buffer already registred\n",
		       __func__);
		return -EINVAL;
	}

	trace_tsn_set_buffer(link, bufsize);
	link->buffer = buffer;
	link->head = link->buffer;
	link->tail = link->buffer;
	link->end = link->buffer + bufsize;
	link->buffer_size = bufsize;
	link->used_buffer_size = bufsize;
	return 0;
}

/**
 * _tsn_free_buffer - remove internal buffers
 *
 * This is the buffer where we store data before shipping it to TSN, or
 * where incoming data is staged.
 *
 * @param link   - the link that holds the buffer
 *
 * Must be called with tsn_lock() held.
 */
static void _tsn_free_buffer(struct tsn_link *link)
{
	if (!link)
		return;
	trace_tsn_free_buffer(link);
	kfree(link->buffer);
	link->buffer = NULL;
	link->head   = NULL;
	link->tail   = NULL;
	link->end    = NULL;
}

int tsn_set_buffer_size(struct tsn_link *link, size_t bsize)
{
	if (!link)
		return -EINVAL;

	if (bsize > link->buffer_size) {
		pr_err("%s: requested buffer (%zd) larger than allocated memory (%zd)\n",
		       __func__, bsize, link->buffer_size);
		return -ENOMEM;
	}

	tsn_lock(link);
	link->used_buffer_size = bsize;
	link->tail = link->buffer;
	link->head = link->buffer;
	link->end = link->buffer + link->used_buffer_size;
	link->low_water_mark = _get_low_water(link);
	link->high_water_mark = _get_high_water(link);
	tsn_unlock(link);

	pr_info("Set buffer_size, size: %zd, lowwater: %zd, highwater: %zd\n",
		link->used_buffer_size, link->low_water_mark,
		link->high_water_mark);
	return 0;
}
EXPORT_SYMBOL(tsn_set_buffer_size);

int tsn_clear_buffer_size(struct tsn_link *link)
{
	if (!link)
		return -EINVAL;

	tsn_lock(link);
	link->tail = link->buffer;
	link->head = link->buffer;
	link->end = link->buffer + link->buffer_size;
	memset(link->buffer, 0,  link->used_buffer_size);
	link->used_buffer_size = link->buffer_size;
	link->low_water_mark = _get_low_water(link);
	link->high_water_mark = _get_high_water(link);
	tsn_unlock(link);
	return 0;
}
EXPORT_SYMBOL(tsn_clear_buffer_size);

void *tsn_set_external_buffer(struct tsn_link *link, void *buffer,
			      size_t buffer_size)
{
	void *old_buffer;

	if (!link)
		return NULL;
	if (buffer_size < link->max_payload_size)
		pr_warn("%s: buffer_size (%zu) < max_payload_size (%u)\n",
			__func__, buffer_size, link->max_payload_size);

	tsn_lock(link);
	if (!link->external_buffer && link->buffer)
		_tsn_free_buffer(link);

	old_buffer = link->buffer;
	link->external_buffer = 1;
	link->buffer_size = buffer_size;
	link->used_buffer_size = buffer_size;
	link->buffer = buffer;
	link->head = link->buffer;
	link->tail = link->buffer;
	link->end = link->buffer + link->used_buffer_size;
	tsn_unlock(link);
	return old_buffer;
}
EXPORT_SYMBOL(tsn_set_external_buffer);

/* Caller must hold link->lock!
 *
 * Write data *into* buffer, either from net or from shim due to a
 * closing underflow event.
 */
static void __tsn_buffer_write(struct tsn_link *link, void *src, size_t bytes)
{
	int rem = 0;

	/* No Need To Wrap, if overflow we will overwrite without
	 * warning.
	 */
	trace_tsn_buffer_write(link, bytes);
	if (link->head + bytes < link->end) {
		memcpy(link->head, src, bytes);
		link->head += bytes;
	} else {
		rem = link->end - link->head;
		memcpy(link->head, src, rem);
		memcpy(link->buffer, (src + rem), bytes - rem);
		link->head = link->buffer + (bytes - rem);
	}
}

int tsn_buffer_write(struct tsn_link *link, void *src, size_t bytes)
{
	if (!link)
		return -EINVAL;

	/* We should not do anything if link has gone inactive */
	if (!tsn_link_is_on(link))
		return 0;

	/* Copied a batch of data and if link is disabled, it is now
	 * safe to enable it. Otherwise we will continue to send
	 * null-frames to remote.
	 */
	if (!tsn_lb(link))
		tsn_lb_enable(link);

	__tsn_buffer_write(link, src, bytes);

	return bytes;
}
EXPORT_SYMBOL(tsn_buffer_write);

/**
 * tsn_buffer_write_net - take data from a skbuff and write it into buffer
 *
 * When we receive a frame, we grab data from the skbuff and add it to
 * link->buffer.
 *
 * Note that this routine does NOT CARE about channels, samplesize etc,
 * it is a _pure_ copy that handles ringbuffer wraps etc.
 *
 * This function have side-effects as it will update internal tsn_link
 * values and trigger refill() should the buffer run low.
 *
 * NOTE: called from tsn_rx_handler() -> _tsnh_handle_du(), with
 *	 tsn_lock held.
 *
 * @param link current link that holds the buffer
 * @param buffer the buffer to copy from
 * @param bytes number of bytes
 * @returns Bytes copied into link->buffer, negative value upon error.
 */
int tsn_buffer_write_net(struct tsn_link *link, void *src, size_t bytes)
{
	size_t used;

	if (!link)
		return -EINVAL;

	/* Driver has not been enabled yet, i.e. it is in state 'off' and we
	 * have no way of knowing the state of the buffers.
	 * Silently drop the data, pretend write went ok
	 */
	trace_tsn_buffer_write_net(link, bytes);
	if (!tsn_lb(link))
		return bytes;

	__tsn_buffer_write(link, src, bytes);

	/* If we stored more data than high_water, we need to drain
	 *
	 * In ALSA, this will trigger a snd_pcm_period_elapsed() for the
	 * substream connected to this particular link.
	 */
	used = _tsn_buffer_used(link);
	if (used > link->high_water_mark) {
		trace_tsn_buffer_drain(link, used);
		link->ops->buffer_drain(link);
	}

	return bytes;
}

/* caller must hold link->lock!
 *
 * Read data *from* buffer, either to net or to shim due to a
 * closing overflow event.
 *
 * Function will *not* care if you read past head and into unchartered
 * territory, caller must ascertain validity of bytes.
 */
static void __tsn_buffer_read(struct tsn_link *link, void *dst, size_t bytes)
{
	int rem = 0;

	trace_tsn_buffer_read(link, bytes);
	if ((link->tail + bytes) < link->end) {
		memcpy(dst, link->tail, bytes);
		link->tail += bytes;
	} else {
		rem = link->end - link->tail;
		memcpy(dst, link->tail, rem);
		memcpy(dst + rem, link->buffer, bytes - rem);
		link->tail = link->buffer + bytes - rem;
	}
}

/**
 * tsn_buffer_read_net - read data from link->buffer and give to network layer
 *
 * When we send a frame, we grab data from the buffer and add it to the
 * sk_buff->data, this is primarily done by the Tx-subsystem in tsn_net
 * and is typically done in small chunks
 *
 * @param link current link that holds the buffer
 * @param buffer the buffer to copy into, must be at least of size bytes
 * @param bytes number of bytes.
 *
 * Note that this routine does NOT CARE about channels, samplesize etc,
 * it is a _pure_ copy that handles ringbuffer wraps etc.
 *
 * This function have side-effects as it will update internal tsn_link
 * values and trigger refill() should the buffer run low.
 *
 * NOTE: expects to be called with locks held
 *
 * @return Bytes copied into link->buffer, negative value upon error.
 */
int tsn_buffer_read_net(struct tsn_link *link, void *buffer, size_t bytes)
{
	size_t used;

	if (!link)
		return -EINVAL;

	/* link is currently inactive, e.g. we send frames, but without
	 * content
	 *
	 * This can be done before we ship data, or if we are muted
	 * (without expressively stating that over 1722.1
	 *
	 * We do not need to grab any locks here as we won't touch the
	 * link
	 */
	if (!tsn_lb(link)) {
		memset(buffer, 0, bytes);
		goto out;
	}

	/* sanity check of bytes to read
	 * FIXME
	 */

	__tsn_buffer_read(link, buffer, bytes);

	/* Trigger refill from client app */
	used = _tsn_buffer_used(link);
	if (used < link->low_water_mark) {
		trace_tsn_refill(link, used);
		link->ops->buffer_refill(link);
	}
out:
	return bytes;
}

int tsn_buffer_read(struct tsn_link *link, void *buffer, size_t bytes)
{
	if (!link)
		return -EINVAL;

	/* We should not do anything if link has gone inactive */
	if (!tsn_link_is_on(link))
		return 0;

	tsn_lock(link);
	__tsn_buffer_read(link, buffer, bytes);
	tsn_unlock(link);
	return bytes;
}
EXPORT_SYMBOL(tsn_buffer_read);

static int _tsn_send_batch(struct tsn_link *link)
{
	int ret = 0;
	int num_frames = (link->class_a ? 8 : 4);
	u64 ts_base_ns = ktime_to_ns(ktime_get()) + (link->class_a ? 2000000 : 50000000);
	u64 ts_delta_ns = (link->class_a ? 125000 : 250000);

	trace_tsn_send_batch(link, num_frames, ts_base_ns, ts_delta_ns);
	ret = tsn_net_send_set(link, num_frames, ts_base_ns, ts_delta_ns);
	if (ret < 0)
		pr_err("%s: could not send frame - %d\n", __func__, ret);

	return ret;
}

static int _tsn_hrtimer_callback(struct tsn_link *link)
{
	int ret = _tsn_send_batch(link);

	if (ret) {
		pr_err("%s: Error sending frames (%d), disabling link.\n",
		       __func__, ret);
		tsn_teardown_link(link);
		return 0;
	}
	return 0;
}

static enum hrtimer_restart tsn_hrtimer_callback(struct hrtimer *hrt)
{
	struct tsn_list *list = container_of(hrt, struct tsn_list, tsn_timer);
	struct tsn_link *link;
	struct hlist_node *tmp;
	int bkt = 0;

	if (!tsn_core_running(list))
		return HRTIMER_NORESTART;

	hrtimer_forward_now(hrt, ns_to_ktime(list->period_ns));

	hash_for_each_safe(tlinks, bkt, tmp, link, node) {
		if (tsn_link_is_on(link) && link->estype_talker)
			_tsn_hrtimer_callback(link);
	}

	return HRTIMER_RESTART;
}

static long tsn_hrtimer_init(void *arg)
{
	/* Run every 1ms, _tsn_send_batch will figure out how many
	 * frames to send for active frames
	 */
	struct tsn_list *list = (struct tsn_list *)arg;

	hrtimer_init(&list->tsn_timer, CLOCK_MONOTONIC,
		     HRTIMER_MODE_REL | HRTIMER_MODE_PINNED);

	list->tsn_timer.function = tsn_hrtimer_callback;
	hrtimer_cancel(&list->tsn_timer);
	atomic_set(&list->running, 1);

	hrtimer_start(&list->tsn_timer, ns_to_ktime(list->period_ns),
		      HRTIMER_MODE_REL);
	return 0;
}

static void tsn_hrtimer_exit(struct tsn_list *list)
{
	atomic_set(&list->running, 0);
	hrtimer_cancel(&list->tsn_timer);
}

/**
 * tsn_prepare_link - prepare link for role as Talker/Receiver
 *
 * Iow; this will start shipping data through the network-layer.
 *
 * @link: the actual link
 *
 * Current status: each link will get a periodic hrtimer that interrupts
 * and ships data every 1ms. This will change once we have proper driver
 * for hw (i.e. i210 driver).
 */
int tsn_prepare_link(struct tsn_link *link, struct tsn_shim_ops *shim_ops)
{
	int ret = 0;
	void *buffer;
	u16 framesize;
	struct net_device *netdev;

	/* TODO: use separate buckets (lists/rbtrees/whatever) for
	 * class_a and class_b talker streams. hrtimer-callback should
	 * not iterate over all.
	 */

	if (!link || !shim_ops || !shim_ops->probe)
		return -EINVAL;

	pr_info("TSN: allocating buffer, %zd bytes\n", link->buffer_size);

	tsn_lock(link);

	/* configure will calculate idle_slope based on framesize
	 * (header + payload)
	 */
	netdev = link->nic->dev;
	if (netdev->netdev_ops->ndo_tsn_link_configure) {
		framesize  = link->max_payload_size +
			     link->shim_header_size + tsnh_len_all();
		ret = netdev->netdev_ops->ndo_tsn_link_configure(netdev, link->class_a,
								 framesize, link->vlan_id & 0xfff);
		if (ret < 0)
			pr_err("Could not configure link - %d\n", ret);
	}

	link->ops = shim_ops;
	tsn_unlock(link);
	ret = link->ops->probe(link);
	if (ret != 0) {
		pr_err("%s: Could not probe shim (%d), cannot create link\n",
		       __func__, ret);
		link->ops = NULL;
		goto out;
	}

	tsn_lock(link);
	if (!link->external_buffer) {
		buffer = kmalloc(link->buffer_size, GFP_KERNEL);
		if (!buffer) {
			pr_err("%s: Could not allocate memory (%zu) for buffer\n",
			       __func__, link->buffer_size);
			link->ops = NULL;
			ret = -ENOMEM;
			goto unlock_out;
		}

		ret = _tsn_set_buffer(link, buffer, link->buffer_size);
		if (ret != 0) {
			pr_err("%s: Could not set buffer for TSN, got %d\n",
			       __func__, ret);
			goto unlock_out;
		}
	} else {
		/* FIXME: not handled */
		pr_info("TSN does not currently handle externally hosted buffers. This is on the TODO-list\n");
		ret = -EINVAL;
		goto unlock_out;
	}

	tsn_link_on(link);

unlock_out:
	tsn_unlock(link);
out:
	pr_info("%s: ret=%d\n", __func__, ret);
	return ret;
}

int tsn_teardown_link(struct tsn_link *link)
{
	if (!link)
		return -EINVAL;

	tsn_lock(link);
	tsn_lb_disable(link);
	tsn_link_off(link);
	tsn_unlock(link);

	/* Need to call media_close() without (spin-)locks held.
	 */
	if (link->ops)
		link->ops->media_close(link);

	tsn_lock(link);
	link->ops = NULL;
	_tsn_free_buffer(link);
	tsn_unlock(link);
	pr_info("%s: disabling all parts of link\n", __func__);
	return 0;
}

int tsn_shim_register_ops(struct tsn_shim_ops *shim_ops)
{
	if (!shim_ops)
		return -EINVAL;

	if (!shim_ops->buffer_refill || !shim_ops->buffer_drain ||
	    !shim_ops->media_close || !shim_ops->copy_size ||
	    !shim_ops->validate_header || !shim_ops->assemble_header ||
	    !shim_ops->get_payload_data)
		return -EINVAL;

	INIT_LIST_HEAD(&shim_ops->head);
	list_add_tail(&shim_ops->head, &tsn_shim_ops);
	return 0;
}
EXPORT_SYMBOL(tsn_shim_register_ops);

void tsn_shim_deregister_ops(struct tsn_shim_ops *shim_ops)
{
	struct tsn_link *link;
	struct hlist_node *tmp;
	int bkt;

	hash_for_each_safe(tlinks, bkt, tmp, link, node) {
		if (!link)
			continue;
		if (link->ops == shim_ops)
			tsn_teardown_link(link);
	}
	list_del(&shim_ops->head);
}
EXPORT_SYMBOL(tsn_shim_deregister_ops);

char *tsn_shim_get_active(struct tsn_link *link)
{
	if (!link || !link->ops)
		return "off";
	return link->ops->shim_name;
}

struct tsn_shim_ops *tsn_shim_find_by_name(const char *name)
{
	struct tsn_shim_ops *ops;

	if (!name || list_empty(&tsn_shim_ops))
		return NULL;

	list_for_each_entry(ops, &tsn_shim_ops, head) {
		if (strcmp(name, ops->shim_name) == 0)
			return ops;
	}
	return NULL;
}

ssize_t tsn_shim_export_probe_triggers(char *page)
{
	struct tsn_shim_ops *ops;
	ssize_t res = 0;

	if (!page || list_empty(&tsn_shim_ops))
		return 0;
	list_for_each_entry(ops, &tsn_shim_ops, head) {
		res += snprintf((page + res), PAGE_SIZE - res, "%s\n",
				 ops->shim_name);
	}
	return res;
}

struct tsn_link *tsn_create_and_add_link(struct tsn_nic *nic)
{
	u64 sid = 0;
	struct tsn_link *link = kzalloc(sizeof(*link), GFP_KERNEL);

	if (!link)
		return NULL;
	if (!nic) {
		kfree(link);
		return NULL;
	}

	spin_lock_init(&link->lock);
	tsn_lock(link);
	tsn_link_off(link);
	tsn_lb_disable(link);
	do {
		sid = prandom_u32();
		sid |= prandom_u32() << 31;
	} while (tsn_find_by_stream_id(sid));
	link->stream_id = sid;

	/* There's a slim chance that we actually hit on the first frame
	 * of data, but if we do, remote seqnr is most likely 0. If this
	 * is not up to par,, fix in rx_handler
	 */
	link->last_seqnr = 0xff;

	/* class B audio 48kHz sampling, S16LE, 2ch and IEC61883-6 CIP
	 * header
	 */
	link->max_payload_size = 48;
	link->shim_header_size =  8;

	/* Default VLAN ID is SR_PVID (2) unless otherwise supplied from
	 * MSRP, PCP is default 3 for class A, 2 for Class B (See IEEE
	 * 802.1Q-2011, table 6-6)
	 */
	link->vlan_id = 0x2;
	link->pcp_a = 3;
	link->pcp_b = 2;
	link->class_a = 0;

	link->buffer_size = 16536;
	/* default: talker since listener isn't implemented yet. */
	link->estype_talker = 1;

	link->nic = nic;
	tsn_unlock(link);

	/* Add the newly created link to the hashmap of all active links.
	 *
	 * test if sid is present in hashmap already (barf on that)
	 */

	mutex_lock(&tlist.lock);
	hash_add(tlinks, &link->node, link->stream_id);
	mutex_unlock(&tlist.lock);
	pr_info("%s: added link with stream_id: %llu\n",
		__func__, link->stream_id);

	return link;
}

ssize_t tsn_get_stream_ids(char *page, ssize_t len)
{
	struct tsn_link *link;
	struct hlist_node *tmp;
	char *buffer = page;
	int bkt;

	if (!page)
		return 0;

	if (hash_empty(tlinks))
		return sprintf(buffer, "no links registered\n");

	hash_for_each_safe(tlinks, bkt, tmp, link, node)
		buffer += sprintf(buffer, "%llu\n", link->stream_id);

	return (buffer - page);
}

struct tsn_link *tsn_find_by_stream_id(u64 sid)
{
	struct tsn_link *link;

	if (hash_empty(tlinks))
		return 0;

	hash_for_each_possible(tlinks, link, node, sid) {
		if (link->stream_id == sid)
			return link;
	}

	return NULL;
}

void tsn_remove_link(struct tsn_link *link)
{
	if (!link)
		return;
	tsn_net_close(link);
	mutex_lock(&tlist.lock);
	hash_del(&link->node);
	if (link->ops) {
		link->ops->media_close(link);
		link->ops = NULL;
	}

	mutex_unlock(&tlist.lock);
}

void tsn_readd_link(struct tsn_link *link, u64 newkey)
{
	if (!link)
		return;
	tsn_lock(link);
	if (hash_hashed(&link->node)) {
		pr_info("%s: updating link with stream_id %llu -> %llu\n",
			__func__, link->stream_id, newkey);
		tsn_remove_link(link);
	}

	link->stream_id = newkey;
	tsn_unlock(link);

	hash_add(tlinks, &link->node, link->stream_id);
}

static int _tsn_capable_nic(struct net_device *netdev, struct tsn_nic *nic)
{
	if (!nic || !netdev || !netdev->netdev_ops ||
	    !netdev->netdev_ops->ndo_tsn_capable)
		return -EINVAL;

	if (netdev->netdev_ops->ndo_tsn_capable(netdev) > 0)
		nic->capable = 1;

	return 0;
}

/* Identify all TSN-capable NICs in the system
 */
static int tsn_nic_probe(void)
{
	struct net *net;
	struct net_device *netdev;
	struct tsn_nic *nic;

	net = &init_net;
	rcu_read_lock();
	for_each_netdev_rcu(net, netdev) {
		pr_info("Found %s, alias %s on irq %d\n",
			netdev->name,
			netdev->ifalias,
			netdev->irq);
		pr_info("MAC: %pM", netdev->dev_addr);
		if (netdev->tx_queue_len)
			pr_info("Tx queue length: %lu\n", netdev->tx_queue_len);
		nic = kzalloc(sizeof(*nic), GFP_KERNEL);
		if (!nic) {
			pr_err("Could not allocate memory for tsn_nic!\n");
			return -ENOMEM;
		}
		nic->dev = netdev;
		nic->txq = netdev->num_tx_queues;
		nic->name = netdev->name;
		nic->tsn_list = &tlist;
		nic->dma_size = 1048576;

		_tsn_capable_nic(netdev, nic);

		/* if not capable and we are not in debug-mode, drop nic
		 * and continue
		 */
		if (!nic->capable && !in_debug) {
			pr_info("Invalid capabilities for NIC (%s), dropping from TSN list\n",
				netdev->name);
			kfree(nic);
			continue;
		}

		INIT_LIST_HEAD(&nic->list);
		mutex_lock(&tlist.lock);
		list_add_tail(&nic->list, &tlist.head);
		tlist.num_avail++;
		mutex_unlock(&tlist.lock);
	}
	rcu_read_unlock();

	return 0;
}

static void tsn_free_nic_list(struct tsn_list *list)
{
	struct tsn_nic *tmp, *next;

	mutex_lock(&list->lock);
	list_for_each_entry_safe(tmp, next, &list->head, list) {
		pr_info("Dropping %s from list\n", tmp->dev->name);
		list_del(&tmp->list);
		tmp->dev = NULL;
		kfree(tmp);
	}
	mutex_unlock(&list->lock);
}

/* all active links are stored in hashmap 'tlinks'
 */
static void tsn_remove_all_links(void)
{
	int bkt;
	struct tsn_link *link;
	struct hlist_node *tmp;

	hash_for_each_safe(tlinks, bkt, tmp, link, node) {
		pr_info("%s removing a link\n", __func__);
		if (!tsn_teardown_link(link))
			tsn_remove_link(link);
	}

	pr_info("%s: all links have been removed\n", __func__);
}

static int __init tsn_init_module(void)
{
	int ret = 0;

	INIT_LIST_HEAD(&tlist.head);
	mutex_init(&tlist.lock);

	atomic_set(&tlist.running, 0);
	tlist.period_ns =  1000000;

	/* Find all NICs, attach a rx-handler for sniffing out TSN
	 * traffic on *all* of them.
	 */
	tlist.num_avail = 0;
	ret = tsn_nic_probe();
	if (ret < 0) {
		pr_err("%s: somethign went awry whilst probing for NICs, aborting\n",
		       __func__);
		goto out;
	}

	if (!tlist.num_avail) {
		pr_err("%s: No capable NIC found. Perhaps load with in_debug=1 ?\n",
		       __func__);
		ret = -EINVAL;
		goto out;
	}

	/* register Rx-callbacks for all (valid) NICs */
	ret = tsn_net_add_rx(&tlist);
	if (ret < 0) {
		pr_err("%s: Could add Rx-handler, aborting\n", __func__);
		goto error_rx_out;
	}

	/* init DMA regions etc */
	ret = tsn_net_prepare_tx(&tlist);
	if (ret < 0) {
		pr_err("%s: could not prepare Tx, aborting\n", __func__);
		goto error_tx_out;
	}

	/* init hashtable */
	hash_init(tlinks);

	/* init configfs */
	ret = tsn_configfs_init(&tlist);
	if (ret < 0) {
		pr_err("%s: Could not initialize configfs properly (%d), aborting\n",
		       __func__, ret);
		goto error_cfs_out;
	}

	/* Test to see if on_cpu is available */
	if (on_cpu >= 0) {
		pr_info("%s: pinning timer on CPU %d\n", __func__, on_cpu);
		ret = work_on_cpu(on_cpu, tsn_hrtimer_init, &tlist);
		if (ret != 0) {
			pr_err("%s: could not init hrtimer properly on CPU %d, aborting\n",
			       __func__, on_cpu);
			goto error_hrt_out;
		}
	} else {
		ret = tsn_hrtimer_init(&tlist);
		if (ret < 0) {
			pr_err("%s: could not init hrtimer properly, aborting\n",
			       __func__);
			goto error_hrt_out;
		}
	}
	pr_info("TSN subsystem init OK\n");
	return 0;

error_hrt_out:
	tsn_remove_all_links();
	tsn_configfs_exit(&tlist);
error_cfs_out:
	tsn_net_disable_tx(&tlist);
error_tx_out:
	tsn_net_remove_rx(&tlist);
error_rx_out:
	tsn_free_nic_list(&tlist);
out:
	return ret;
}

static void __exit tsn_exit_module(void)
{
	pr_warn("removing module TSN\n");
	tsn_hrtimer_exit(&tlist);

	tsn_remove_all_links();
	tsn_configfs_exit(&tlist);

	/* Unregister Rx-handlers if set */
	tsn_net_remove_rx(&tlist);

	tsn_net_disable_tx(&tlist);

	tsn_free_nic_list(&tlist);

	pr_warn("TSN exit\n");
}
module_param(in_debug, int, S_IRUGO);
module_param(on_cpu, int, S_IRUGO);
module_init(tsn_init_module);
module_exit(tsn_exit_module);
MODULE_AUTHOR("Henrik Austad");
MODULE_LICENSE("GPL");
