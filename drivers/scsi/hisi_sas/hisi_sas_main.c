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
