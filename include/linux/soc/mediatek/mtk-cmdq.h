/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2018 MediaTek Inc.
 *
 */

#ifndef __MTK_CMDQ_H__
#define __MTK_CMDQ_H__

#include <linux/mailbox_client.h>
#include <linux/mailbox/mtk-cmdq-mailbox.h>

/** cmdq event maximum */
#define CMDQ_MAX_EVENT				0x3ff

struct cmdq_pkt;

struct cmdq_client {
	struct mbox_client client;
	struct mbox_chan *chan;
};

/**
 * cmdq_mbox_create() - create CMDQ mailbox client and channel
 * @dev:	device of CMDQ mailbox client
 * @index:	index of CMDQ mailbox channel
 *
 * Return: CMDQ mailbox client pointer
 */
struct cmdq_client *cmdq_mbox_create(struct device *dev, int index);

/**
 * cmdq_mbox_destroy() - destroy CMDQ mailbox client and channel
 * @client:	the CMDQ mailbox client
 */
void cmdq_mbox_destroy(struct cmdq_client *client);

/**
 * cmdq_pkt_create() - create a CMDQ packet
 * @pkt_ptr:	CMDQ packet pointer to retrieve cmdq_pkt
 *
 * Return: 0 for success; else the error code is returned
 */
int cmdq_pkt_create(struct cmdq_pkt **pkt_ptr);

/**
 * cmdq_pkt_destroy() - destroy the CMDQ packet
 * @pkt:	the CMDQ packet
 */
void cmdq_pkt_destroy(struct cmdq_pkt *pkt);

/**
 * cmdq_pkt_realloc_cmd_buffer() - reallocate command buffer for CMDQ packet
 * @pkt:	the CMDQ packet
 * @size:	the request size
 * Return: 0 for success; else the error code is returned
 */
int cmdq_pkt_realloc_cmd_buffer(struct cmdq_pkt *pkt, size_t size);

/**
 * cmdq_pkt_write() - append write command to the CMDQ packet
 * @pkt:	the CMDQ packet
 * @value:	the specified target register value
 * @subsys:	the CMDQ sub system code
 * @offset:	register offset from CMDQ sub system
 *
 * Return: 0 for success; else the error code is returned
 */
int cmdq_pkt_write(struct cmdq_pkt *pkt, u32 value, u32 subsys, u32 offset);

/**
 * cmdq_pkt_write_mask() - append write command with mask to the CMDQ packet
 * @pkt:	the CMDQ packet
 * @value:	the specified target register value
 * @subsys:	the CMDQ sub system code
 * @offset:	register offset from CMDQ sub system
 * @mask:	the specified target register mask
 *
 * Return: 0 for success; else the error code is returned
 */
int cmdq_pkt_write_mask(struct cmdq_pkt *pkt, u32 value,
			u32 subsys, u32 offset, u32 mask);

/**
 * cmdq_pkt_wfe() - append wait for event command to the CMDQ packet
 * @pkt:	the CMDQ packet
 * @event:	the desired event type to "wait and CLEAR"
 *
 * Return: 0 for success; else the error code is returned
 */
int cmdq_pkt_wfe(struct cmdq_pkt *pkt, u32 event);

/**
 * cmdq_pkt_clear_event() - append clear event command to the CMDQ packet
 * @pkt:	the CMDQ packet
 * @event:	the desired event to be cleared
 *
 * Return: 0 for success; else the error code is returned
 */
int cmdq_pkt_clear_event(struct cmdq_pkt *pkt, u32 event);

/**
 * cmdq_pkt_flush() - trigger CMDQ to execute the CMDQ packet
 * @client:	the CMDQ mailbox client
 * @pkt:	the CMDQ packet
 *
 * Return: 0 for success; else the error code is returned
 *
 * Trigger CMDQ to execute the CMDQ packet. Note that this is a
 * synchronous flush function. When the function returned, the recorded
 * commands have been done.
 */
int cmdq_pkt_flush(struct cmdq_client *client, struct cmdq_pkt *pkt);

/**
 * cmdq_pkt_flush_async() - trigger CMDQ to asynchronously execute the CMDQ
 *                          packet and call back at the end of done packet
 * @client:	the CMDQ mailbox client
 * @pkt:	the CMDQ packet
 * @cb:		called at the end of done packet
 * @data:	this data will pass back to cb
 *
 * Return: 0 for success; else the error code is returned
 *
 * Trigger CMDQ to asynchronously execute the CMDQ packet and call back
 * at the end of done packet. Note that this is an ASYNC function. When the
 * function returned, it may or may not be finished.
 */
int cmdq_pkt_flush_async(struct cmdq_client *client, struct cmdq_pkt *pkt,
			 cmdq_async_flush_cb cb, void *data);

#endif	/* __MTK_CMDQ_H__ */
