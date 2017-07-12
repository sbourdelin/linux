/*
 * Huawei HiNIC PCI Express Linux driver
 * Copyright(c) 2017 Huawei Technologies Co., Ltd
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 *
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/errno.h>
#include <linux/pci.h>
#include <linux/device.h>
#include <linux/semaphore.h>
#include <linux/completion.h>
#include <linux/slab.h>
#include <asm/barrier.h>

#include "hinic_hw_if.h"
#include "hinic_hw_eqs.h"
#include "hinic_hw_api_cmd.h"
#include "hinic_hw_mgmt.h"
#include "hinic_hw_dev.h"

#define SYNC_MSG_ID_MASK		0x1FF

#define SYNC_MSG_ID(pf_to_mgmt)		((pf_to_mgmt)->sync_msg_id)

#define SYNC_MSG_ID_INC(pf_to_mgmt)	(SYNC_MSG_ID(pf_to_mgmt) = \
					((SYNC_MSG_ID(pf_to_mgmt) + 1) & \
					 SYNC_MSG_ID_MASK))

#define MSG_SZ_IS_VALID(in_size)	((in_size) <= MAX_MSG_SZ)

#define MGMT_MSG_SIZE_MIN		20
#define MGMT_MSG_SIZE_STEP		16
#define	MGMT_MSG_RSVD_FOR_DEV		8

#define SEGMENT_LEN			48

#define MAX_PF_MGMT_BUF_SIZE		2048

/* Data should be SEG LEN size aligned */
#define MAX_MSG_SZ			2016

#define MSG_NOT_RESP			0xFFFF

#define MGMT_MSG_TIMEOUT		1000

#define mgmt_to_pfhwdev(pf_mgmt)	\
		container_of(pf_mgmt, struct hinic_pfhwdev, pf_to_mgmt)

enum msg_segment_type {
	NOT_LAST_SEGMENT = 0,
	LAST_SEGMENT = 1,
};

enum mgmt_direction_type {
	MGMT_DIRECT_SEND = 0,
	MGMT_RESP = 1,
};

enum msg_ack_type {
	MSG_ACK = 0,
	MSG_NO_ACK = 1,
};

/**
 * hinic_register_mgmt_msg_cb - register msg handler for a msg from a module
 * @pf_to_mgmt: PF to MGMT channel
 * @mod: module in the chip that this handler will handle its messages
 * @handle: private data for the callback
 * @callback: the handler that will handle messages
 **/
void hinic_register_mgmt_msg_cb(struct hinic_pf_to_mgmt *pf_to_mgmt,
				enum hinic_mod_type mod,
				void *handle,
				void (*callback)(void *handle,
						 u8 cmd, void *buf_in,
						 u16 in_size, void *buf_out,
						 u16 *out_size))
{
	struct hinic_mgmt_cb *mgmt_cb = &pf_to_mgmt->mgmt_cb[mod];

	mgmt_cb->cb = callback;
	mgmt_cb->handle = handle;
	mgmt_cb->state = HINIC_MGMT_CB_ENABLED;
}

/**
 * hinic_unregister_mgmt_msg_cb - unregister msg handler for a msg from a module
 * @pf_to_mgmt: PF to MGMT channel
 * @mod: module in the chip that this handler handles its messages
 **/
void hinic_unregister_mgmt_msg_cb(struct hinic_pf_to_mgmt *pf_to_mgmt,
				  enum hinic_mod_type mod)
{
	struct hinic_mgmt_cb *mgmt_cb = &pf_to_mgmt->mgmt_cb[mod];

	mgmt_cb->state &= ~HINIC_MGMT_CB_ENABLED;

	while (mgmt_cb->state & HINIC_MGMT_CB_RUNNING)
		schedule();

	mgmt_cb->cb = NULL;
}

/**
 * prepare_header - prepare the header of the message
 * @pf_to_mgmt: PF to MGMT channel
 * @header: pointer of the header to prepare
 * @msg_len: the length of the message
 * @mod: module in the chip that will get the message
 * @ack_type: ask for response
 * @direction: the direction of the message
 * @cmd: command of the message
 * @msg_id: message id
 **/
static void prepare_header(struct hinic_pf_to_mgmt *pf_to_mgmt,
			   u64 *header, int msg_len, enum hinic_mod_type mod,
			   enum msg_ack_type ack_type,
			   enum mgmt_direction_type direction,
			   u16 cmd, u16 msg_id)
{
	struct hinic_hwif *hwif = pf_to_mgmt->hwif;

	*header = HINIC_MSG_HEADER_SET(msg_len, MSG_LEN)		|
		  HINIC_MSG_HEADER_SET(mod, MODULE)			|
		  HINIC_MSG_HEADER_SET(SEGMENT_LEN, SEG_LEN)		|
		  HINIC_MSG_HEADER_SET(ack_type, NO_ACK)		|
		  HINIC_MSG_HEADER_SET(0, ASYNC_MGMT_TO_PF)		|
		  HINIC_MSG_HEADER_SET(0, SEQID)			|
		  HINIC_MSG_HEADER_SET(LAST_SEGMENT, LAST)		|
		  HINIC_MSG_HEADER_SET(direction, DIRECTION)		|
		  HINIC_MSG_HEADER_SET(cmd, CMD)			|
		  HINIC_MSG_HEADER_SET(HINIC_HWIF_PCI_INTF(hwif), PCI_INTF) |
		  HINIC_MSG_HEADER_SET(HINIC_HWIF_PF_IDX(hwif), PF_IDX) |
		  HINIC_MSG_HEADER_SET(msg_id, MSG_ID);
}

/**
 * prepare_mgmt_cmd - prepare the mgmt command
 * @mgmt_cmd: pointer to the command to prepare
 * @header: pointer of the header for the message
 * @msg: the data of the message
 * @msg_len: the length of the message
 **/
static void prepare_mgmt_cmd(void *mgmt_cmd, u64 *header, void *msg,
			     u16 msg_len)
{
	memset(mgmt_cmd, 0, MGMT_MSG_RSVD_FOR_DEV);

	mgmt_cmd += MGMT_MSG_RSVD_FOR_DEV;
	memcpy(mgmt_cmd, header, sizeof(*header));

	mgmt_cmd += sizeof(*header);
	memcpy(mgmt_cmd, msg, msg_len);
}

/**
 * mgmt_msg_len - calculate the total message length
 * @msg_data_len: the length of the message data
 *
 * Return the total message length
 **/
static u16 mgmt_msg_len(u16 msg_data_len)
{
	/* RSVD + HEADER_SIZE + DATA_LEN */
	u16 msg_size = MGMT_MSG_RSVD_FOR_DEV + sizeof(u64) + msg_data_len;

	if (msg_size > MGMT_MSG_SIZE_MIN)
		msg_size = MGMT_MSG_SIZE_MIN +
			   ALIGN((msg_size - MGMT_MSG_SIZE_MIN),
				 MGMT_MSG_SIZE_STEP);
	else
		msg_size = MGMT_MSG_SIZE_MIN;

	return msg_size;
}

/**
 * send_msg_to_mgmt - send message to mgmt by API CMD
 * @pf_to_mgmt: PF to MGMT channel
 * @mod: module in the chip that will get the message
 * @cmd: command of the message
 * @msg: the msg data
 * @msg_len: the msg data length
 * @ack_type: ask for response
 * @direction: the direction of the original message
 * @resp_msg_id: msg id to response for
 *
 * Return 0 - Success, negative - Failure
 **/
static int send_msg_to_mgmt(struct hinic_pf_to_mgmt *pf_to_mgmt,
			    enum hinic_mod_type mod, u8 cmd,
			    void *msg, u16 msg_len,
			    enum msg_ack_type ack_type,
			    enum mgmt_direction_type direction,
			    u16 resp_msg_id)
{
	struct hinic_api_cmd_chain *chain;
	void *mgmt_cmd = pf_to_mgmt->sync_msg_buf;
	u64 header;
	u16 msg_id, cmd_size = mgmt_msg_len(msg_len);

	msg_id = SYNC_MSG_ID(pf_to_mgmt);

	if (direction == MGMT_RESP) {
		prepare_header(pf_to_mgmt, &header, msg_len, mod, ack_type,
			       direction, cmd, resp_msg_id);
	} else {
		SYNC_MSG_ID_INC(pf_to_mgmt);
		prepare_header(pf_to_mgmt, &header, msg_len, mod, ack_type,
			       direction, cmd, msg_id);
	}

	prepare_mgmt_cmd(mgmt_cmd, &header, msg, msg_len);

	chain = pf_to_mgmt->cmd_chain[HINIC_API_CMD_WRITE_TO_MGMT_CPU];

	return hinic_api_cmd_write(chain, HINIC_NODE_ID_MGMT, mgmt_cmd,
				   cmd_size);
}

/**
 * msg_to_mgmt_sync - send sync message to mgmt
 * @pf_to_mgmt: PF to MGMT channel
 * @mod: module in the chip that will get the message
 * @cmd: command of the message
 * @buf_in: the msg data
 * @in_size: the msg data length
 * @buf_out: response
 * @out_size: response length
 * @direction: the direction of the original message
 * @resp_msg_id: msg id to response for
 *
 * Return 0 - Success, negative - Failure
 **/
static int msg_to_mgmt_sync(struct hinic_pf_to_mgmt *pf_to_mgmt,
			    enum hinic_mod_type mod, u8 cmd,
			    void *buf_in, u16 in_size,
			    void *buf_out, u16 *out_size,
			    enum mgmt_direction_type direction,
			    u16 resp_msg_id)
{
	struct hinic_hwif *hwif = pf_to_mgmt->hwif;
	struct pci_dev *pdev = hwif->pdev;
	struct hinic_recv_msg *recv_msg;
	struct completion *recv_done;
	u16 msg_id;
	int err;

	/* Lock the sync_msg_buf */
	down(&pf_to_mgmt->sync_msg_lock);

	recv_msg = &pf_to_mgmt->recv_resp_msg_from_mgmt;
	recv_done = &recv_msg->recv_done;

	if (resp_msg_id == MSG_NOT_RESP)
		msg_id = SYNC_MSG_ID(pf_to_mgmt);
	else
		msg_id = resp_msg_id;

	init_completion(recv_done);

	err = send_msg_to_mgmt(pf_to_mgmt, mod, cmd, buf_in, in_size,
			       MSG_ACK, direction, resp_msg_id);
	if (err) {
		dev_err(&pdev->dev, "Failed to send sync msg to mgmt\n");
		goto unlock_sync_msg;
	}

	if (!wait_for_completion_timeout(recv_done, MGMT_MSG_TIMEOUT)) {
		dev_err(&pdev->dev, "MGMT timeout, MSG id = %d\n", msg_id);
		err = -ETIMEDOUT;
		goto unlock_sync_msg;
	}

	smp_rmb();	/* verify reading after completion */

	if (recv_msg->msg_id != msg_id) {
		dev_err(&pdev->dev, "incorrect MSG for id = %d\n", msg_id);
		err = -EFAULT;
		goto unlock_sync_msg;
	}

	if ((buf_out) && (recv_msg->msg_len <= MAX_PF_MGMT_BUF_SIZE)) {
		memcpy(buf_out, recv_msg->msg, recv_msg->msg_len);
		*out_size = recv_msg->msg_len;
	}

unlock_sync_msg:
	up(&pf_to_mgmt->sync_msg_lock);
	return err;
}

/**
 * msg_to_mgmt_async - send message to mgmt without response
 * @pf_to_mgmt: PF to MGMT channel
 * @mod: module in the chip that will get the message
 * @cmd: command of the message
 * @buf_in: the msg data
 * @in_size: the msg data length
 * @direction: the direction of the original message
 * @resp_msg_id: msg id to response for
 *
 * Return 0 - Success, negative - Failure
 **/
static int msg_to_mgmt_async(struct hinic_pf_to_mgmt *pf_to_mgmt,
			     enum hinic_mod_type mod, u8 cmd,
			     void *buf_in, u16 in_size,
			     enum mgmt_direction_type direction,
			     u16 resp_msg_id)
{
	int err;

	/* Lock the sync_msg_buf */
	down(&pf_to_mgmt->sync_msg_lock);

	err = send_msg_to_mgmt(pf_to_mgmt, mod, cmd, buf_in, in_size,
			       MSG_NO_ACK, direction, resp_msg_id);

	up(&pf_to_mgmt->sync_msg_lock);

	return err;
}

/**
 * hinic_msg_to_mgmt - send message to mgmt
 * @pf_to_mgmt: PF to MGMT channel
 * @mod: module in the chip that will get the message
 * @cmd: command of the message
 * @buf_in: the msg data
 * @in_size: the msg data length
 * @buf_out: response
 * @out_size: returned response length
 * @sync: sync msg or async msg
 *
 * Return 0 - Success, negative - Failure
 **/
int hinic_msg_to_mgmt(struct hinic_pf_to_mgmt *pf_to_mgmt,
		      enum hinic_mod_type mod, u8 cmd,
		      void *buf_in, u16 in_size, void *buf_out, u16 *out_size,
		      enum hinic_mgmt_msg_type sync)
{
	if (sync != HINIC_MGMT_MSG_SYNC) {
		pr_err("Invalid MGMT msg type\n");
		return -EINVAL;
	}

	if (!MSG_SZ_IS_VALID(in_size)) {
		pr_err("Invalid MGMT msg buffer size\n");
		return -EINVAL;
	}

	return msg_to_mgmt_sync(pf_to_mgmt, mod, cmd, buf_in, in_size,
				buf_out, out_size, MGMT_DIRECT_SEND,
				MSG_NOT_RESP);
}

/**
 * mgmt_recv_msg_handler - handler for message from mgmt cpu
 * @pf_to_mgmt: PF to MGMT channel
 * @recv_msg: received message details
 **/
static void mgmt_recv_msg_handler(struct hinic_pf_to_mgmt *pf_to_mgmt,
				  struct hinic_recv_msg *recv_msg)
{
	struct hinic_hwif *hwif = pf_to_mgmt->hwif;
	struct pci_dev *pdev = hwif->pdev;
	void *handle, *buf_out = recv_msg->buf_out;
	enum hinic_mod_type mod = recv_msg->mod;
	struct hinic_mgmt_cb *mgmt_cb;
	unsigned long cb_state;
	u16 out_size = 0;

	if (mod >= HINIC_MOD_MAX) {
		dev_err(&pdev->dev, "Unknown MGMT MSG module = %d\n", mod);
		return;
	}

	mgmt_cb = &pf_to_mgmt->mgmt_cb[mod];
	handle = mgmt_cb->handle;

	cb_state = cmpxchg(&mgmt_cb->state,
			   HINIC_MGMT_CB_ENABLED,
			   HINIC_MGMT_CB_ENABLED | HINIC_MGMT_CB_RUNNING);

	if ((cb_state == HINIC_MGMT_CB_ENABLED) && (mgmt_cb->cb))
		mgmt_cb->cb(handle, recv_msg->cmd,
			    recv_msg->msg, recv_msg->msg_len,
			    buf_out, &out_size);
	else
		dev_err(&pdev->dev, "No MGMT msg handler, mod = %d\n", mod);

	mgmt_cb->state &= ~HINIC_MGMT_CB_RUNNING;

	if (!recv_msg->async_mgmt_to_pf)
		/* MGMT sent sync msg, send the response */
		msg_to_mgmt_async(pf_to_mgmt, mod, recv_msg->cmd,
				  buf_out, out_size, MGMT_RESP,
				  recv_msg->msg_id);
}

/**
 * mgmt_resp_msg_handler - handler for a response message from mgmt cpu
 * @pf_to_mgmt: PF to MGMT channel
 * @recv_msg: received message details
 **/
static void mgmt_resp_msg_handler(struct hinic_pf_to_mgmt *pf_to_mgmt,
				  struct hinic_recv_msg *recv_msg)
{
	wmb();	/* verify writing all, before reading */

	complete(&recv_msg->recv_done);
}

/**
 * recv_mgmt_msg_handler - handler for a message from mgmt cpu
 * @pf_to_mgmt: PF to MGMT channel
 * @header: the header of the message
 * @recv_msg: received message details
 **/
static void recv_mgmt_msg_handler(struct hinic_pf_to_mgmt *pf_to_mgmt,
				  u64 *header, struct hinic_recv_msg *recv_msg)
{
	struct hinic_hwif *hwif = pf_to_mgmt->hwif;
	struct pci_dev *pdev = hwif->pdev;
	void *msg_body = (void *)header + sizeof(*header);
	int seq_id, seg_len;

	seq_id = HINIC_MSG_HEADER_GET(*header, SEQID);
	seg_len = HINIC_MSG_HEADER_GET(*header, SEG_LEN);

	if (seq_id >= (MAX_MSG_SZ / SEGMENT_LEN)) {
		dev_err(&pdev->dev, "recv big mgmt msg\n");
		return;
	}

	memcpy(recv_msg->msg + seq_id * SEGMENT_LEN, msg_body, seg_len);

	if (!HINIC_MSG_HEADER_GET(*header, LAST))
		return;

	recv_msg->cmd = HINIC_MSG_HEADER_GET(*header, CMD);
	recv_msg->mod = HINIC_MSG_HEADER_GET(*header, MODULE);
	recv_msg->async_mgmt_to_pf = HINIC_MSG_HEADER_GET(*header,
							  ASYNC_MGMT_TO_PF);
	recv_msg->msg_len = HINIC_MSG_HEADER_GET(*header, MSG_LEN);
	recv_msg->msg_id = HINIC_MSG_HEADER_GET(*header, MSG_ID);

	if (HINIC_MSG_HEADER_GET(*header, DIRECTION) == MGMT_RESP)
		mgmt_resp_msg_handler(pf_to_mgmt, recv_msg);
	else
		mgmt_recv_msg_handler(pf_to_mgmt, recv_msg);
}

/**
 * mgmt_msg_aeqe_handler - handler for a mgmt message event
 * @handle: PF to MGMT channel
 * @data: the header of the message
 * @size: unused
 **/
static void mgmt_msg_aeqe_handler(void *handle, void *data, u8 size)
{
	struct hinic_pf_to_mgmt *pf_to_mgmt = handle;
	struct hinic_recv_msg *recv_msg;
	u64 *header = (u64 *)data;

	recv_msg = HINIC_MSG_HEADER_GET(*header, DIRECTION) ==
		   MGMT_DIRECT_SEND ?
		   &pf_to_mgmt->recv_msg_from_mgmt :
		   &pf_to_mgmt->recv_resp_msg_from_mgmt;

	recv_mgmt_msg_handler(pf_to_mgmt, header, recv_msg);
}

/**
 * alloc_recv_msg - allocate receive message memory
 * @recv_msg: pointer that will hold the allocated data
 *
 * Return 0 - Success, negative - Failure
 **/
static int alloc_recv_msg(struct hinic_recv_msg *recv_msg)
{
	int err;

	recv_msg->msg = kzalloc(MAX_PF_MGMT_BUF_SIZE, GFP_KERNEL);
	if (!recv_msg->msg)
		return -ENOMEM;

	recv_msg->buf_out = kzalloc(MAX_PF_MGMT_BUF_SIZE, GFP_KERNEL);
	if (!recv_msg->buf_out) {
		err = -ENOMEM;
		goto alloc_buf_out_err;
	}

	return 0;

alloc_buf_out_err:
	kfree(recv_msg->msg);
	return err;
}

/**
 * free_recv_msg - free receive message memory
 * @recv_msg: pointer that holds the allocated data
 **/
static void free_recv_msg(struct hinic_recv_msg *recv_msg)
{
	kfree(recv_msg->buf_out);
	kfree(recv_msg->msg);
}

/**
 * alloc_msg_buf - allocate all the message buffers of PF to MGMT channel
 * @pf_to_mgmt: PF to MGMT channel
 *
 * Return 0 - Success, negative - Failure
 **/
static int alloc_msg_buf(struct hinic_pf_to_mgmt *pf_to_mgmt)
{
	int err;

	err = alloc_recv_msg(&pf_to_mgmt->recv_msg_from_mgmt);
	if (err) {
		pr_err("Failed to allocate recv msg\n");
		return err;
	}

	err = alloc_recv_msg(&pf_to_mgmt->recv_resp_msg_from_mgmt);
	if (err) {
		pr_err("Failed to allocate resp recv msg\n");
		goto alloc_msg_for_resp_err;
	}

	pf_to_mgmt->sync_msg_buf = kzalloc(MAX_PF_MGMT_BUF_SIZE, GFP_KERNEL);
	if (!pf_to_mgmt->sync_msg_buf) {
		err = -ENOMEM;
		goto sync_msg_buf_err;
	}

	return 0;

sync_msg_buf_err:
	free_recv_msg(&pf_to_mgmt->recv_resp_msg_from_mgmt);

alloc_msg_for_resp_err:
	free_recv_msg(&pf_to_mgmt->recv_msg_from_mgmt);
	return err;
}

/**
 * free_msg_buf - free all the message buffers of PF to MGMT channel
 * @pf_to_mgmt: PF to MGMT channel
 **/
static void free_msg_buf(struct hinic_pf_to_mgmt *pf_to_mgmt)
{
	kfree(pf_to_mgmt->sync_msg_buf);

	free_recv_msg(&pf_to_mgmt->recv_resp_msg_from_mgmt);
	free_recv_msg(&pf_to_mgmt->recv_msg_from_mgmt);
}

/**
 * hinic_pf_to_mgmt_init - initialize PF to MGMT channel
 * @pf_to_mgmt: PF to MGMT channel
 * @hwif: HW interface the PF to MGMT will use for accessing HW
 *
 * Return 0 - Success, negative - Failure
 **/
int hinic_pf_to_mgmt_init(struct hinic_pf_to_mgmt *pf_to_mgmt,
			  struct hinic_hwif *hwif)
{
	struct hinic_pfhwdev *pfhwdev = mgmt_to_pfhwdev(pf_to_mgmt);
	struct hinic_hwdev *hwdev = &pfhwdev->hwdev;
	int err;

	pf_to_mgmt->hwif = hwif;

	sema_init(&pf_to_mgmt->sync_msg_lock, 1);
	pf_to_mgmt->sync_msg_id = 0;

	err = alloc_msg_buf(pf_to_mgmt);
	if (err) {
		pr_err("Failed to allocate msg buffers\n");
		return err;
	}

	err = hinic_api_cmd_init(hwif, pf_to_mgmt->cmd_chain);
	if (err) {
		pr_err("Failed to initialize cmd chains\n");
		goto api_cmd_init_err;
	}

	hinic_aeq_register_hw_cb(&hwdev->aeqs, HINIC_MSG_FROM_MGMT_CPU,
				 pf_to_mgmt,
				 mgmt_msg_aeqe_handler);

	return 0;

api_cmd_init_err:
	free_msg_buf(pf_to_mgmt);
	return err;
}

/**
 * hinic_pf_to_mgmt_free - free PF to MGMT channel
 * @pf_to_mgmt: PF to MGMT channel
 **/
void hinic_pf_to_mgmt_free(struct hinic_pf_to_mgmt *pf_to_mgmt)
{
	struct hinic_pfhwdev *pfhwdev = mgmt_to_pfhwdev(pf_to_mgmt);
	struct hinic_hwdev *hwdev = &pfhwdev->hwdev;

	hinic_aeq_unregister_hw_cb(&hwdev->aeqs, HINIC_MSG_FROM_MGMT_CPU);
	hinic_api_cmd_free(pf_to_mgmt->cmd_chain);
	free_msg_buf(pf_to_mgmt);
}
