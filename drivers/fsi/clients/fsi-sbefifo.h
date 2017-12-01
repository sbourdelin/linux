/* SPDX-License-Identifier: GPL-2.0 */
#ifndef DRIVERS_FSI_SBEFIFO_H

#include <linux/timer.h>
#include <linux/types.h>
#include <linux/wait.h>

enum sbefifo_direction {
	sbefifo_up = 0,
	sbefifo_down,
};

enum sbefifo_poll_state {
	sbefifo_poll_wait,
	sbefifo_poll_ready,
	sbefifo_poll_reset,
};

/* Readiness Polling */
struct sbefifo_poll {
	struct timer_list timer;
	wait_queue_head_t wait;
	enum sbefifo_direction dir;
	unsigned long interval;
	bool expire;
	unsigned long expire_at;
	enum sbefifo_poll_state state;
	int rv;
};

struct sbefifo_client;

enum sbefifo_state {
	sbefifo_ready = 0,
	sbefifo_tx,
	sbefifo_interval,
	sbefifo_rx,
	sbefifo_broken,
	sbefifo_dead,
};

/**
 * @eot True when read() dequeues and ACKs an EOT. Set false in the write() path
 */
struct sbefifo {
	struct device *dev;
	struct fsi_device *fsi;
	int id;
	enum sbefifo_state state;
	struct sbefifo_poll poll;
	struct sbefifo_client *curr;
	wait_queue_head_t wait;

	struct list_head entry;
};

enum sbefifo_client_state {
	sbefifo_client_closed = 0,
	sbefifo_client_idle,
	sbefifo_client_active,
};

struct sbefifo_client {
	struct sbefifo *fifo;

	wait_queue_head_t wait;
	enum sbefifo_client_state state;
	unsigned int flags;
};

int sbefifo_open(struct sbefifo *fifo, struct sbefifo_client *client,
		 unsigned long flags);
ssize_t sbefifo_write(struct sbefifo_client *client, const u32 *buf, ssize_t len);
ssize_t sbefifo_read(struct sbefifo_client *client, u32 *buf, ssize_t len);
int sbefifo_release(struct sbefifo_client *client);

extern struct list_head sbefifos;

#define sbefifo_for_each_dev(pos) \
	list_for_each_entry(pos, &sbefifos, entry)

#endif /* DRIVERS_FSI_SBEFIFO_H */
