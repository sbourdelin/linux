/*
 * Serial Attached SCSI (SAS) Event processing
 *
 * Copyright (C) 2005 Adaptec, Inc.  All rights reserved.
 * Copyright (C) 2005 Luben Tuikov <luben_tuikov@adaptec.com>
 *
 * This file is licensed under GPLv2.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#include <linux/export.h>
#include <scsi/scsi_host.h>
#include "sas_internal.h"
#include "sas_dump.h"

static DEFINE_SPINLOCK(sas_event_lock);

static const work_func_t sas_ha_event_fns[HA_NUM_EVENTS] = {
	   [HAE_RESET] = sas_hae_reset,
};

int sas_queue_work(struct sas_ha_struct *ha, struct sas_work *sw)
{
	int rc = 0;

	if (!test_bit(SAS_HA_REGISTERED, &ha->state))
		return rc;

	rc = 1;
	if (test_bit(SAS_HA_DRAINING, &ha->state)) {
		/* add it to the defer list, if not already pending */
		if (list_empty(&sw->drain_node))
			list_add(&sw->drain_node, &ha->defer_q);
	} else
		rc = queue_work(ha->event_q, &sw->work);

	return rc;
}

static int sas_queue_event(int event, struct sas_work *work,
			    struct sas_ha_struct *ha)
{
	int rc = 0;
	unsigned long flags;

	spin_lock_irqsave(&ha->lock, flags);
	rc = sas_queue_work(ha, work);
	spin_unlock_irqrestore(&ha->lock, flags);

	return rc;
}


void __sas_drain_work(struct sas_ha_struct *ha)
{
	int ret;
	unsigned long flags;
	struct workqueue_struct *wq = ha->event_q;
	struct sas_work *sw, *_sw;

	set_bit(SAS_HA_DRAINING, &ha->state);
	/* flush submitters */
	spin_lock_irq(&ha->lock);
	spin_unlock_irq(&ha->lock);

	drain_workqueue(wq);

	spin_lock_irq(&ha->lock);
	clear_bit(SAS_HA_DRAINING, &ha->state);
	list_for_each_entry_safe(sw, _sw, &ha->defer_q, drain_node) {
		list_del_init(&sw->drain_node);
		ret = sas_queue_work(ha, sw);
		if (ret != 1) {
			spin_lock_irqsave(&sas_event_lock, flags);
			sw->used = false;
			spin_unlock_irqrestore(&sas_event_lock, flags);
		}
	}
	spin_unlock_irq(&ha->lock);
}

int sas_drain_work(struct sas_ha_struct *ha)
{
	int err;

	err = mutex_lock_interruptible(&ha->drain_mutex);
	if (err)
		return err;
	if (test_bit(SAS_HA_REGISTERED, &ha->state))
		__sas_drain_work(ha);
	mutex_unlock(&ha->drain_mutex);

	return 0;
}
EXPORT_SYMBOL_GPL(sas_drain_work);

void sas_disable_revalidation(struct sas_ha_struct *ha)
{
	mutex_lock(&ha->disco_mutex);
	set_bit(SAS_HA_ATA_EH_ACTIVE, &ha->state);
	mutex_unlock(&ha->disco_mutex);
}

void sas_enable_revalidation(struct sas_ha_struct *ha)
{
	int i;

	mutex_lock(&ha->disco_mutex);
	clear_bit(SAS_HA_ATA_EH_ACTIVE, &ha->state);
	for (i = 0; i < ha->num_phys; i++) {
		struct asd_sas_port *port = ha->sas_port[i];
		const int ev = DISCE_REVALIDATE_DOMAIN;
		struct sas_discovery *d = &port->disc;

		if (!test_and_clear_bit(ev, &d->pending))
			continue;

		sas_queue_event(ev, &d->disc_work[ev].work, ha);
	}
	mutex_unlock(&ha->disco_mutex);
}

static void sas_free_ha_event(struct sas_ha_event *event)
{
	unsigned long flags;
	spin_lock_irqsave(&sas_event_lock, flags);
	event->work.used = false;
	spin_unlock_irqrestore(&sas_event_lock, flags);
}

static void sas_free_port_event(struct asd_sas_event *event)
{
	unsigned long flags;
	spin_lock_irqsave(&sas_event_lock, flags);
	event->work.used = false;
	spin_unlock_irqrestore(&sas_event_lock, flags);
}

static void sas_free_phy_event(struct asd_sas_event *event)
{
	unsigned long flags;
	spin_lock_irqsave(&sas_event_lock, flags);
	event->work.used = false;
	spin_unlock_irqrestore(&sas_event_lock, flags);
}

static void sas_ha_event_worker(struct work_struct *work)
{
	struct sas_ha_event *ev = to_sas_ha_event(work);

	sas_ha_event_fns[ev->type](work);
	sas_free_ha_event(ev);
}

static void sas_port_event_worker(struct work_struct *work)
{
	struct asd_sas_event *ev = to_asd_sas_event(work);

	sas_port_event_fns[ev->type](work);
	sas_free_port_event(ev);
}

static void sas_phy_event_worker(struct work_struct *work)
{
	struct asd_sas_event *ev = to_asd_sas_event(work);

	sas_phy_event_fns[ev->type](work);
	sas_free_phy_event(ev);
}

static struct sas_ha_event *sas_alloc_ha_event(struct sas_ha_struct *sas_ha)
{
	int i;
	unsigned long flags;

	spin_lock_irqsave(&sas_event_lock, flags);
	for (i = 0; i < HA_NUM_EVENTS; i++)
		if (!sas_ha->ha_events[i].work.used)
			break;

	if (i == HA_NUM_EVENTS) {
		spin_unlock_irqrestore(&sas_event_lock, flags);
		return NULL;
	}

	sas_ha->ha_events[i].work.used = true;
	spin_unlock_irqrestore(&sas_event_lock, flags);
	return &sas_ha->ha_events[i];
}

static int notify_ha_event(struct sas_ha_struct *sas_ha, enum ha_event event)
{
	int ret;
	struct sas_ha_event *ev;

	BUG_ON(event >= HA_NUM_EVENTS);

	ev = sas_alloc_ha_event(sas_ha);
	if (!ev) {
		pr_err("%s: alloc sas ha event fail!\n", __func__);
		return 0;
	}

	INIT_SAS_WORK(&ev->work, sas_ha_event_worker);
	ev->ha = sas_ha;
	ev->type = event;
	ret = sas_queue_event(event, &ev->work, sas_ha);
	if (ret != 1)
		sas_free_ha_event(ev);

	return ret;
}

struct asd_sas_event *sas_alloc_port_event(struct asd_sas_phy *phy)
{
	int i;
	unsigned long flags;

	spin_lock_irqsave(&sas_event_lock, flags);
	for (i = 0; i < PORT_POOL_SIZE; i++)
	{
		if (!phy->port_events[i].work.used)
			break;
	}

	if (i == PORT_POOL_SIZE) {
		spin_unlock_irqrestore(&sas_event_lock, flags);
		return NULL;
	}

	phy->port_events[i].work.used = true;
	spin_unlock_irqrestore(&sas_event_lock, flags);
	return &phy->port_events[i];
}

static int notify_port_event(struct asd_sas_phy *phy, enum port_event event)
{
	int ret;
	struct asd_sas_event *ev;
	struct sas_ha_struct *ha = phy->ha;

	BUG_ON(event >= PORT_NUM_EVENTS);

	ev = sas_alloc_port_event(phy);
	if (!ev) {
		pr_err("%s: alloc sas port event fail!\n", __func__);
		return 0;
	}

	INIT_SAS_WORK(&ev->work, sas_port_event_worker);
	ev->phy = phy;
	ev->type = event;
	ret = sas_queue_event(event, &ev->work, ha);
	if (ret != 1)
		sas_free_port_event(ev);

	return ret;
}

struct asd_sas_event *sas_alloc_phy_event(struct asd_sas_phy *phy)
{
	int i;
	unsigned long flags;

	spin_lock_irqsave(&sas_event_lock, flags);
	for (i = 0; i < PHY_POOL_SIZE; i++)
		if (!phy->phy_events[i].work.used)
			break;

	if (i == PHY_POOL_SIZE) {
		spin_unlock_irqrestore(&sas_event_lock, flags);
		return NULL;
	}

	phy->phy_events[i].work.used = true;
	spin_unlock_irqrestore(&sas_event_lock, flags);
	return &phy->phy_events[i];
}
int sas_notify_phy_event(struct asd_sas_phy *phy, enum phy_event event)
{
	int ret;
	struct asd_sas_event *ev;
	struct sas_ha_struct *ha = phy->ha;

	BUG_ON(event >= PHY_NUM_EVENTS);

	ev = sas_alloc_phy_event(phy);
	if (!ev) {
		pr_err("%s: alloc sas phy event fail!\n", __func__);
		return 0;
	}

	INIT_SAS_WORK(&ev->work, sas_phy_event_worker);
	ev->phy = phy;
	ev->type = event;
	ret = sas_queue_event(event, &ev->work, ha);
	if (ret != 1)
		sas_free_phy_event(ev);

	return ret;
}

int sas_init_events(struct sas_ha_struct *sas_ha)
{
	int i;

	for (i = 0; i < HA_NUM_EVENTS; i++)
		sas_ha->ha_events[i].work.used = false;

	sas_ha->notify_ha_event = notify_ha_event;
	sas_ha->notify_port_event = notify_port_event;
	sas_ha->notify_phy_event = sas_notify_phy_event;

	return 0;
}
