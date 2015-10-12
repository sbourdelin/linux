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


#define DEV_IS_GONE(dev) \
	((!dev) || (dev->dev_type == SAS_PHY_UNUSED))

static struct hisi_hba *dev_to_hisi_hba(struct domain_device *device)
{
	return device->port->ha->lldd_ha;
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
	case SAS_PROTOCOL_SSP:
		rc = hisi_sas_task_prep_ssp(hisi_hba, &tei, is_tmf, tmf);
		break;
	case SAS_PROTOCOL_SMP:
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

int hisi_sas_queue_command(struct sas_task *task, gfp_t gfp_flags)
{
	return hisi_sas_task_exec(task, gfp_flags, NULL, 0, NULL);
}
