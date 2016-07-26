/*
 * This file is provided under a dual BSD/GPLv2 license.  When using or
 *   redistributing this file, you may do so under either license.
 *
 *   GPL LICENSE SUMMARY
 *
 *   Copyright (C) 2015 EMC Corporation. All Rights Reserved.
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of version 2 of the GNU General Public License as
 *   published by the Free Software Foundation.
 *
 *   This program is distributed in the hope that it will be useful, but
 *   WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *   General Public License for more details.
 *
 *   BSD LICENSE
 *
 *   Copyright (C) 2015 EMC Corporation. All Rights Reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copy
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * PCIe NTB Linux driver
 *
 * Contact Information:
 * Allen Hubbe <Allen.Hubbe@emc.com>
 */

#ifndef _NTB_H_
#define _NTB_H_

#include <linux/completion.h>
#include <linux/device.h>

struct ntb_client;
struct ntb_dev;
struct pci_dev;

/**
 * enum ntb_topo - NTB connection topology
 * @NTB_TOPO_NONE:	Topology is unknown or invalid.
 * @NTB_TOPO_PRI:	On primary side of local ntb.
 * @NTB_TOPO_SEC:	On secondary side of remote ntb.
 * @NTB_TOPO_B2B_USD:	On primary side of local ntb upstream of remote ntb.
 * @NTB_TOPO_B2B_DSD:	On primary side of local ntb downstream of remote ntb.
 */
enum ntb_topo {
	NTB_TOPO_NONE = -1,
	NTB_TOPO_PRI,
	NTB_TOPO_SEC,
	NTB_TOPO_B2B_USD,
	NTB_TOPO_B2B_DSD,
};

static inline int ntb_topo_is_b2b(enum ntb_topo topo)
{
	switch ((int)topo) {
	case NTB_TOPO_B2B_USD:
	case NTB_TOPO_B2B_DSD:
		return 1;
	}
	return 0;
}

static inline char *ntb_topo_string(enum ntb_topo topo)
{
	switch (topo) {
	case NTB_TOPO_NONE:	return "NTB_TOPO_NONE";
	case NTB_TOPO_PRI:	return "NTB_TOPO_PRI";
	case NTB_TOPO_SEC:	return "NTB_TOPO_SEC";
	case NTB_TOPO_B2B_USD:	return "NTB_TOPO_B2B_USD";
	case NTB_TOPO_B2B_DSD:	return "NTB_TOPO_B2B_DSD";
	}
	return "NTB_TOPO_INVALID";
}

/**
 * enum ntb_speed - NTB link training speed
 * @NTB_SPEED_AUTO:	Request the max supported speed.
 * @NTB_SPEED_NONE:	Link is not trained to any speed.
 * @NTB_SPEED_GEN1:	Link is trained to gen1 speed.
 * @NTB_SPEED_GEN2:	Link is trained to gen2 speed.
 * @NTB_SPEED_GEN3:	Link is trained to gen3 speed.
 */
enum ntb_speed {
	NTB_SPEED_AUTO = -1,
	NTB_SPEED_NONE = 0,
	NTB_SPEED_GEN1 = 1,
	NTB_SPEED_GEN2 = 2,
	NTB_SPEED_GEN3 = 3,
};

/**
 * enum ntb_width - NTB link training width
 * @NTB_WIDTH_AUTO:	Request the max supported width.
 * @NTB_WIDTH_NONE:	Link is not trained to any width.
 * @NTB_WIDTH_1:	Link is trained to 1 lane width.
 * @NTB_WIDTH_2:	Link is trained to 2 lane width.
 * @NTB_WIDTH_4:	Link is trained to 4 lane width.
 * @NTB_WIDTH_8:	Link is trained to 8 lane width.
 * @NTB_WIDTH_12:	Link is trained to 12 lane width.
 * @NTB_WIDTH_16:	Link is trained to 16 lane width.
 * @NTB_WIDTH_32:	Link is trained to 32 lane width.
 */
enum ntb_width {
	NTB_WIDTH_AUTO = -1,
	NTB_WIDTH_NONE = 0,
	NTB_WIDTH_1 = 1,
	NTB_WIDTH_2 = 2,
	NTB_WIDTH_4 = 4,
	NTB_WIDTH_8 = 8,
	NTB_WIDTH_12 = 12,
	NTB_WIDTH_16 = 16,
	NTB_WIDTH_32 = 32,
};

/**
 * struct ntb_client_ops - ntb client operations
 * @probe:		Notify client of a new device.
 * @remove:		Notify client to remove a device.
 */
struct ntb_client_ops {
	int (*probe)(struct ntb_client *client, struct ntb_dev *ntb);
	void (*remove)(struct ntb_client *client, struct ntb_dev *ntb);
};

static inline int ntb_client_ops_is_valid(const struct ntb_client_ops *ops)
{
	/* commented callbacks are not required: */
	return
		ops->probe			&&
		ops->remove			&&
		1;
}

/**
 * struct ntb_msg - ntb driver message structure
 * @type:	Message type.
 * @payload:	Payload data to send to a peer
 * @data:	Array of u32 data to send (size might be hw dependent)
 */
#define NTB_MAX_MSGSIZE 4
struct ntb_msg {
	union {
		struct {
			u32 type;
			u32 payload[NTB_MAX_MSGSIZE - 1];
		};
		u32 data[NTB_MAX_MSGSIZE];
	};
};

/**
 * enum NTB_MSG_EVENT - message event types
 * @NTB_MSG_NEW:	New message just arrived and passed to the handler
 * @NTB_MSG_SENT:	Posted message has just been successfully sent
 * @NTB_MSG_FAIL:	Posted message failed to be sent
 */
enum NTB_MSG_EVENT {
	NTB_MSG_NEW,
	NTB_MSG_SENT,
	NTB_MSG_FAIL
};

/**
 * struct ntb_ctx_ops - ntb driver context operations
 * @link_event:		See ntb_link_event().
 * @db_event:		See ntb_db_event().
 * @msg_event:		See ntb_msg_event().
 */
struct ntb_ctx_ops {
	void (*link_event)(void *ctx);
	void (*db_event)(void *ctx, int db_vector);
	void (*msg_event)(void *ctx, enum NTB_MSG_EVENT ev, struct ntb_msg *msg);
};

static inline int ntb_ctx_ops_is_valid(const struct ntb_ctx_ops *ops)
{
	/* commented callbacks are not required: */
	return
		/* ops->link_event		&& */
		/* ops->db_event		&& */
		/* ops->msg_event		&& */
		1;
}

/**
 * struct ntb_ctx_ops - ntb device operations
 * @link_is_up:		See ntb_link_is_up().
 * @link_enable:	See ntb_link_enable().
 * @link_disable:	See ntb_link_disable().
 * @mw_count:		See ntb_mw_count().
 * @mw_get_maprsc:	See ntb_mw_get_maprsc().
 * @mw_set_trans:	See ntb_mw_set_trans().
 * @mw_get_trans:	See ntb_mw_get_trans().
 * @mw_get_align:	See ntb_mw_get_align().
 * @peer_mw_count:	See ntb_peer_mw_count().
 * @peer_mw_set_trans:	See ntb_peer_mw_set_trans().
 * @peer_mw_get_trans:	See ntb_peer_mw_get_trans().
 * @peer_mw_get_align:	See ntb_peer_mw_get_align().
 * @db_is_unsafe:	See ntb_db_is_unsafe().
 * @db_valid_mask:	See ntb_db_valid_mask().
 * @db_vector_count:	See ntb_db_vector_count().
 * @db_vector_mask:	See ntb_db_vector_mask().
 * @db_read:		See ntb_db_read().
 * @db_set:		See ntb_db_set().
 * @db_clear:		See ntb_db_clear().
 * @db_read_mask:	See ntb_db_read_mask().
 * @db_set_mask:	See ntb_db_set_mask().
 * @db_clear_mask:	See ntb_db_clear_mask().
 * @peer_db_addr:	See ntb_peer_db_addr().
 * @peer_db_read:	See ntb_peer_db_read().
 * @peer_db_set:	See ntb_peer_db_set().
 * @peer_db_clear:	See ntb_peer_db_clear().
 * @peer_db_read_mask:	See ntb_peer_db_read_mask().
 * @peer_db_set_mask:	See ntb_peer_db_set_mask().
 * @peer_db_clear_mask:	See ntb_peer_db_clear_mask().
 * @spad_is_unsafe:	See ntb_spad_is_unsafe().
 * @spad_count:		See ntb_spad_count().
 * @spad_read:		See ntb_spad_read().
 * @spad_write:		See ntb_spad_write().
 * @peer_spad_addr:	See ntb_peer_spad_addr().
 * @peer_spad_read:	See ntb_peer_spad_read().
 * @peer_spad_write:	See ntb_peer_spad_write().
 * @msg_post:		See ntb_msg_post().
 * @msg_size:		See ntb_msg_size().
 */
struct ntb_dev_ops {
	int (*link_is_up)(struct ntb_dev *ntb,
			  enum ntb_speed *speed, enum ntb_width *width);
	int (*link_enable)(struct ntb_dev *ntb,
			   enum ntb_speed max_speed, enum ntb_width max_width);
	int (*link_disable)(struct ntb_dev *ntb);

	int (*mw_count)(struct ntb_dev *ntb);
	int (*mw_get_maprsc)(struct ntb_dev *ntb, int idx,
			     phys_addr_t *base, resource_size_t *size);
	int (*mw_get_align)(struct ntb_dev *ntb, int idx,
			    resource_size_t *addr_align,
			    resource_size_t *size_align,
			    resource_size_t *size_max);
	int (*mw_set_trans)(struct ntb_dev *ntb, int idx,
			    dma_addr_t addr, resource_size_t size);
	int (*mw_get_trans)(struct ntb_dev *ntb, int idx,
			    dma_addr_t *addr, resource_size_t *size);

	int (*peer_mw_count)(struct ntb_dev *ntb);
	int (*peer_mw_get_align)(struct ntb_dev *ntb, int idx,
				 resource_size_t *addr_align,
				 resource_size_t *size_align,
				 resource_size_t *size_max);
	int (*peer_mw_set_trans)(struct ntb_dev *ntb, int idx,
				 dma_addr_t addr, resource_size_t size);
	int (*peer_mw_get_trans)(struct ntb_dev *ntb, int idx,
				 dma_addr_t *addr, resource_size_t *size);

	int (*db_is_unsafe)(struct ntb_dev *ntb);
	u64 (*db_valid_mask)(struct ntb_dev *ntb);
	int (*db_vector_count)(struct ntb_dev *ntb);
	u64 (*db_vector_mask)(struct ntb_dev *ntb, int db_vector);

	u64 (*db_read)(struct ntb_dev *ntb);
	int (*db_set)(struct ntb_dev *ntb, u64 db_bits);
	int (*db_clear)(struct ntb_dev *ntb, u64 db_bits);

	u64 (*db_read_mask)(struct ntb_dev *ntb);
	int (*db_set_mask)(struct ntb_dev *ntb, u64 db_bits);
	int (*db_clear_mask)(struct ntb_dev *ntb, u64 db_bits);

	int (*peer_db_addr)(struct ntb_dev *ntb,
			    phys_addr_t *db_addr, resource_size_t *db_size);
	u64 (*peer_db_read)(struct ntb_dev *ntb);
	int (*peer_db_set)(struct ntb_dev *ntb, u64 db_bits);
	int (*peer_db_clear)(struct ntb_dev *ntb, u64 db_bits);

	u64 (*peer_db_read_mask)(struct ntb_dev *ntb);
	int (*peer_db_set_mask)(struct ntb_dev *ntb, u64 db_bits);
	int (*peer_db_clear_mask)(struct ntb_dev *ntb, u64 db_bits);

	int (*spad_is_unsafe)(struct ntb_dev *ntb);
	int (*spad_count)(struct ntb_dev *ntb);

	u32 (*spad_read)(struct ntb_dev *ntb, int idx);
	int (*spad_write)(struct ntb_dev *ntb, int idx, u32 val);

	int (*peer_spad_addr)(struct ntb_dev *ntb, int idx,
			      phys_addr_t *spad_addr);
	u32 (*peer_spad_read)(struct ntb_dev *ntb, int idx);
	int (*peer_spad_write)(struct ntb_dev *ntb, int idx, u32 val);

	int (*msg_post)(struct ntb_dev *ntb, struct ntb_msg *msg);
	int (*msg_size)(struct ntb_dev *ntb);
};

/**
 * struct ntb_client - client interested in ntb devices
 * @drv:		Linux driver object.
 * @ops:		See &ntb_client_ops.
 */
struct ntb_client {
	struct device_driver		drv;
	const struct ntb_client_ops	ops;
};
#define drv_ntb_client(__drv) container_of((__drv), struct ntb_client, drv)

/**
 * struct ntb_bus_data - NTB bus data
 * @sync_msk:	Synchroous devices mask
 * @async_msk:	Asynchronous devices mask
 * @both_msk:	Both sync and async devices mask
 */
#define NTB_MAX_DEVID (8*BITS_PER_LONG)
struct ntb_bus_data {
	unsigned long sync_msk[8];
	unsigned long async_msk[8];
	unsigned long both_msk[8];
};

/**
 * struct ntb_device - ntb device
 * @dev:		Linux device object.
 * @pdev:		Pci device entry of the ntb.
 * @topo:		Detected topology of the ntb.
 * @ops:		See &ntb_dev_ops.
 * @ctx:		See &ntb_ctx_ops.
 * @ctx_ops:		See &ntb_ctx_ops.
 */
struct ntb_dev {
	struct device			dev;
	struct pci_dev			*pdev;
	enum ntb_topo			topo;
	const struct ntb_dev_ops	*ops;
	void				*ctx;
	const struct ntb_ctx_ops	*ctx_ops;

	/* private: */

	/* device id */
	int id;
	/* synchronize setting, clearing, and calling ctx_ops */
	spinlock_t			ctx_lock;
	/* block unregister until device is fully released */
	struct completion		released;
};
#define dev_ntb(__dev) container_of((__dev), struct ntb_dev, dev)

/**
 * ntb_valid_sync_dev_ops() - valid operations for synchronous hardware setup
 * @ntb:	NTB device
 *
 * There might be two types of NTB hardware differed by the way of the settings
 * configuration. The synchronous chips allows to set the memory windows by
 * directly writing to the peer registers. Additionally there can be shared
 * Scratchpad registers for synchronous information exchange. Client drivers
 * should call this function to make sure the hardware supports the proper
 * functionality.
 */
static inline int ntb_valid_sync_dev_ops(const struct ntb_dev *ntb)
{
	const struct ntb_dev_ops *ops = ntb->ops;

	/* Commented callbacks are not required, but might be developed */
	return	/* NTB link status ops */
		ops->link_is_up					&&
		ops->link_enable				&&
		ops->link_disable				&&

		/* Synchronous memory windows ops */
		ops->mw_count					&&
		ops->mw_get_maprsc				&&
		/* ops->mw_get_align				&& */
		/* ops->mw_set_trans				&& */
		/* ops->mw_get_trans				&& */
		ops->peer_mw_count				&&
		ops->peer_mw_get_align				&&
		ops->peer_mw_set_trans				&&
		/* ops->peer_mw_get_trans			&& */

		/* Doorbell ops */
		/* ops->db_is_unsafe				&& */
		ops->db_valid_mask				&&
		/* both set, or both unset */
		(!ops->db_vector_count == !ops->db_vector_mask)	&&
		ops->db_read					&&
		/* ops->db_set					&& */
		ops->db_clear					&&
		/* ops->db_read_mask				&& */
		ops->db_set_mask				&&
		ops->db_clear_mask				&&
		/* ops->peer_db_addr				&& */
		/* ops->peer_db_read				&& */
		ops->peer_db_set				&&
		/* ops->peer_db_clear				&& */
		/* ops->peer_db_read_mask			&& */
		/* ops->peer_db_set_mask			&& */
		/* ops->peer_db_clear_mask			&& */

		/* Scratchpad ops */
		/* ops->spad_is_unsafe				&& */
		ops->spad_count					&&
		ops->spad_read					&&
		ops->spad_write					&&
		/* ops->peer_spad_addr				&& */
		/* ops->peer_spad_read				&& */
		ops->peer_spad_write				&&

		/* Messages IO ops */
		/* ops->msg_post				&& */
		/* ops->msg_size				&& */
		1;
}

/**
 * ntb_valid_async_dev_ops() - valid operations for asynchronous hardware setup
 * @ntb:	NTB device
 *
 * There might be two types of NTB hardware differed by the way of the settings
 * configuration. The asynchronous chips does not allow to set the memory
 * windows by directly writing to the peer registers. Instead it implements
 * the additional method to communinicate between NTB nodes like messages.
 * Scratchpad registers aren't likely supported by such hardware. Client
 * drivers should call this function to make sure the hardware supports
 * the proper functionality.
 */
static inline int ntb_valid_async_dev_ops(const struct ntb_dev *ntb)
{
	const struct ntb_dev_ops *ops = ntb->ops;

	/* Commented callbacks are not required, but might be developed */
	return	/* NTB link status ops */
		ops->link_is_up					&&
		ops->link_enable				&&
		ops->link_disable				&&

		/* Asynchronous memory windows ops */
		ops->mw_count					&&
		ops->mw_get_maprsc				&&
		ops->mw_get_align				&&
		ops->mw_set_trans				&&
		/* ops->mw_get_trans				&& */
		ops->peer_mw_count				&&
		ops->peer_mw_get_align				&&
		/* ops->peer_mw_set_trans			&& */
		/* ops->peer_mw_get_trans			&& */

		/* Doorbell ops */
		/* ops->db_is_unsafe				&& */
		ops->db_valid_mask				&&
		/* both set, or both unset */
		(!ops->db_vector_count == !ops->db_vector_mask)	&&
		ops->db_read					&&
		/* ops->db_set					&& */
		ops->db_clear					&&
		/* ops->db_read_mask				&& */
		ops->db_set_mask				&&
		ops->db_clear_mask				&&
		/* ops->peer_db_addr				&& */
		/* ops->peer_db_read				&& */
		ops->peer_db_set				&&
		/* ops->peer_db_clear				&& */
		/* ops->peer_db_read_mask			&& */
		/* ops->peer_db_set_mask			&& */
		/* ops->peer_db_clear_mask			&& */

		/* Scratchpad ops */
		/* ops->spad_is_unsafe				&& */
		/* ops->spad_count				&& */
		/* ops->spad_read				&& */
		/* ops->spad_write				&& */
		/* ops->peer_spad_addr				&& */
		/* ops->peer_spad_read				&& */
		/* ops->peer_spad_write				&& */

		/* Messages IO ops */
		ops->msg_post					&&
		ops->msg_size					&&
		1;
}



/**
 * ntb_register_client() - register a client for interest in ntb devices
 * @client:	Client context.
 *
 * The client will be added to the list of clients interested in ntb devices.
 * The client will be notified of any ntb devices that are not already
 * associated with a client, or if ntb devices are registered later.
 *
 * Return: Zero if the client is registered, otherwise an error number.
 */
#define ntb_register_client(client) \
	__ntb_register_client((client), THIS_MODULE, KBUILD_MODNAME)

int __ntb_register_client(struct ntb_client *client, struct module *mod,
			  const char *mod_name);

/**
 * ntb_unregister_client() - unregister a client for interest in ntb devices
 * @client:	Client context.
 *
 * The client will be removed from the list of clients interested in ntb
 * devices.  If any ntb devices are associated with the client, the client will
 * be notified to remove those devices.
 */
void ntb_unregister_client(struct ntb_client *client);

#define module_ntb_client(__ntb_client) \
	module_driver(__ntb_client, ntb_register_client, \
			ntb_unregister_client)

/**
 * ntb_register_device() - register a ntb device
 * @ntb:	NTB device context.
 *
 * The device will be added to the list of ntb devices.  If any clients are
 * interested in ntb devices, each client will be notified of the ntb device,
 * until at most one client accepts the device.
 *
 * Return: Zero if the device is registered, otherwise an error number.
 */
int ntb_register_device(struct ntb_dev *ntb);

/**
 * ntb_register_device() - unregister a ntb device
 * @ntb:	NTB device context.
 *
 * The device will be removed from the list of ntb devices.  If the ntb device
 * is associated with a client, the client will be notified to remove the
 * device.
 */
void ntb_unregister_device(struct ntb_dev *ntb);

/**
 * ntb_set_ctx() - associate a driver context with an ntb device
 * @ntb:	NTB device context.
 * @ctx:	Driver context.
 * @ctx_ops:	Driver context operations.
 *
 * Associate a driver context and operations with a ntb device.  The context is
 * provided by the client driver, and the driver may associate a different
 * context with each ntb device.
 *
 * Return: Zero if the context is associated, otherwise an error number.
 */
int ntb_set_ctx(struct ntb_dev *ntb, void *ctx,
		const struct ntb_ctx_ops *ctx_ops);

/**
 * ntb_clear_ctx() - disassociate any driver context from an ntb device
 * @ntb:	NTB device context.
 *
 * Clear any association that may exist between a driver context and the ntb
 * device.
 */
void ntb_clear_ctx(struct ntb_dev *ntb);

/**
 * ntb_link_event() - notify driver context of a change in link status
 * @ntb:	NTB device context.
 *
 * Notify the driver context that the link status may have changed.  The driver
 * should call ntb_link_is_up() to get the current status.
 */
void ntb_link_event(struct ntb_dev *ntb);

/**
 * ntb_db_event() - notify driver context of a doorbell event
 * @ntb:	NTB device context.
 * @vector:	Interrupt vector number.
 *
 * Notify the driver context of a doorbell event.  If hardware supports
 * multiple interrupt vectors for doorbells, the vector number indicates which
 * vector received the interrupt.  The vector number is relative to the first
 * vector used for doorbells, starting at zero, and must be less than
 ** ntb_db_vector_count().  The driver may call ntb_db_read() to check which
 * doorbell bits need service, and ntb_db_vector_mask() to determine which of
 * those bits are associated with the vector number.
 */
void ntb_db_event(struct ntb_dev *ntb, int vector);

/**
 * ntb_msg_event() - notify driver context of event in messaging subsystem
 * @ntb:	NTB device context.
 * @ev:		Event type caused the handler invocation
 * @msg:	Message related to the event
 *
 * Notify the driver context that there is some event happaned in the event
 * subsystem. If NTB_MSG_NEW is emitted then the new message has just arrived.
 * NTB_MSG_SENT is rised if some message has just been successfully sent to a
 * peer. If a message failed to be sent then NTB_MSG_FAIL is emitted. The very
 * last argument is used to pass the event related message. It discarded right
 * after the handler returns.
 */
void ntb_msg_event(struct ntb_dev *ntb, enum NTB_MSG_EVENT ev,
		   struct ntb_msg *msg);

/**
 * ntb_link_is_up() - get the current ntb link state
 * @ntb:	NTB device context.
 * @speed:	OUT - The link speed expressed as PCIe generation number.
 * @width:	OUT - The link width expressed as the number of PCIe lanes.
 *
 * Get the current state of the ntb link.  It is recommended to query the link
 * state once after every link event.  It is safe to query the link state in
 * the context of the link event callback.
 *
 * Return: One if the link is up, zero if the link is down, otherwise a
 *		negative value indicating the error number.
 */
static inline int ntb_link_is_up(struct ntb_dev *ntb,
				 enum ntb_speed *speed, enum ntb_width *width)
{
	return ntb->ops->link_is_up(ntb, speed, width);
}

/**
 * ntb_link_enable() - enable the link on the secondary side of the ntb
 * @ntb:	NTB device context.
 * @max_speed:	The maximum link speed expressed as PCIe generation number.
 * @max_width:	The maximum link width expressed as the number of PCIe lanes.
 *
 * Enable the link on the secondary side of the ntb.  This can only be done
 * from only one (primary or secondary) side of the ntb in primary or b2b
 * topology.  The ntb device should train the link to its maximum speed and
 * width, or the requested speed and width, whichever is smaller, if supported.
 *
 * Return: Zero on success, otherwise an error number.
 */
static inline int ntb_link_enable(struct ntb_dev *ntb,
				  enum ntb_speed max_speed,
				  enum ntb_width max_width)
{
	return ntb->ops->link_enable(ntb, max_speed, max_width);
}

/**
 * ntb_link_disable() - disable the link on the secondary side of the ntb
 * @ntb:	NTB device context.
 *
 * Disable the link on the secondary side of the ntb.  This can only be
 * done from only one (primary or secondary) side of the ntb in primary or b2b
 * topology.  The ntb device should disable the link.  Returning from this call
 * must indicate that a barrier has passed, though with no more writes may pass
 * in either direction across the link, except if this call returns an error
 * number.
 *
 * Return: Zero on success, otherwise an error number.
 */
static inline int ntb_link_disable(struct ntb_dev *ntb)
{
	return ntb->ops->link_disable(ntb);
}

/**
 * ntb_mw_count() - get the number of local memory windows
 * @ntb:	NTB device context.
 *
 * Hardware and topology may support a different number of memory windows at
 * local and remote devices
 *
 * Return: the number of memory windows.
 */
static inline int ntb_mw_count(struct ntb_dev *ntb)
{
	return ntb->ops->mw_count(ntb);
}

/**
 * ntb_mw_get_maprsc() - get the range of a memory window to map
 * @ntb:	NTB device context.
 * @idx:	Memory window number.
 * @base:	OUT - the base address for mapping the memory window
 * @size:	OUT - the size for mapping the memory window
 *
 * Get the map range of a memory window. The base and size may be used for
 * mapping the memory window to access the peer memory.
 *
 * Return: Zero on success, otherwise an error number.
 */
static inline int ntb_mw_get_maprsc(struct ntb_dev *ntb, int idx,
				    phys_addr_t *base, resource_size_t *size)
{
	return ntb->ops->mw_get_maprsc(ntb, idx, base, size);
}

/**
 * ntb_mw_get_align() - get memory window alignment of the local node
 * @ntb:	NTB device context.
 * @idx:	Memory window number.
 * @addr_align:	OUT - the translated base address alignment of the memory window
 * @size_align:	OUT - the translated memory size alignment of the memory window
 * @size_max:	OUT - the translated memory maximum size
 *
 * Get the alignment parameters to allocate the proper memory window. NULL may
 * be given for any output parameter if the value is not needed.
 *
 * Drivers of synchronous hardware don't have to support it.
 *
 * Return: Zero on success, otherwise an error number.
 */
static inline int ntb_mw_get_align(struct ntb_dev *ntb, int idx,
				   resource_size_t *addr_align,
				   resource_size_t *size_align,
				   resource_size_t *size_max)
{
	if (!ntb->ops->mw_get_align)
		return -EINVAL;

	return ntb->ops->mw_get_align(ntb, idx, addr_align, size_align, size_max);
}

/**
 * ntb_mw_set_trans() - set the translated base address of a peer memory window
 * @ntb:	NTB device context.
 * @idx:	Memory window number.
 * @addr:	DMA memory address exposed by the peer.
 * @size:	Size of the memory exposed by the peer.
 *
 * Set the translated base address of a memory window. The peer preliminary
 * allocates a memory, then someway passes the address to the remote node, that
 * finally sets up the memory window at the address, up to the size. The address
 * and size must be aligned to the parameters specified by ntb_mw_get_align() of
 * the local node and ntb_peer_mw_get_align() of the peer, which must return the
 * same values. Zero size effectively disables the memory window.
 *
 * Drivers of synchronous hardware don't have to support it.
 *
 * Return: Zero on success, otherwise an error number.
 */
static inline int ntb_mw_set_trans(struct ntb_dev *ntb, int idx,
				   dma_addr_t addr, resource_size_t size)
{
	if (!ntb->ops->mw_set_trans)
		return -EINVAL;

	return ntb->ops->mw_set_trans(ntb, idx, addr, size);
}

/**
 * ntb_mw_get_trans() - get the translated base address of a memory window
 * @ntb:	NTB device context.
 * @idx:	Memory window number.
 * @addr:	The dma memory address exposed by the peer.
 * @size:	The size of the memory exposed by the peer.
 *
 * Get the translated base address of a memory window spicified for the local
 * hardware and allocated by the peer. If the addr and size are zero, the
 * memory window is effectively disabled.
 *
 * Return: Zero on success, otherwise an error number.
 */
static inline int ntb_mw_get_trans(struct ntb_dev *ntb, int idx,
				   dma_addr_t *addr, resource_size_t *size)
{
	if (!ntb->ops->mw_get_trans)
		return -EINVAL;

	return ntb->ops->mw_get_trans(ntb, idx, addr, size);
}

/**
 * ntb_peer_mw_count() - get the number of peer memory windows
 * @ntb:	NTB device context.
 *
 * Hardware and topology may support a different number of memory windows at
 * local and remote nodes.
 *
 * Return: the number of memory windows.
 */
static inline int ntb_peer_mw_count(struct ntb_dev *ntb)
{
	return ntb->ops->peer_mw_count(ntb);
}

/**
 * ntb_peer_mw_get_align() - get memory window alignment of the peer
 * @ntb:	NTB device context.
 * @idx:	Memory window number.
 * @addr_align:	OUT - the translated base address alignment of the memory window
 * @size_align:	OUT - the translated memory size alignment of the memory window
 * @size_max:	OUT - the translated memory maximum size
 *
 * Get the alignment parameters to allocate the proper memory window for the
 * peer. NULL may be given for any output parameter if the value is not needed.
 *
 * Return: Zero on success, otherwise an error number.
 */
static inline int ntb_peer_mw_get_align(struct ntb_dev *ntb, int idx,
					resource_size_t *addr_align,
					resource_size_t *size_align,
					resource_size_t *size_max)
{
	if (!ntb->ops->peer_mw_get_align)
		return -EINVAL;

	return ntb->ops->peer_mw_get_align(ntb, idx, addr_align, size_align,
					   size_max);
}

/**
 * ntb_peer_mw_set_trans() - set the translated base address of a peer
 *			     memory window
 * @ntb:	NTB device context.
 * @idx:	Memory window number.
 * @addr:	Local DMA memory address exposed to the peer.
 * @size:	Size of the memory exposed to the peer.
 *
 * Set the translated base address of a memory window exposed to the peer.
 * The local node preliminary allocates the window, then directly writes the
 * address and size to the peer control registers. The address and size must
 * be aligned to the parameters specified by ntb_peer_mw_get_align() of
 * the local node and ntb_mw_get_align() of the peer, which must return the
 * same values. Zero size effectively disables the memory window.
 *
 * Drivers of synchronous hardware must support it.
 *
 * Return: Zero on success, otherwise an error number.
 */
static inline int ntb_peer_mw_set_trans(struct ntb_dev *ntb, int idx,
					dma_addr_t addr, resource_size_t size)
{
	if (!ntb->ops->peer_mw_set_trans)
		return -EINVAL;

	return ntb->ops->peer_mw_set_trans(ntb, idx, addr, size);
}

/**
 * ntb_peer_mw_get_trans() - get the translated base address of a peer
 *			     memory window
 * @ntb:	NTB device context.
 * @idx:	Memory window number.
 * @addr:	Local dma memory address exposed to the peer.
 * @size:	Size of the memory exposed to the peer.
 *
 * Get the translated base address of a memory window spicified for the peer
 * hardware. If the addr and size are zero then the memory window is effectively
 * disabled.
 *
 * Return: Zero on success, otherwise an error number.
 */
static inline int ntb_peer_mw_get_trans(struct ntb_dev *ntb, int idx,
					dma_addr_t *addr, resource_size_t *size)
{
	if (!ntb->ops->peer_mw_get_trans)
		return -EINVAL;

	return ntb->ops->peer_mw_get_trans(ntb, idx, addr, size);
}

/**
 * ntb_db_is_unsafe() - check if it is safe to use hardware doorbell
 * @ntb:	NTB device context.
 *
 * It is possible for some ntb hardware to be affected by errata.  Hardware
 * drivers can advise clients to avoid using doorbells.  Clients may ignore
 * this advice, though caution is recommended.
 *
 * Return: Zero if it is safe to use doorbells, or One if it is not safe.
 */
static inline int ntb_db_is_unsafe(struct ntb_dev *ntb)
{
	if (!ntb->ops->db_is_unsafe)
		return 0;

	return ntb->ops->db_is_unsafe(ntb);
}

/**
 * ntb_db_valid_mask() - get a mask of doorbell bits supported by the ntb
 * @ntb:	NTB device context.
 *
 * Hardware may support different number or arrangement of doorbell bits.
 *
 * Return: A mask of doorbell bits supported by the ntb.
 */
static inline u64 ntb_db_valid_mask(struct ntb_dev *ntb)
{
	return ntb->ops->db_valid_mask(ntb);
}

/**
 * ntb_db_vector_count() - get the number of doorbell interrupt vectors
 * @ntb:	NTB device context.
 *
 * Hardware may support different number of interrupt vectors.
 *
 * Return: The number of doorbell interrupt vectors.
 */
static inline int ntb_db_vector_count(struct ntb_dev *ntb)
{
	if (!ntb->ops->db_vector_count)
		return 1;

	return ntb->ops->db_vector_count(ntb);
}

/**
 * ntb_db_vector_mask() - get a mask of doorbell bits serviced by a vector
 * @ntb:	NTB device context.
 * @vector:	Doorbell vector number.
 *
 * Each interrupt vector may have a different number or arrangement of bits.
 *
 * Return: A mask of doorbell bits serviced by a vector.
 */
static inline u64 ntb_db_vector_mask(struct ntb_dev *ntb, int vector)
{
	if (!ntb->ops->db_vector_mask)
		return ntb_db_valid_mask(ntb);

	return ntb->ops->db_vector_mask(ntb, vector);
}

/**
 * ntb_db_read() - read the local doorbell register
 * @ntb:	NTB device context.
 *
 * Read the local doorbell register, and return the bits that are set.
 *
 * Return: The bits currently set in the local doorbell register.
 */
static inline u64 ntb_db_read(struct ntb_dev *ntb)
{
	return ntb->ops->db_read(ntb);
}

/**
 * ntb_db_set() - set bits in the local doorbell register
 * @ntb:	NTB device context.
 * @db_bits:	Doorbell bits to set.
 *
 * Set bits in the local doorbell register, which may generate a local doorbell
 * interrupt.  Bits that were already set must remain set.
 *
 * This is unusual, and hardware may not support it.
 *
 * Return: Zero on success, otherwise an error number.
 */
static inline int ntb_db_set(struct ntb_dev *ntb, u64 db_bits)
{
	if (!ntb->ops->db_set)
		return -EINVAL;

	return ntb->ops->db_set(ntb, db_bits);
}

/**
 * ntb_db_clear() - clear bits in the local doorbell register
 * @ntb:	NTB device context.
 * @db_bits:	Doorbell bits to clear.
 *
 * Clear bits in the local doorbell register, arming the bits for the next
 * doorbell.
 *
 * Return: Zero on success, otherwise an error number.
 */
static inline int ntb_db_clear(struct ntb_dev *ntb, u64 db_bits)
{
	return ntb->ops->db_clear(ntb, db_bits);
}

/**
 * ntb_db_read_mask() - read the local doorbell mask
 * @ntb:	NTB device context.
 *
 * Read the local doorbell mask register, and return the bits that are set.
 *
 * This is unusual, though hardware is likely to support it.
 *
 * Return: The bits currently set in the local doorbell mask register.
 */
static inline u64 ntb_db_read_mask(struct ntb_dev *ntb)
{
	if (!ntb->ops->db_read_mask)
		return 0;

	return ntb->ops->db_read_mask(ntb);
}

/**
 * ntb_db_set_mask() - set bits in the local doorbell mask
 * @ntb:	NTB device context.
 * @db_bits:	Doorbell mask bits to set.
 *
 * Set bits in the local doorbell mask register, preventing doorbell interrupts
 * from being generated for those doorbell bits.  Bits that were already set
 * must remain set.
 *
 * Return: Zero on success, otherwise an error number.
 */
static inline int ntb_db_set_mask(struct ntb_dev *ntb, u64 db_bits)
{
	return ntb->ops->db_set_mask(ntb, db_bits);
}

/**
 * ntb_db_clear_mask() - clear bits in the local doorbell mask
 * @ntb:	NTB device context.
 * @db_bits:	Doorbell bits to clear.
 *
 * Clear bits in the local doorbell mask register, allowing doorbell interrupts
 * from being generated for those doorbell bits.  If a doorbell bit is already
 * set at the time the mask is cleared, and the corresponding mask bit is
 * changed from set to clear, then the ntb driver must ensure that
 * ntb_db_event() is called.  If the hardware does not generate the interrupt
 * on clearing the mask bit, then the driver must call ntb_db_event() anyway.
 *
 * Return: Zero on success, otherwise an error number.
 */
static inline int ntb_db_clear_mask(struct ntb_dev *ntb, u64 db_bits)
{
	return ntb->ops->db_clear_mask(ntb, db_bits);
}

/**
 * ntb_peer_db_addr() - address and size of the peer doorbell register
 * @ntb:	NTB device context.
 * @db_addr:	OUT - The address of the peer doorbell register.
 * @db_size:	OUT - The number of bytes to write the peer doorbell register.
 *
 * Return the address of the peer doorbell register.  This may be used, for
 * example, by drivers that offload memory copy operations to a dma engine.
 * The drivers may wish to ring the peer doorbell at the completion of memory
 * copy operations.  For efficiency, and to simplify ordering of operations
 * between the dma memory copies and the ringing doorbell, the driver may
 * append one additional dma memory copy with the doorbell register as the
 * destination, after the memory copy operations.
 *
 * This is unusual, and hardware may not be suitable to implement it.
 *
 * Return: Zero on success, otherwise an error number.
 */
static inline int ntb_peer_db_addr(struct ntb_dev *ntb,
				   phys_addr_t *db_addr,
				   resource_size_t *db_size)
{
	if (!ntb->ops->peer_db_addr)
		return -EINVAL;

	return ntb->ops->peer_db_addr(ntb, db_addr, db_size);
}

/**
 * ntb_peer_db_read() - read the peer doorbell register
 * @ntb:	NTB device context.
 *
 * Read the peer doorbell register, and return the bits that are set.
 *
 * This is unusual, and hardware may not support it.
 *
 * Return: The bits currently set in the peer doorbell register.
 */
static inline u64 ntb_peer_db_read(struct ntb_dev *ntb)
{
	if (!ntb->ops->peer_db_read)
		return 0;

	return ntb->ops->peer_db_read(ntb);
}

/**
 * ntb_peer_db_set() - set bits in the peer doorbell register
 * @ntb:	NTB device context.
 * @db_bits:	Doorbell bits to set.
 *
 * Set bits in the peer doorbell register, which may generate a peer doorbell
 * interrupt.  Bits that were already set must remain set.
 *
 * Return: Zero on success, otherwise an error number.
 */
static inline int ntb_peer_db_set(struct ntb_dev *ntb, u64 db_bits)
{
	return ntb->ops->peer_db_set(ntb, db_bits);
}

/**
 * ntb_peer_db_clear() - clear bits in the peer doorbell register
 * @ntb:	NTB device context.
 * @db_bits:	Doorbell bits to clear.
 *
 * Clear bits in the peer doorbell register, arming the bits for the next
 * doorbell.
 *
 * This is unusual, and hardware may not support it.
 *
 * Return: Zero on success, otherwise an error number.
 */
static inline int ntb_peer_db_clear(struct ntb_dev *ntb, u64 db_bits)
{
	if (!ntb->ops->db_clear)
		return -EINVAL;

	return ntb->ops->peer_db_clear(ntb, db_bits);
}

/**
 * ntb_peer_db_read_mask() - read the peer doorbell mask
 * @ntb:	NTB device context.
 *
 * Read the peer doorbell mask register, and return the bits that are set.
 *
 * This is unusual, and hardware may not support it.
 *
 * Return: The bits currently set in the peer doorbell mask register.
 */
static inline u64 ntb_peer_db_read_mask(struct ntb_dev *ntb)
{
	if (!ntb->ops->db_read_mask)
		return 0;

	return ntb->ops->peer_db_read_mask(ntb);
}

/**
 * ntb_peer_db_set_mask() - set bits in the peer doorbell mask
 * @ntb:	NTB device context.
 * @db_bits:	Doorbell mask bits to set.
 *
 * Set bits in the peer doorbell mask register, preventing doorbell interrupts
 * from being generated for those doorbell bits.  Bits that were already set
 * must remain set.
 *
 * This is unusual, and hardware may not support it.
 *
 * Return: Zero on success, otherwise an error number.
 */
static inline int ntb_peer_db_set_mask(struct ntb_dev *ntb, u64 db_bits)
{
	if (!ntb->ops->db_set_mask)
		return -EINVAL;

	return ntb->ops->peer_db_set_mask(ntb, db_bits);
}

/**
 * ntb_peer_db_clear_mask() - clear bits in the peer doorbell mask
 * @ntb:	NTB device context.
 * @db_bits:	Doorbell bits to clear.
 *
 * Clear bits in the peer doorbell mask register, allowing doorbell interrupts
 * from being generated for those doorbell bits.  If the hardware does not
 * generate the interrupt on clearing the mask bit, then the driver should not
 * implement this function!
 *
 * This is unusual, and hardware may not support it.
 *
 * Return: Zero on success, otherwise an error number.
 */
static inline int ntb_peer_db_clear_mask(struct ntb_dev *ntb, u64 db_bits)
{
	if (!ntb->ops->db_clear_mask)
		return -EINVAL;

	return ntb->ops->peer_db_clear_mask(ntb, db_bits);
}

/**
 * ntb_spad_is_unsafe() - check if it is safe to use the hardware scratchpads
 * @ntb:	NTB device context.
 *
 * It is possible for some ntb hardware to be affected by errata.  Hardware
 * drivers can advise clients to avoid using scratchpads.  Clients may ignore
 * this advice, though caution is recommended.
 *
 * Return: Zero if it is safe to use scratchpads, or One if it is not safe.
 */
static inline int ntb_spad_is_unsafe(struct ntb_dev *ntb)
{
	if (!ntb->ops->spad_is_unsafe)
		return 0;

	return ntb->ops->spad_is_unsafe(ntb);
}

/**
 * ntb_mw_count() - get the number of scratchpads
 * @ntb:	NTB device context.
 *
 * Hardware and topology may support a different number of scratchpads.
 *
 * Asynchronous hardware may not support it.
 *
 * Return: the number of scratchpads.
 */
static inline int ntb_spad_count(struct ntb_dev *ntb)
{
	if (!ntb->ops->spad_count)
		return -EINVAL;

	return ntb->ops->spad_count(ntb);
}

/**
 * ntb_spad_read() - read the local scratchpad register
 * @ntb:	NTB device context.
 * @idx:	Scratchpad index.
 *
 * Read the local scratchpad register, and return the value.
 *
 * Asynchronous hardware may not support it.
 *
 * Return: The value of the local scratchpad register.
 */
static inline u32 ntb_spad_read(struct ntb_dev *ntb, int idx)
{
	if (!ntb->ops->spad_read)
		return 0;

	return ntb->ops->spad_read(ntb, idx);
}

/**
 * ntb_spad_write() - write the local scratchpad register
 * @ntb:	NTB device context.
 * @idx:	Scratchpad index.
 * @val:	Scratchpad value.
 *
 * Write the value to the local scratchpad register.
 *
 * Asynchronous hardware may not support it.
 *
 * Return: Zero on success, otherwise an error number.
 */
static inline int ntb_spad_write(struct ntb_dev *ntb, int idx, u32 val)
{
	if (!ntb->ops->spad_write)
		return -EINVAL;

	return ntb->ops->spad_write(ntb, idx, val);
}

/**
 * ntb_peer_spad_addr() - address of the peer scratchpad register
 * @ntb:	NTB device context.
 * @idx:	Scratchpad index.
 * @spad_addr:	OUT - The address of the peer scratchpad register.
 *
 * Return the address of the peer doorbell register.  This may be used, for
 * example, by drivers that offload memory copy operations to a dma engine.
 *
 * Asynchronous hardware may not support it.
 *
 * Return: Zero on success, otherwise an error number.
 */
static inline int ntb_peer_spad_addr(struct ntb_dev *ntb, int idx,
				     phys_addr_t *spad_addr)
{
	if (!ntb->ops->peer_spad_addr)
		return -EINVAL;

	return ntb->ops->peer_spad_addr(ntb, idx, spad_addr);
}

/**
 * ntb_peer_spad_read() - read the peer scratchpad register
 * @ntb:	NTB device context.
 * @idx:	Scratchpad index.
 *
 * Read the peer scratchpad register, and return the value.
 *
 * Asynchronous hardware may not support it.
 *
 * Return: The value of the local scratchpad register.
 */
static inline u32 ntb_peer_spad_read(struct ntb_dev *ntb, int idx)
{
	if (!ntb->ops->peer_spad_read)
		return 0;

	return ntb->ops->peer_spad_read(ntb, idx);
}

/**
 * ntb_peer_spad_write() - write the peer scratchpad register
 * @ntb:	NTB device context.
 * @idx:	Scratchpad index.
 * @val:	Scratchpad value.
 *
 * Write the value to the peer scratchpad register.
 *
 * Asynchronous hardware may not support it.
 *
 * Return: Zero on success, otherwise an error number.
 */
static inline int ntb_peer_spad_write(struct ntb_dev *ntb, int idx, u32 val)
{
	if (!ntb->ops->peer_spad_write)
		return -EINVAL;

	return ntb->ops->peer_spad_write(ntb, idx, val);
}

/**
 * ntb_msg_post() - post the message to the peer
 * @ntb:	NTB device context.
 * @msg:	Message
 *
 * Post the message to a peer. It shall be delivered to the peer by the
 * corresponding hardware method. The peer should be notified about the new
 * message by calling the ntb_msg_event() handler of NTB_MSG_NEW event type.
 * If delivery is fails for some reasong the local node will get NTB_MSG_FAIL
 * event. Otherwise the NTB_MSG_SENT is emitted.
 *
 * Synchronous hardware may not support it.
 *
 * Return: Zero on success, otherwise an error number.
 */
static inline int ntb_msg_post(struct ntb_dev *ntb, struct ntb_msg *msg)
{
	if (!ntb->ops->msg_post)
		return -EINVAL;

	return ntb->ops->msg_post(ntb, msg);
}

/**
 * ntb_msg_size() - size of the message data
 * @ntb:	NTB device context.
 *
 * Different hardware may support different number of message registers. This
 * callback shall return the number of those used for data sending and
 * receiving including the type field.
 *
 * Synchronous hardware may not support it.
 *
 * Return: Zero on success, otherwise an error number.
 */
static inline int ntb_msg_size(struct ntb_dev *ntb)
{
	if (!ntb->ops->msg_size)
		return 0;

	return ntb->ops->msg_size(ntb);
}

#endif
