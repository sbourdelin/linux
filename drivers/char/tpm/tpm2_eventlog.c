/*
 * Copyright (C) 2016 IBM Corporation
 *
 * Authors:
 *      Nayna Jain <nayna@linux.vnet.ibm.com>
 *
 * Access to TPM 2.0 event log as written by Firmware.
 * It assumes that writer of event log has followed TCG Spec 2.0
 * has written the event struct data in little endian. With that,
 * it doesn't need any endian conversion for structure content.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */

#include <linux/seq_file.h>
#include <linux/fs.h>
#include <linux/security.h>
#include <linux/module.h>
#include <linux/slab.h>

#include "tpm.h"
#include "tpm_eventlog.h"

static int calc_tpm2_event_size(struct tcg_pcr_event2 *event,
		struct tcg_pcr_event *event_header)
{
	struct tcg_efi_specid_event *efispecid;
	struct tcg_event_field *event_field;
	void *marker, *marker_start;
	int i, j;
	u16 halg;
	u32 halg_size;
	size_t size = 0;

	/*
	 * NOTE: TPM 2.0 supports extend to multiple PCR Banks. This implies
	 * event log also has multiple digest values, one for each PCR Bank.
	 * This is called Crypto Agile Log Entry Format.
	 * TCG EFI Protocol Specification defines the procedure to parse
	 * the event log. Below code implements this procedure to parse
	 * correctly the Crypto agile log entry format.
	 * Example of Crypto Agile Log Digests Format :
	 * digest_values.count = 2;
	 * digest_values.digest[0].alg_id = sha1;
	 * digest_values.digest[0].digest.sha1 = {20 bytes raw data};
	 * digest_values.digest[1].alg_id = sha256;
	 * digest_values.digest[1].digest.sha256 = {32 bytes raw data};
	 * Offset of eventsize is sizeof(count) + sizeof(alg_id) + 20
	 *			+ sizeof(alg_id) + 32;
	 *
	 * Since, offset of event_size can vary based on digests count, offset
	 * has to be calculated at run time. void *marker is used to traverse
	 * the dynamic structure and calculate the offset of event_size.
	 */

	marker = event;
	marker_start = marker;
	marker = marker + sizeof(event->pcr_idx) + sizeof(event->event_type)
		+ sizeof(event->digests.count);

	efispecid = (struct tcg_efi_specid_event *) event_header->event;

	for (i = 0; (i < event->digests.count) && (i < HASH_COUNT); i++) {
		halg_size = sizeof(event->digests.digests[i].alg_id);
		memcpy(&halg, marker, halg_size);
		marker = marker + halg_size;
		for (j = 0; (j < efispecid->num_algs); j++) {
			if (halg == efispecid->digest_sizes[j].alg_id) {
				marker = marker +
					efispecid->digest_sizes[j].digest_size;
				break;
			}
		}
	}

	event_field = (struct tcg_event_field *) marker;
	marker = marker + sizeof(event_field->event_size)
		+ event_field->event_size;
	size = marker - marker_start;

	if ((event->event_type == 0) && (event_field->event_size == 0))
		return 0;

	return size;
}

static void *tpm2_bios_measurements_start(struct seq_file *m, loff_t *pos)
{
	struct tpm_chip *chip = m->private;
	struct tpm_bios_log *log = &chip->log;
	void *addr = log->bios_event_log;
	void *limit = log->bios_event_log_end;
	struct tcg_pcr_event *event_header;
	struct tcg_pcr_event2 *event;
	int i;
	size_t size = 0;

	event_header = addr;

	size = sizeof(struct tcg_pcr_event) - sizeof(event_header->event)
		+ event_header->event_size;


	if (*pos == 0) {
		if (addr + size < limit) {
			if ((event_header->event_type == 0) &&
					(event_header->event_size == 0))
				return NULL;
			return SEQ_START_TOKEN;
		}
	}

	if (*pos > 0) {
		addr += size;
		event = addr;
		size = calc_tpm2_event_size(event, event_header);
		if ((addr + size >=  limit) || (size == 0))
			return NULL;
	}

	/* read over *pos measurements */
	for (i = 0; i < (*pos - 1); i++) {
		event = addr;
		size = calc_tpm2_event_size(event, event_header);

		if ((addr + size >= limit) || (size == 0))
			return NULL;
		addr += size;
	}

	return addr;
}

static void *tpm2_bios_measurements_next(struct seq_file *m, void *v,
		loff_t *pos)
{
	struct tcg_pcr_event *event_header;
	struct tcg_pcr_event2 *event;
	struct tpm_chip *chip = m->private;
	struct tpm_bios_log *log = &chip->log;
	void *limit = log->bios_event_log_end;
	void *marker;
	size_t event_size = 0;

	event_header = log->bios_event_log;

	if (v == SEQ_START_TOKEN) {
		event_size = sizeof(struct tcg_pcr_event)
			- sizeof(event_header->event)
			+ event_header->event_size;
		marker = event_header;
	} else {
		event = v;
		event_size = calc_tpm2_event_size(event, event_header);
		if (event_size == 0)
			return NULL;
		marker =  event;
	}

	marker = marker + event_size;
	if (marker >= limit)
		return NULL;
	v = marker;
	event = v;

	event_size = calc_tpm2_event_size(event, event_header);
	if (((v + event_size) >= limit) || (event_size == 0))
		return NULL;

	(*pos)++;
	return v;
}

static void tpm2_bios_measurements_stop(struct seq_file *m, void *v)
{
}

static int tpm2_binary_bios_measurements_show(struct seq_file *m, void *v)
{
	struct tpm_chip *chip = m->private;
	struct tpm_bios_log *log = &chip->log;
	struct tcg_pcr_event *event_header = log->bios_event_log;
	struct tcg_pcr_event2 *event = v;
	void *temp_ptr;
	size_t size = 0;

	if (v == SEQ_START_TOKEN) {
		size = sizeof(struct tcg_pcr_event)
			- sizeof(event_header->event)
			+ event_header->event_size;

		temp_ptr = event_header;

		if (size > 0)
			seq_write(m, temp_ptr, size);
	} else {
		size = calc_tpm2_event_size(event, event_header);
		temp_ptr = event;
		if (size > 0)
			seq_write(m, temp_ptr, size);
	}

	return 0;
}

const struct seq_operations tpm2_binary_b_measurements_seqops = {
	.start = tpm2_bios_measurements_start,
	.next = tpm2_bios_measurements_next,
	.stop = tpm2_bios_measurements_stop,
	.show = tpm2_binary_bios_measurements_show,
};
