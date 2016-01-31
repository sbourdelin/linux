/*
 * ssip_slave.h
 *
 * SSIP slave support header file
 *
 * Copyright (C) 2010 Nokia Corporation. All rights reserved.
 *
 * Contact: Carlos Chinea <carlos.chinea@nokia.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA
 */

#ifndef __LINUX_SSIP_SLAVE_H__
#define __LINUX_SSIP_SLAVE_H__

#include <linux/hsi/hsi.h>

enum nokia_modem_state {
	STATE_BOOT,
	STATE_ON,
	STATE_OFF,
};

enum nokia_modem_type {
	UNKNOWN = 0,
	RAPUYAMA_V1,
	RAPUYAMA_V2,
};

struct ssi_protocol_platform_data {
	enum nokia_modem_type type;
	struct device *nokia_modem_dev;
};

static inline void ssip_slave_put_master(struct hsi_client *master)
{
}

struct hsi_client *ssip_slave_get_master(struct hsi_client *slave);
int ssip_slave_start_tx(struct hsi_client *master);
int ssip_slave_stop_tx(struct hsi_client *master);
void ssip_reset_event(struct hsi_client *master);

int ssip_notifier_register(struct hsi_client *master, struct notifier_block *nb);
int ssip_notifier_unregister(struct hsi_client *master, struct notifier_block *nb);

int ssip_slave_running(struct hsi_client *master);

#endif /* __LINUX_SSIP_SLAVE_H__ */

