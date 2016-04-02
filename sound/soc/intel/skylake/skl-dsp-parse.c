/*
 *  skl-dsp-parse.c - Implements DSP firmware manifest parsing
 *
 *  Copyright (C) 2016 Intel Corp
 *  Author: Shreyas NC <shreyas.nc@intel.com>
 *  ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as version 2, as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */

#include <linux/module.h>
#include <asm/types.h>
#include <linux/types.h>
#include <linux/string.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/device.h>
#include <linux/slab.h>
#include "../common/sst-dsp.h"
#include "../common/sst-dsp-priv.h"
#include "skl-tplg-interface.h"
#include "skl-sst-ipc.h"
#include "skl-dsp-parse.h"

/*
 * Get the module id for the module by checking
 * the table for the UUID for the module
 */
int snd_skl_get_module_info(struct skl_sst *ctx, char *uuid,
				struct skl_dfw_module *dfw_config)
{
	int i, num;
	struct uuid_tbl *tbl;

	tbl = ctx->tbl;
	num = ctx->num_modules;

	for (i = 0; i < num; i++) {
		if (!strcmp(uuid, tbl[i].uuid)) {
			dfw_config->module_id = tbl[i].module_id;
			dfw_config->is_loadable = tbl[i].is_loadable;

			return 0;
		}
	}
	return -EINVAL;
}
EXPORT_SYMBOL_GPL(snd_skl_get_module_info);

/*
 * Get the uuid from the DSP firmware structure in a string
 * of the format XXXXXXXX-XXXX-XXXX-XXXXXXXXXXXX
 */
static char *get_canonical_uuid(char *buf,
		struct adsp_module_entry *entry)
{
	unsigned int i;

	sprintf(buf, "%08X", entry->uuid.id_1);
	sprintf(buf+strlen(buf), "-");

	for (i = 0; i < ARRAY_SIZE(entry->uuid.id_2); i++) {
		sprintf(buf+strlen(buf),
				"%04X", entry->uuid.id_2[i]);
		sprintf(buf+strlen(buf), "-");
	}

	for (i = 0; i < ARRAY_SIZE(entry->uuid.id_3); i++) {
		sprintf(buf+strlen(buf),
				"%02X", entry->uuid.id_3[i]);
		if (i == 1)
			sprintf(buf+strlen(buf), "-");
	}

	return buf;
}

/*
 * Parse the firmware binary to get the UUID, module id
 * and loadable flags
 */
int parse_fw_bin(struct sst_dsp *ctx)
{
	struct adsp_fw_hdr *adsp_hdr;
	struct adsp_module_entry *mod_entry;
	int i, num_entry;
	char uuid_str[UUID_STR_SIZE];
	const char *buf;
	struct skl_sst *skl = ctx->thread_context;
	struct uuid_tbl *tbl;


	/* Get the FW pointer to derive ADSP header */
	buf = ctx->fw->data;

	adsp_hdr = (struct adsp_fw_hdr *)(buf + SKL_ADSP_FW_BIN_HDR_OFFSET);

	mod_entry = (struct adsp_module_entry *)
		(buf + SKL_ADSP_FW_BIN_HDR_OFFSET + adsp_hdr->header_len);

	num_entry = adsp_hdr->num_module_entries;

	tbl = devm_kzalloc(ctx->dev,
		num_entry * sizeof(struct uuid_tbl), GFP_KERNEL);

	if (!tbl)
		return -ENOMEM;

	/*
	 * Populate the UUID table to store module_id
	 * and loadable flags for the module
	 */

	for (i = 0; i < num_entry; i++, mod_entry++) {
		get_canonical_uuid(uuid_str, mod_entry);

		strncpy(tbl[i].uuid, uuid_str, strlen(uuid_str));

		tbl[i].module_id = i;
		tbl[i].is_loadable = mod_entry->type.load_type;
	}

	skl->tbl = tbl;
	skl->num_modules = num_entry;
	return 0;
}
EXPORT_SYMBOL_GPL(parse_fw_bin);
