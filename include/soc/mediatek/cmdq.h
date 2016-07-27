/*
 * Copyright (c) 2015 MediaTek Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#ifndef __MTK_CMDQ_H__
#define __MTK_CMDQ_H__

#include <linux/mailbox_client.h>
#include <linux/platform_device.h>
#include <linux/types.h>

/* display events in command queue(CMDQ) */
enum cmdq_event {
	/* Display start of frame(SOF) events */
	CMDQ_EVENT_DISP_OVL0_SOF,
	CMDQ_EVENT_DISP_OVL1_SOF,
	CMDQ_EVENT_DISP_RDMA0_SOF,
	CMDQ_EVENT_DISP_RDMA1_SOF,
	CMDQ_EVENT_DISP_RDMA2_SOF,
	CMDQ_EVENT_DISP_WDMA0_SOF,
	CMDQ_EVENT_DISP_WDMA1_SOF,
	/* Display end of frame(EOF) events */
	CMDQ_EVENT_DISP_OVL0_EOF,
	CMDQ_EVENT_DISP_OVL1_EOF,
	CMDQ_EVENT_DISP_RDMA0_EOF,
	CMDQ_EVENT_DISP_RDMA1_EOF,
	CMDQ_EVENT_DISP_RDMA2_EOF,
	CMDQ_EVENT_DISP_WDMA0_EOF,
	CMDQ_EVENT_DISP_WDMA1_EOF,
	/* Mutex end of frame(EOF) events */
	CMDQ_EVENT_MUTEX0_STREAM_EOF,
	CMDQ_EVENT_MUTEX1_STREAM_EOF,
	CMDQ_EVENT_MUTEX2_STREAM_EOF,
	CMDQ_EVENT_MUTEX3_STREAM_EOF,
	CMDQ_EVENT_MUTEX4_STREAM_EOF,
	/* Display underrun events */
	CMDQ_EVENT_DISP_RDMA0_UNDERRUN,
	CMDQ_EVENT_DISP_RDMA1_UNDERRUN,
	CMDQ_EVENT_DISP_RDMA2_UNDERRUN,
	/* Keep this at the end */
	CMDQ_MAX_EVENT,
};

struct cmdq_cb_data {
	bool	err;
	void	*data;
};

typedef void (*cmdq_async_flush_cb)(struct cmdq_cb_data data);

struct cmdq_task;

struct cmdq_base {
	int	subsys;
	u32	base;
};

struct cmdq_client {
	struct mbox_client client;
	struct mbox_chan *chan;
};

/**
 * cmdq_register_device() - register device which needs CMDQ
 * @dev:	device for CMDQ to access its registers
 *
 * Return: cmdq_base pointer or NULL for failed
 */
struct cmdq_base *cmdq_register_device(struct device *dev);

/**
 * cmdq_mbox_create() - create CMDQ mailbox client and channel
 * @dev:	device of CMDQ mailbox client
 * @index:	index of CMDQ mailbox channel
 *
 * Return: CMDQ mailbox client pointer
 */
struct cmdq_client *cmdq_mbox_create(struct device *dev, int index);

/**
 * cmdq_task_create() - create CMDQ task
 * @dev:	CMDQ device
 * @task_ptr:	CMDQ task pointer to retrieve cmdq_task
 *
 * Return: 0 for success; else the error code is returned
 */
int cmdq_task_create(struct device *dev, struct cmdq_task **task_ptr);

/**
 * cmdq_task_write() - append write command to the CMDQ task
 * @task:	the CMDQ task
 * @value:	the specified target register value
 * @base:	the CMDQ base
 * @offset:	register offset from module base
 *
 * Return: 0 for success; else the error code is returned
 */
int cmdq_task_write(struct cmdq_task *task, u32 value,
		    struct cmdq_base *base, u32 offset);

/**
 * cmdq_task_write_mask() - append write command with mask to the CMDQ task
 * @task:	the CMDQ task
 * @value:	the specified target register value
 * @base:	the CMDQ base
 * @offset:	register offset from module base
 * @mask:	the specified target register mask
 *
 * Return: 0 for success; else the error code is returned
 */
int cmdq_task_write_mask(struct cmdq_task *task, u32 value,
			 struct cmdq_base *base, u32 offset, u32 mask);

/**
 * cmdq_task_wfe() - append wait for event command to the CMDQ task
 * @task:	the CMDQ task
 * @event:	the desired event type to "wait and CLEAR"
 *
 * Return: 0 for success; else the error code is returned
 */
int cmdq_task_wfe(struct cmdq_task *task, enum cmdq_event event);

/**
 * cmdq_task_clear_event() - append clear event command to the CMDQ task
 * @task:	the CMDQ task
 * @event:	the desired event to be cleared
 *
 * Return: 0 for success; else the error code is returned
 */
int cmdq_task_clear_event(struct cmdq_task *task, enum cmdq_event event);

/**
 * cmdq_task_flush() - trigger CMDQ to execute the recorded commands
 * @client:	the CMDQ mailbox client
 * @task:	the CMDQ task
 *
 * Return: 0 for success; else the error code is returned
 *
 * Trigger CMDQ to execute the recorded commands. Note that this is a
 * synchronous flush function. When the function returned, the recorded
 * commands have been done. CMDQ task will be destroy automatically
 * after CMDQ finish all the recorded commands.
 */
int cmdq_task_flush(struct cmdq_client *client, struct cmdq_task *task);

/**
 * cmdq_task_flush_async() - trigger CMDQ to asynchronously execute the recorded
 *			     commands and call back at the end of ISR
 * @client:	the CMDQ mailbox client
 * @task:	the CMDQ task
 * @cb:		called at the end of CMDQ ISR
 * @data:	this data will pass back to cb
 *
 * Return: 0 for success; else the error code is returned
 *
 * Trigger CMDQ to asynchronously execute the recorded commands and call back
 * at the end of ISR. Note that this is an ASYNC function. When the function
 * returned, it may or may not be finished. CMDQ task will be destroy
 * automatically after CMDQ finish all the recorded commands.
 */
int cmdq_task_flush_async(struct cmdq_client *client, struct cmdq_task *task,
			  cmdq_async_flush_cb cb, void *data);

/**
 * cmdq_mbox_free() - destroy CMDQ mailbox client and channel
 * @client:	the CMDQ mailbox client
 */
void cmdq_mbox_free(struct cmdq_client *client);

#endif	/* __MTK_CMDQ_H__ */
