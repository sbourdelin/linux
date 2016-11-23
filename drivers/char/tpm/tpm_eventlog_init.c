/*
 * Copyright (C) 2005, 2012 IBM Corporation
 *
 * Authors:
 *	Kent Yoder <key@linux.vnet.ibm.com>
 *	Seiji Munetoh <munetoh@jp.ibm.com>
 *	Stefan Berger <stefanb@us.ibm.com>
 *	Reiner Sailer <sailer@watson.ibm.com>
 *	Kylene Hall <kjhall@us.ibm.com>
 *	Nayna Jain <nayna@linux.vnet.ibm.com>
 *
 * Maintained by: <tpmdd-devel@lists.sourceforge.net>
 *
 * Defines common initialization functions to access
 * firmware event log for TPM 1.2 and TPM 2.0
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 *
 */

#include <linux/seq_file.h>
#include <linux/fs.h>
#include <linux/security.h>
#include <linux/module.h>
#include <linux/slab.h>

#include "tpm.h"
#include "tpm_eventlog.h"

static int tpm_bios_measurements_release(struct inode *inode,
					 struct file *file)
{
	struct seq_file *seq = (struct seq_file *)file->private_data;
	struct tpm_chip *chip = (struct tpm_chip *)seq->private;

	put_device(&chip->dev);

	return seq_release(inode, file);
}

static int tpm_bios_measurements_open(struct inode *inode,
					    struct file *file)
{
	int err;
	struct seq_file *seq;
	struct tpm_chip_seqops *chip_seqops;
	const struct seq_operations *seqops;
	struct tpm_chip *chip;

	inode_lock(inode);
	if (!inode->i_private) {
		inode_unlock(inode);
		return -ENODEV;
	}
	chip_seqops = (struct tpm_chip_seqops *)inode->i_private;
	seqops = chip_seqops->seqops;
	chip = chip_seqops->chip;
	get_device(&chip->dev);
	inode_unlock(inode);

	/* now register seq file */
	err = seq_open(file, seqops);
	if (!err) {
		seq = file->private_data;
		seq->private = chip;
	}

	return err;
}

static const struct file_operations tpm_bios_measurements_ops = {
	.owner = THIS_MODULE,
	.open = tpm_bios_measurements_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = tpm_bios_measurements_release,
};

static int is_bad(void *p)
{
	if (!p)
		return 1;
	if (IS_ERR(p) && (PTR_ERR(p) != -ENODEV))
		return 1;
	return 0;
}

static int tpm_read_log(struct tpm_chip *chip)
{
	int rc;

	if (chip->log.bios_event_log != NULL) {
		dev_dbg(&chip->dev,
			"%s: ERROR - event log already initialized\n",
			__func__);
		return -EFAULT;
	}

	rc = tpm_read_log_acpi(chip);
	if (rc != -ENODEV)
		return rc;

	return tpm_read_log_of(chip);
}

/*
 * tpm_bios_log_setup() - Read the event log from the firmware
 * @chip: TPM chip to use.
 *
 * If an event log is found then the securityfs files are setup to
 * export it to userspace, otherwise nothing is done.
 *
 * Returns -ENODEV if the firmware has no event log.
 */
int tpm_bios_log_setup(struct tpm_chip *chip)
{
	const char *name = dev_name(&chip->dev);
	unsigned int cnt;
	int rc = 0;

	if (chip->flags & TPM_CHIP_FLAG_TPM2)
		return 0;

	rc = tpm_read_log(chip);
	if (rc)
		return rc;

	cnt = 0;
	chip->bios_dir[cnt] = securityfs_create_dir(name, NULL);
	if (is_bad(chip->bios_dir[cnt]))
		goto err;
	cnt++;

	chip->bin_log_seqops.chip = chip;
	chip->bin_log_seqops.seqops = &tpm_binary_b_measurements_seqops;

	chip->bios_dir[cnt] =
	    securityfs_create_file("binary_bios_measurements",
				   0440, chip->bios_dir[0],
				   (void *)&chip->bin_log_seqops,
				   &tpm_bios_measurements_ops);
	if (is_bad(chip->bios_dir[cnt]))
		goto err;
	cnt++;

	chip->ascii_log_seqops.chip = chip;
	chip->ascii_log_seqops.seqops = &tpm_ascii_b_measurements_seqops;

	chip->bios_dir[cnt] =
	    securityfs_create_file("ascii_bios_measurements",
				   0440, chip->bios_dir[0],
				   (void *)&chip->ascii_log_seqops,
				   &tpm_bios_measurements_ops);
	if (is_bad(chip->bios_dir[cnt]))
		goto err;
	cnt++;

	return 0;

err:
	chip->bios_dir[cnt] = NULL;
	tpm_bios_log_teardown(chip);
	return -EIO;
}

void tpm_bios_log_teardown(struct tpm_chip *chip)
{
	int i;
	struct inode *inode;

	/* securityfs_remove currently doesn't take care of handling sync
	 * between removal and opening of pseudo files. To handle this, a
	 * workaround is added by making i_private = NULL here during removal
	 * and to check it during open(), both within inode_lock()/unlock().
	 * This design ensures that open() either safely gets kref or fails.
	 */
	for (i = (TPM_NUM_EVENT_LOG_FILES - 1); i >= 0; i--) {
		if (chip->bios_dir[i]) {
			inode = d_inode(chip->bios_dir[i]);
			inode_lock(inode);
			inode->i_private = NULL;
			inode_unlock(inode);
			securityfs_remove(chip->bios_dir[i]);
		}
	}
}
