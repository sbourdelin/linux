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


static void hisi_sas_slot_index_clear(struct hisi_hba *hisi_hba, int slot_idx)
{
	void *bitmap = hisi_hba->slot_index_tags;

	clear_bit(slot_idx, bitmap);
}

void hisi_sas_slot_index_init(struct hisi_hba *hisi_hba)
{
	int i;

	for (i = 0; i < hisi_hba->slot_index_count; ++i)
		hisi_sas_slot_index_clear(hisi_hba, i);
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
