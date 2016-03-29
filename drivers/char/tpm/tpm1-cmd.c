/*
 * Copyright (C) 2004 IBM Corporation
 * Copyright (C) 2014, 2016 Intel Corporation
 *
 * Authors:
 * Leendert van Doorn <leendert@watson.ibm.com>
 * Dave Safford <safford@watson.ibm.com>
 * Reiner Sailer <sailer@watson.ibm.com>
 * Kylene Hall <kjhall@us.ibm.com>
 *
 * Maintained by: <tpmdd-devel@lists.sourceforge.net>
 *
 * Device driver for TCG/TCPA TPM (trusted platform module).
 * Specifications at www.trustedcomputinggroup.org
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation, version 2 of the
 * License.
 *
 * Note, the TPM chip is not interrupt driven (only polling)
 * and can have very long timeouts (minutes!). Hence the unusual
 * calls to msleep.
 *
 */

#include "tpm.h"

int tpm1_pcr_read(struct tpm_chip *chip, int pcr_idx, u8 *res_buf)
{
	int rc;
	struct tpm_cmd_t cmd;

	cmd.header.in.tag = TPM_TAG_RQU_COMMAND;
	cmd.header.in.length = sizeof(struct tpm_input_header) +
			       sizeof(struct tpm_pcrread_in);
	cmd.header.in.ordinal = TPM1_ORD_PCRREAD;
	cmd.params.pcrread_in.pcr_idx = cpu_to_be32(pcr_idx);
	rc = tpm_transmit_cmd(chip, &cmd, sizeof(cmd),
			      "attempting to read a pcr value");

	if (rc == 0)
		memcpy(res_buf, cmd.params.pcrread_out.pcr_result,
		       TPM_DIGEST_SIZE);
	return rc;
}
