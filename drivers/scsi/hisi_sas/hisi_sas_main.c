/*
 * Copyright (c) 2015 Linaro Ltd.
 * Copyright (c) 2015 Hisilicon Limited.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 */

#include "hisi_sas.h"

#define DEV_IS_EXPANDER(type) \
	((type == SAS_EDGE_EXPANDER_DEVICE) || \
	(type == SAS_FANOUT_EXPANDER_DEVICE))

#define DEV_IS_GONE(dev) \
	((!dev) || (dev->dev_type == SAS_PHY_UNUSED))

static struct hisi_hba *dev_to_hisi_hba(struct domain_device *device)
{
	return device->port->ha->lldd_ha;
}

static int hisi_sas_find_tag(struct hisi_hba *hisi_hba,
			     struct sas_task *task, u32 *tag)
{
	if (task->lldd_task) {
		struct hisi_sas_slot *slot;

		slot = task->lldd_task;
		*tag = slot->idx;
		return 1;
	}
	return 0;
}

static void hisi_sas_slot_index_clear(struct hisi_hba *hisi_hba, int slot_idx)
{
	void *bitmap = hisi_hba->slot_index_tags;

	clear_bit(slot_idx, bitmap);
}

static void hisi_sas_slot_index_free(struct hisi_hba *hisi_hba, int slot_idx)
{
	hisi_sas_slot_index_clear(hisi_hba, slot_idx);
}

static void hisi_sas_slot_index_set(struct hisi_hba *hisi_hba, int slot_idx)
{
	void *bitmap = hisi_hba->slot_index_tags;

	set_bit(slot_idx, bitmap);
}

static int hisi_sas_slot_index_alloc(struct hisi_hba *hisi_hba, int *slot_idx)
{
	unsigned int index;
	void *bitmap = hisi_hba->slot_index_tags;

	index = find_first_zero_bit(bitmap, hisi_hba->slot_index_count);
	if (index >= hisi_hba->slot_index_count)
		return -SAS_QUEUE_FULL;
	hisi_sas_slot_index_set(hisi_hba, index);
	*slot_idx = index;
	return 0;
}

void hisi_sas_slot_index_init(struct hisi_hba *hisi_hba)
{
	int i;

	for (i = 0; i < hisi_hba->slot_index_count; ++i)
		hisi_sas_slot_index_clear(hisi_hba, i);
}

void hisi_sas_slot_task_free(struct hisi_hba *hisi_hba,
			struct sas_task *task,
			struct hisi_sas_slot *slot)
{
	struct device *dev = &hisi_hba->pdev->dev;

	if (!slot->task)
		return;

	if (!sas_protocol_ata(task->task_proto))
		if (slot->n_elem)
			dma_unmap_sg(dev, task->scatter, slot->n_elem,
				     task->data_dir);

	switch (task->task_proto) {
	case SAS_PROTOCOL_SMP:
		break;

	case SAS_PROTOCOL_SATA:
	case SAS_PROTOCOL_STP:
	case SAS_PROTOCOL_SSP:
	default:
		/* do nothing */
		break;
	}

	if (slot->command_table)
		dma_pool_free(hisi_hba->command_table_pool,
			      slot->command_table, slot->command_table_dma);

	if (slot->status_buffer)
		dma_pool_free(hisi_hba->status_buffer_pool,
			      slot->status_buffer, slot->status_buffer_dma);

	if (slot->sge_page)
		dma_pool_free(hisi_hba->sge_page_pool, slot->sge_page,
			slot->sge_page_dma);

	list_del_init(&slot->entry);
	task->lldd_task = NULL;
	slot->task = NULL;
	slot->port = NULL;
	hisi_sas_slot_index_free(hisi_hba, slot->idx);
	memset(slot, 0, sizeof(*slot));
}

static int hisi_sas_task_prep_smp(struct hisi_hba *hisi_hba,
				struct hisi_sas_tei *tei)
{
	return prep_smp_v1_hw(hisi_hba, tei);
}

static int hisi_sas_task_prep_ssp(struct hisi_hba *hisi_hba,
				  struct hisi_sas_tei *tei, int is_tmf,
				  struct hisi_sas_tmf_task *tmf)
{
	return prep_ssp_v1_hw(hisi_hba, tei, is_tmf, tmf);
}

static int hisi_sas_task_prep(struct sas_task *task,
				struct hisi_hba *hisi_hba,
				int is_tmf, struct hisi_sas_tmf_task *tmf,
				int *pass)
{
	struct domain_device *device = task->dev;
	struct hisi_sas_device *sas_dev = device->lldd_dev;
	struct hisi_sas_tei tei;
	struct hisi_sas_slot *slot;
	struct hisi_sas_cmd_hdr	*cmd_hdr_base;
	struct device *dev = &hisi_hba->pdev->dev;
	int dlvry_queue_slot, dlvry_queue, n_elem = 0, rc, slot_idx;

	if (!device->port) {
		struct task_status_struct *tsm = &task->task_status;

		tsm->resp = SAS_TASK_UNDELIVERED;
		tsm->stat = SAS_PHY_DOWN;
		/*
		 * libsas will use dev->port, should
		 * not call task_done for sata
		 */
		if (device->dev_type != SAS_SATA_DEV)
			task->task_done(task);
		return 0;
	}

	if (DEV_IS_GONE(sas_dev)) {
		if (sas_dev)
			dev_info(dev, "task prep: device %llu not ready\n",
				 sas_dev->device_id);
		else
			dev_info(dev, "task prep: device %016llx not ready\n",
				 SAS_ADDR(device->sas_addr));

		rc = SAS_PHY_DOWN;
		return rc;
	}
	tei.port = device->port->lldd_port;
	if (tei.port && !tei.port->port_attached && !tmf) {
		if (sas_protocol_ata(task->task_proto)) {
			struct task_status_struct *ts = &task->task_status;

			dev_info(dev,
				 "task prep: SATA/STP port%d not attach device\n",
				 device->port->id);
			ts->resp = SAS_TASK_COMPLETE;
			ts->stat = SAS_PHY_DOWN;
			task->task_done(task);
		} else {
			struct task_status_struct *ts = &task->task_status;

			dev_info(dev,
				 "task prep: SAS port%d does not attach device\n",
				 device->port->id);
			ts->resp = SAS_TASK_UNDELIVERED;
			ts->stat = SAS_PHY_DOWN;
			task->task_done(task);
		}
		return 0;
	}

	if (!sas_protocol_ata(task->task_proto)) {
		if (task->num_scatter) {
			n_elem = dma_map_sg(dev,
					task->scatter,
					task->num_scatter,
					task->data_dir);
			if (!n_elem) {
				rc = -ENOMEM;
				goto prep_out;
			}
		}
	} else {
		n_elem = task->num_scatter;
	}

	rc = hisi_sas_slot_index_alloc(hisi_hba, &slot_idx);
	if (rc)
		goto err_out;
	rc = get_free_slot_v1_hw(hisi_hba,
				 &dlvry_queue,
				 &dlvry_queue_slot);
	if (rc)
		goto err_out_tag;

	slot = &hisi_hba->slot_info[slot_idx];
	memset(slot, 0, sizeof(struct hisi_sas_slot));

	task->lldd_task = NULL;
	slot->idx = slot_idx;
	tei.iptt = slot_idx;
	slot->n_elem = n_elem;
	slot->dlvry_queue = dlvry_queue;
	slot->dlvry_queue_slot = dlvry_queue_slot;
	cmd_hdr_base = hisi_hba->cmd_hdr[dlvry_queue];
	slot->cmd_hdr = &cmd_hdr_base[dlvry_queue_slot];

	slot->status_buffer = dma_pool_alloc(hisi_hba->status_buffer_pool,
					     GFP_ATOMIC,
					     &slot->status_buffer_dma);
	if (!slot->status_buffer)
		goto err_out_slot_buf;
	memset(slot->status_buffer, 0, HISI_SAS_STATUS_BUF_SZ);

	slot->command_table = dma_pool_alloc(hisi_hba->command_table_pool,
					     GFP_ATOMIC,
					     &slot->command_table_dma);
	if (!slot->command_table)
		goto err_out_status_buf;
	memset(slot->command_table, 0, HISI_SAS_COMMAND_TABLE_SZ);
	memset(slot->cmd_hdr, 0, sizeof(struct hisi_sas_cmd_hdr));

	tei.hdr = slot->cmd_hdr;
	tei.task = task;
	tei.n_elem = n_elem;
	tei.slot = slot;
	switch (task->task_proto) {
	case SAS_PROTOCOL_SMP:
		rc = hisi_sas_task_prep_smp(hisi_hba, &tei);
		break;
	case SAS_PROTOCOL_SSP:
		rc = hisi_sas_task_prep_ssp(hisi_hba, &tei, is_tmf, tmf);
		break;
	case SAS_PROTOCOL_SATA:
	case SAS_PROTOCOL_STP:
	case SAS_PROTOCOL_SATA | SAS_PROTOCOL_STP:
	default:
		dev_err(dev, "task prep: unknown/unsupported proto (0x%x)\n",
			task->task_proto);
		rc = -EINVAL;
		break;
	}

	if (rc) {
		dev_err(dev, "task prep: rc = 0x%x\n", rc);
		if (slot->sge_page)
			goto err_out_sge;
		goto err_out_command_table;
	}

	slot->task = task;
	slot->port = tei.port;
	task->lldd_task = slot;
	list_add_tail(&slot->entry, &tei.port->list);
	spin_lock(&task->task_state_lock);
	task->task_state_flags |= SAS_TASK_AT_INITIATOR;
	spin_unlock(&task->task_state_lock);

	hisi_hba->slot_prep = slot;

	sas_dev->running_req++;
	++(*pass);

	return rc;

err_out_sge:
	dma_pool_free(hisi_hba->sge_page_pool, slot->sge_page,
		slot->sge_page_dma);
err_out_command_table:
	dma_pool_free(hisi_hba->command_table_pool, slot->command_table,
		slot->command_table_dma);
err_out_status_buf:
	dma_pool_free(hisi_hba->status_buffer_pool, slot->status_buffer,
		slot->status_buffer_dma);
err_out_slot_buf:
	/* Nothing to be done */
err_out_tag:
	hisi_sas_slot_index_free(hisi_hba, slot_idx);
err_out:
	dev_err(dev, "task prep: failed[%d]!\n", rc);
	if (!sas_protocol_ata(task->task_proto))
		if (n_elem)
			dma_unmap_sg(dev, task->scatter, n_elem,
				     task->data_dir);
prep_out:
	return rc;
}

static int hisi_sas_task_exec(struct sas_task *task,
	gfp_t gfp_flags,
	struct completion *completion,
	int is_tmf,
	struct hisi_sas_tmf_task *tmf)
{
	u32 rc;
	u32 pass = 0;
	unsigned long flags = 0;
	struct hisi_hba *hisi_hba = dev_to_hisi_hba(task->dev);
	struct device *dev = &hisi_hba->pdev->dev;

	spin_lock_irqsave(&hisi_hba->lock, flags);
	rc = hisi_sas_task_prep(task, hisi_hba, is_tmf, tmf, &pass);
	if (rc)
		dev_err(dev, "task exec: failed[%d]!\n", rc);

	if (likely(pass))
		start_delivery_v1_hw(hisi_hba);
	spin_unlock_irqrestore(&hisi_hba->lock, flags);

	return rc;
}

void hisi_sas_bytes_dmaed(struct hisi_hba *hisi_hba, int phy_no)
{
	struct hisi_sas_phy *phy = &hisi_hba->phy[phy_no];
	struct asd_sas_phy *sas_phy = &phy->sas_phy;
	struct sas_ha_struct *sas_ha;

	if (!phy->phy_attached)
		return;

	sas_ha = &hisi_hba->sha;
	sas_ha->notify_phy_event(sas_phy, PHYE_OOB_DONE);

	if (sas_phy->phy) {
		struct sas_phy *sphy = sas_phy->phy;

		sphy->negotiated_linkrate = sas_phy->linkrate;
		sphy->minimum_linkrate = phy->minimum_linkrate;
		sphy->minimum_linkrate_hw = SAS_LINK_RATE_1_5_GBPS;
		sphy->maximum_linkrate = phy->maximum_linkrate;
	}

	if (phy->phy_type & PORT_TYPE_SAS) {
		struct sas_identify_frame *id;

		id = (struct sas_identify_frame *)phy->frame_rcvd;
		id->dev_type = phy->identify.device_type;
		id->initiator_bits = SAS_PROTOCOL_ALL;
		id->target_bits = phy->identify.target_port_protocols;
	} else if (phy->phy_type & PORT_TYPE_SATA) {
		/*Nothing*/
	}

	sas_phy->frame_rcvd_size = phy->frame_rcvd_size;

	sas_ha->notify_port_event(sas_phy, PORTE_BYTES_DMAED);
}

struct hisi_sas_device *hisi_sas_alloc_dev(struct hisi_hba *hisi_hba)
{
	int dev_id;
	struct device *dev = &hisi_hba->pdev->dev;

	for (dev_id = 0; dev_id < HISI_SAS_MAX_DEVICES; dev_id++) {
		if (hisi_hba->devices[dev_id].dev_type == SAS_PHY_UNUSED) {
			hisi_hba->devices[dev_id].device_id = dev_id;
			return &hisi_hba->devices[dev_id];
		}
	}

	dev_err(dev, "alloc dev: max support %d devices - could not alloc\n",
		HISI_SAS_MAX_DEVICES);

	return NULL;
}

int hisi_sas_dev_found_notify(struct domain_device *device, int lock)
{
	unsigned long flags = 0;
	int res = 0;
	struct hisi_hba *hisi_hba = dev_to_hisi_hba(device);
	struct domain_device *parent_dev = device->parent;
	struct hisi_sas_device *sas_dev;
	struct device *dev = &hisi_hba->pdev->dev;

	if (lock)
		spin_lock_irqsave(&hisi_hba->lock, flags);

	sas_dev = hisi_sas_alloc_dev(hisi_hba);
	if (!sas_dev) {
		res = -EINVAL;
		goto found_out;
	}

	device->lldd_dev = sas_dev;
	sas_dev->dev_status = HISI_SAS_DEV_NORMAL;
	sas_dev->dev_type = device->dev_type;
	sas_dev->hisi_hba = hisi_hba;
	sas_dev->sas_device = device;
	setup_itct_v1_hw(hisi_hba, sas_dev);

	if (parent_dev && DEV_IS_EXPANDER(parent_dev->dev_type)) {
		int phy_no;
		u8 phy_num = parent_dev->ex_dev.num_phys;
		struct ex_phy *phy;

		for (phy_no = 0; phy_no < phy_num; phy_no++) {
			phy = &parent_dev->ex_dev.ex_phy[phy_no];
			if (SAS_ADDR(phy->attached_sas_addr) ==
				SAS_ADDR(device->sas_addr)) {
				sas_dev->attached_phy = phy_no;
				break;
			}
		}

		if (phy_no == phy_num) {
			dev_info(dev,
				 "dev found: no attached "
				 "dev:%016llx at ex:%016llx\n",
				 SAS_ADDR(device->sas_addr),
				SAS_ADDR(parent_dev->sas_addr));
			res = -EINVAL;
		}
	}

found_out:
	if (lock)
		spin_unlock_irqrestore(&hisi_hba->lock, flags);
	return res;
}

void hisi_sas_scan_start(struct Scsi_Host *shost)
{
	struct sas_ha_struct *sha = SHOST_TO_SAS_HA(shost);
	struct hisi_hba *hisi_hba = sha->lldd_ha;
	int i;

	for (i = 0; i < hisi_hba->n_phy; ++i)
		hisi_sas_bytes_dmaed(hisi_hba, i);

	hisi_hba->scan_finished = 1;
}

int hisi_sas_scan_finished(struct Scsi_Host *shost, unsigned long time)
{
	struct sas_ha_struct *sha = SHOST_TO_SAS_HA(shost);
	struct hisi_hba *hisi_hba = sha->lldd_ha;

	if (hisi_hba->scan_finished == 0)
		return 0;

	sas_drain_work(sha);
	return 1;
}


static void hisi_sas_phyup_work(struct hisi_hba *hisi_hba,
				      int phy_no)
{
	sl_notify_v1_hw(hisi_hba, phy_no); /* This requires a sleep */
	hisi_sas_bytes_dmaed(hisi_hba, phy_no);
}

void hisi_sas_wq_process(struct work_struct *work)
{
	struct hisi_sas_wq *wq =
		container_of(work, struct hisi_sas_wq, work_struct);
	struct hisi_hba *hisi_hba = wq->hisi_hba;
	int event = wq->event;
	int phy_no = wq->phy_no;

	switch (event) {
	case PHYUP:
		hisi_sas_phyup_work(hisi_hba, phy_no);
		break;
	}

	kfree(wq);
}

void hisi_sas_phy_init(struct hisi_hba *hisi_hba, int phy_no)
{
	struct hisi_sas_phy *phy = &hisi_hba->phy[phy_no];
	struct asd_sas_phy *sas_phy = &phy->sas_phy;

	phy->hisi_hba = hisi_hba;
	phy->port = NULL;
	init_timer(&phy->timer);
	sas_phy->enabled = (phy_no < hisi_hba->n_phy) ? 1 : 0;
	sas_phy->class = SAS;
	sas_phy->iproto = SAS_PROTOCOL_ALL;
	sas_phy->tproto = 0;
	sas_phy->type = PHY_TYPE_PHYSICAL;
	sas_phy->role = PHY_ROLE_INITIATOR;
	sas_phy->oob_mode = OOB_NOT_CONNECTED;
	sas_phy->linkrate = SAS_LINK_RATE_UNKNOWN;
	sas_phy->id = phy_no;
	sas_phy->sas_addr = &hisi_hba->sas_addr[0];
	sas_phy->frame_rcvd = &phy->frame_rcvd[0];
	sas_phy->ha = (struct sas_ha_struct *)hisi_hba->shost->hostdata;
	sas_phy->lldd_phy = phy;
}

void hisi_sas_port_notify_formed(struct asd_sas_phy *sas_phy, int lock)
{
	struct sas_ha_struct *sas_ha = sas_phy->ha;
	struct hisi_hba *hisi_hba = NULL;
	int i = 0;
	struct hisi_sas_phy *phy = sas_phy->lldd_phy;
	struct asd_sas_port *sas_port = sas_phy->port;
	struct hisi_sas_port *port;
	unsigned long flags = 0;

	if (!sas_port)
		return;

	while (sas_ha->sas_phy[i]) {
		if (sas_ha->sas_phy[i] == sas_phy) {
			hisi_hba = (struct hisi_hba *)sas_ha->lldd_ha;
			port = &hisi_hba->port[i];
			break;
		}
		i++;
	}

	if (hisi_hba == NULL) {
		pr_err("%s: could not find hba\n", __func__);
		return;
	}

	if (lock)
		spin_lock_irqsave(&hisi_hba->lock, flags);
	port->port_attached = 1;
	port->id = phy->port_id;
	phy->port = port;
	sas_port->lldd_port = port;

	if (lock)
		spin_unlock_irqrestore(&hisi_hba->lock, flags);
}

void hisi_sas_do_release_task(struct hisi_hba *hisi_hba,
		int phy_no, struct domain_device *device)
{
	struct hisi_sas_phy *phy;
	struct hisi_sas_port *port;
	struct hisi_sas_slot *slot, *slot2;
	struct device *dev = &hisi_hba->pdev->dev;

	phy = &hisi_hba->phy[phy_no];
	port = phy->port;
	if (!port)
		return;

	list_for_each_entry_safe(slot, slot2, &port->list, entry) {
		struct sas_task *task;

		task = slot->task;
		if (device && task->dev != device)
			continue;

		dev_info(dev, "Release slot [%d:%d], task [%p]:\n",
			 slot->dlvry_queue, slot->dlvry_queue_slot, task);
		slot_complete_v1_hw(hisi_hba, slot, 1);
	}
}

static void hisi_sas_port_notify_deformed(struct asd_sas_phy *sas_phy,
					  int lock)
{
	struct domain_device *device;
	struct hisi_sas_phy *phy = sas_phy->lldd_phy;
	struct hisi_hba *hisi_hba = phy->hisi_hba;
	struct asd_sas_port *sas_port = sas_phy->port;
	struct hisi_sas_port *port = sas_port->lldd_port;
	int phy_no = 0;

	port->port_attached = 0;
	port->id = -1;

	while (phy != &hisi_hba->phy[phy_no]) {
		phy_no++;

		if (phy_no >= hisi_hba->n_phy)
			return;
	}
	list_for_each_entry(device, &sas_port->dev_list, dev_list_node)
		hisi_sas_do_release_task(phy->hisi_hba, phy_no, device);
}

int hisi_sas_dev_found(struct domain_device *device)
{
	return hisi_sas_dev_found_notify(device, 1);
}

int hisi_sas_find_dev_phyno(struct domain_device *device, int *phyno)
{
	int i = 0, j = 0, num = 0, n = 0;
	struct sas_ha_struct *sha = device->port->ha;

	while (sha->sas_port[i]) {
		if (sha->sas_port[i] == device->port) {
			struct asd_sas_phy *phy;

			list_for_each_entry(phy,
				&sha->sas_port[i]->phy_list, port_phy_el) {
				j = 0;

				while (sha->sas_phy[j]) {
					if (sha->sas_phy[j] == phy)
						break;
					j++;
				}
				phyno[n] = j;
				num++;
				n++;
			}
			break;
		}
		i++;
	}
	return num;
}

static void hisi_sas_release_task(struct hisi_hba *hisi_hba,
			struct domain_device *device)
{
	int i, phyno[4], num;

	num = hisi_sas_find_dev_phyno(device, phyno);
	for (i = 0; i < num; i++)
		hisi_sas_do_release_task(hisi_hba, phyno[i], device);
}

static void hisi_sas_dev_gone_notify(struct domain_device *device)
{
	struct hisi_sas_device *sas_dev = device->lldd_dev;
	struct hisi_hba *hisi_hba = dev_to_hisi_hba(device);
	struct device *dev = &hisi_hba->pdev->dev;

	if (!sas_dev) {
		pr_warn("%s: found dev has gone\n", __func__);
		return;
	}

	dev_info(dev, "found dev[%lld:%x] is gone\n",
		 sas_dev->device_id, sas_dev->dev_type);

	free_device_v1_hw(hisi_hba, sas_dev);

	device->lldd_dev = NULL;
	sas_dev->sas_device = NULL;
}

void hisi_sas_dev_gone(struct domain_device *device)
{
	hisi_sas_dev_gone_notify(device);
}

int hisi_sas_queue_command(struct sas_task *task, gfp_t gfp_flags)
{
	return hisi_sas_task_exec(task, gfp_flags, NULL, 0, NULL);
}


static void hisi_sas_task_done(struct sas_task *task)
{
	if (!del_timer(&task->slow_task->timer))
		return;
	complete(&task->slow_task->completion);
}

static void hisi_sas_tmf_timedout(unsigned long data)
{
	struct sas_task *task = (struct sas_task *)data;

	task->task_state_flags |= SAS_TASK_STATE_ABORTED;
	complete(&task->slow_task->completion);
}

#define TASK_TIMEOUT 20
static int hisi_sas_exec_internal_tmf_task(struct domain_device *device,
					   void *parameter,
					   u32 para_len,
					   struct hisi_sas_tmf_task *tmf)
{
	int res, retry;
	struct hisi_sas_device *sas_dev = device->lldd_dev;
	struct hisi_hba *hisi_hba = sas_dev->hisi_hba;
	struct device *dev = &hisi_hba->pdev->dev;
	struct sas_task *task = NULL;

	for (retry = 0; retry < 3; retry++) {
		task = sas_alloc_slow_task(GFP_KERNEL);
		if (!task)
			return -ENOMEM;

		task->dev = device;
		task->task_proto = device->tproto;

		memcpy(&task->ssp_task, parameter, para_len);
		task->task_done = hisi_sas_task_done;

		task->slow_task->timer.data = (unsigned long) task;
		task->slow_task->timer.function = hisi_sas_tmf_timedout;
		task->slow_task->timer.expires = jiffies + TASK_TIMEOUT*HZ;
		add_timer(&task->slow_task->timer);

		res = hisi_sas_task_exec(task, GFP_KERNEL, NULL, 1, tmf);

		if (res) {
			del_timer(&task->slow_task->timer);
			dev_err(dev, "executing internal task failed: %d\n",
				res);
			goto ex_err;
		}

		wait_for_completion(&task->slow_task->completion);
		res = TMF_RESP_FUNC_FAILED;
		/* Even TMF timed out, return direct. */
		if ((task->task_state_flags & SAS_TASK_STATE_ABORTED)) {
			if (!(task->task_state_flags & SAS_TASK_STATE_DONE)) {
				dev_err(dev, "TMF task[%d] timeout\n",
				       tmf->tag_of_task_to_be_managed);
				if (task->lldd_task) {
					struct hisi_sas_slot *slot;

					slot = (struct hisi_sas_slot *)
						task->lldd_task;
					hisi_sas_slot_task_free(hisi_hba,
								task, slot);
				}

				goto ex_err;
			}
		}

		if (task->task_status.resp == SAS_TASK_COMPLETE &&
		    task->task_status.stat == SAM_STAT_GOOD) {
			res = TMF_RESP_FUNC_COMPLETE;
			break;
		}

		if (task->task_status.resp == SAS_TASK_COMPLETE &&
		      task->task_status.stat == SAS_DATA_UNDERRUN) {
			/* no error, but return the number of bytes of
			 * underrun */
			pr_warn(" ok: task to dev %016llx response: 0x%x "
				    "status 0x%x underrun\n",
				    SAS_ADDR(device->sas_addr),
				    task->task_status.resp,
				    task->task_status.stat);
			res = task->task_status.residual;
			break;
		}

		if (task->task_status.resp == SAS_TASK_COMPLETE &&
			task->task_status.stat == SAS_DATA_OVERRUN) {
			pr_warn("%s: blocked task error\n", __func__);
			res = -EMSGSIZE;
			break;
		}

		pr_warn("%s: task to dev %016llx response: 0x%x "
			"status 0x%x\n", __func__,
			SAS_ADDR(device->sas_addr),
			task->task_status.resp,
			task->task_status.stat);
		sas_free_task(task);
		task = NULL;
	}
ex_err:
	BUG_ON(retry == 3 && task != NULL);
	sas_free_task(task);
	return res;
}

static int hisi_sas_debug_issue_ssp_tmf(struct domain_device *device,
				u8 *lun, struct hisi_sas_tmf_task *tmf)
{
	struct sas_ssp_task ssp_task;

	if (!(device->tproto & SAS_PROTOCOL_SSP))
		return TMF_RESP_FUNC_ESUPP;

	memcpy(ssp_task.LUN, lun, 8);

	return hisi_sas_exec_internal_tmf_task(device, &ssp_task,
				sizeof(ssp_task), tmf);
}

int hisi_sas_abort_task(struct sas_task *task)
{
	struct scsi_lun lun;
	struct hisi_sas_tmf_task tmf_task;
	struct domain_device *device = task->dev;
	struct hisi_sas_device *sas_dev = device->lldd_dev;
	struct hisi_hba *hisi_hba;
	struct device *dev;
	int rc = TMF_RESP_FUNC_FAILED;
	unsigned long flags;
	u32 tag;

	if (!sas_dev) {
		pr_warn("%s: Device has been removed\n", __func__);
		return TMF_RESP_FUNC_FAILED;
	}

	hisi_hba = dev_to_hisi_hba(task->dev);
	dev = &hisi_hba->pdev->dev;

	spin_lock_irqsave(&task->task_state_lock, flags);
	if (task->task_state_flags & SAS_TASK_STATE_DONE) {
		spin_unlock_irqrestore(&task->task_state_lock, flags);
		rc = TMF_RESP_FUNC_COMPLETE;
		goto out;
	}

	spin_unlock_irqrestore(&task->task_state_lock, flags);
	sas_dev->dev_status = HISI_SAS_DEV_EH;
	if (task->lldd_task && task->task_proto & SAS_PROTOCOL_SSP) {
		struct scsi_cmnd *cmnd = (struct scsi_cmnd *)task->uldd_task;

		int_to_scsilun(cmnd->device->lun, &lun);
		rc = hisi_sas_find_tag(hisi_hba, task, &tag);
		if (rc == 0) {
			dev_notice(dev, "abort task: No such tag\n");
			rc = TMF_RESP_FUNC_FAILED;
			return rc;
		}

		tmf_task.tmf = TMF_ABORT_TASK;
		tmf_task.tag_of_task_to_be_managed = cpu_to_le16(tag);

		rc = hisi_sas_debug_issue_ssp_tmf(task->dev,
						  lun.scsi_lun,
						  &tmf_task);

		/* if successful, clear the task and callback forwards.*/
		if (rc == TMF_RESP_FUNC_COMPLETE) {
			if (task->lldd_task) {
				struct hisi_sas_slot *slot;

				slot = &hisi_hba->slot_info
					[tmf_task.tag_of_task_to_be_managed];
				spin_lock_irqsave(&hisi_hba->lock, flags);
				slot_complete_v1_hw(hisi_hba, slot, 1);
				spin_unlock_irqrestore(&hisi_hba->lock, flags);
			}
		}

	} else if (task->task_proto & SAS_PROTOCOL_SATA ||
		task->task_proto & SAS_PROTOCOL_STP) {
		if (SAS_SATA_DEV == task->dev->dev_type) {
			struct hisi_slot_info *slot = task->lldd_task;

			dev_notice(dev, "abort task: hba=%p task=%p slot=%p\n",
				   hisi_hba, task, slot);
			task->task_state_flags |= SAS_TASK_STATE_ABORTED;
			rc = TMF_RESP_FUNC_COMPLETE;
			goto out;
		}

	}

out:
	if (rc != TMF_RESP_FUNC_COMPLETE)
		dev_notice(dev, "abort task: rc=%d\n", rc);
	return rc;
}

int hisi_sas_abort_task_set(struct domain_device *device, u8 *lun)
{
	int rc = TMF_RESP_FUNC_FAILED;
	struct hisi_sas_tmf_task tmf_task;

	tmf_task.tmf = TMF_ABORT_TASK_SET;
	rc = hisi_sas_debug_issue_ssp_tmf(device, lun, &tmf_task);

	return rc;
}

int hisi_sas_clear_aca(struct domain_device *device, u8 *lun)
{
	int rc = TMF_RESP_FUNC_FAILED;
	struct hisi_sas_tmf_task tmf_task;

	tmf_task.tmf = TMF_CLEAR_ACA;
	rc = hisi_sas_debug_issue_ssp_tmf(device, lun, &tmf_task);

	return rc;
}

int hisi_sas_clear_task_set(struct domain_device *device, u8 *lun)
{
	int rc = TMF_RESP_FUNC_FAILED;
	struct hisi_sas_tmf_task tmf_task;

	tmf_task.tmf = TMF_CLEAR_TASK_SET;
	rc = hisi_sas_debug_issue_ssp_tmf(device, lun, &tmf_task);

	return rc;
}

static int hisi_sas_debug_I_T_nexus_reset(struct domain_device *device)
{
	int rc;
	struct sas_phy *phy = sas_get_local_phy(device);
	int reset_type = (device->dev_type == SAS_SATA_DEV ||
			(device->tproto & SAS_PROTOCOL_STP)) ? 0 : 1;
	rc = sas_phy_reset(phy, reset_type);
	sas_put_local_phy(phy);
	msleep(2000);
	return rc;
}

int hisi_sas_I_T_nexus_reset(struct domain_device *device)
{
	unsigned long flags;
	int rc = TMF_RESP_FUNC_FAILED;
	struct hisi_sas_device *sas_dev = device->lldd_dev;
	struct hisi_hba *hisi_hba = dev_to_hisi_hba(device);

	if (sas_dev->dev_status != HISI_SAS_DEV_EH)
		return TMF_RESP_FUNC_FAILED;
	sas_dev->dev_status = HISI_SAS_DEV_NORMAL;

	rc = hisi_sas_debug_I_T_nexus_reset(device);

	spin_lock_irqsave(&hisi_hba->lock, flags);
	hisi_sas_release_task(hisi_hba, device);
	spin_unlock_irqrestore(&hisi_hba->lock, flags);

	return 0;
}

int hisi_sas_lu_reset(struct domain_device *device, u8 *lun)
{
	unsigned long flags;
	int rc = TMF_RESP_FUNC_FAILED;
	struct hisi_sas_tmf_task tmf_task;
	struct hisi_sas_device *sas_dev = device->lldd_dev;
	struct hisi_hba *hisi_hba = dev_to_hisi_hba(device);
	struct device *dev = &hisi_hba->pdev->dev;

	tmf_task.tmf = TMF_LU_RESET;
	sas_dev->dev_status = HISI_SAS_DEV_EH;
	rc = hisi_sas_debug_issue_ssp_tmf(device, lun, &tmf_task);
	if (rc == TMF_RESP_FUNC_COMPLETE) {
		spin_lock_irqsave(&hisi_hba->lock, flags);
		hisi_sas_release_task(hisi_hba, device);
		spin_unlock_irqrestore(&hisi_hba->lock, flags);
	}
	/* If failed, fall-through I_T_Nexus reset */
	dev_err(dev, "lu_reset: for device[%llx]:rc= %d\n",
		sas_dev->device_id, rc);
	return rc;
}

int hisi_sas_query_task(struct sas_task *task)
{
	u32 tag;
	struct scsi_lun lun;
	struct hisi_sas_tmf_task tmf_task;
	int rc = TMF_RESP_FUNC_FAILED;

	if (task->lldd_task && task->task_proto & SAS_PROTOCOL_SSP) {
		struct scsi_cmnd *cmnd = (struct scsi_cmnd *)task->uldd_task;
		struct domain_device *device = task->dev;
		struct hisi_sas_device *sas_dev = device->lldd_dev;
		struct hisi_hba *hisi_hba = sas_dev->hisi_hba;

		int_to_scsilun(cmnd->device->lun, &lun);
		rc = hisi_sas_find_tag(hisi_hba, task, &tag);
		if (rc == 0) {
			rc = TMF_RESP_FUNC_FAILED;
			return rc;
		}

		tmf_task.tmf = TMF_QUERY_TASK;
		tmf_task.tag_of_task_to_be_managed = cpu_to_le16(tag);

		rc = hisi_sas_debug_issue_ssp_tmf(device,
						  lun.scsi_lun,
						  &tmf_task);
		switch (rc) {
		/* The task is still in Lun, release it then */
		case TMF_RESP_FUNC_SUCC:
		/* The task is not in Lun or failed, reset the phy */
		case TMF_RESP_FUNC_FAILED:
		case TMF_RESP_FUNC_COMPLETE:
			break;
		}
	}
	pr_info("%s: rc=%d\n", __func__, rc);
	return rc;
}

void hisi_sas_port_formed(struct asd_sas_phy *sas_phy)
{
	hisi_sas_port_notify_formed(sas_phy, 1);
}

void hisi_sas_port_deformed(struct asd_sas_phy *sas_phy)
{
	hisi_sas_port_notify_deformed(sas_phy, 1);
}

static void hisi_sas_phy_disconnected(struct hisi_sas_phy *phy)
{
	phy->phy_attached = 0;
	phy->phy_type = 0;
}

void hisi_sas_phy_down(struct hisi_hba *hisi_hba, int phy_no, int rdy)
{
	struct hisi_sas_phy *phy = &hisi_hba->phy[phy_no];
	struct asd_sas_phy *sas_phy = &phy->sas_phy;
	struct sas_ha_struct *sas_ha = &hisi_hba->sha;

	if (rdy) {
		/* Phy down but ready */
		hisi_sas_bytes_dmaed(hisi_hba, phy_no);
		hisi_sas_port_notify_formed(sas_phy, 0);
	} else {
		/* Phy down and not ready */
		sas_ha->notify_phy_event(sas_phy, PHYE_LOSS_OF_SIGNAL);
		phy->phy_attached = 0;
		sas_phy_disconnected(sas_phy);
		hisi_sas_phy_disconnected(phy);
	}
}
