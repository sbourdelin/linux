/*
 * APEI Boot Error Record Table (BERT) support
 *
 * Copyright 2011 Intel Corp.
 *   Author: Huang Ying <ying.huang@intel.com>
 *
 * Under normal circumstances, when a hardware error occurs, kernel
 * will be notified via NMI, MCE or some other method, then kernel
 * will process the error condition, report it, and recover it if
 * possible. But sometime, the situation is so bad, so that firmware
 * may choose to reset directly without notifying Linux kernel.
 *
 * Linux kernel can use the Boot Error Record Table (BERT) to get the
 * un-notified hardware errors that occurred in a previous boot.
 *
 * For more information about BERT, please refer to ACPI Specification
 * version 4.0, section 17.3.1
 *
 * This file is licensed under GPLv2.
 *
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/acpi.h>
#include <linux/io.h>

#include "apei-internal.h"

#undef pr_fmt
#define pr_fmt(fmt) "BERT: " fmt

static int bert_disable;

static void __init bert_print_all(struct acpi_bert_region *region,
				  unsigned int region_len)
{
	/*
	 * We use cper_estatus_* which uses struct acpi_hest_generic_status,
	 * struct acpi_hest_generic_status and acpi_bert_region are the same
	 * (Generic Error Status Block), so we declare the "estatus" here.
	 */
	struct acpi_hest_generic_status *estatus =
		(struct acpi_hest_generic_status *)region;
	int remain = region_len;
	u32 estatus_len;

	/* The records have been polled*/
	if (!estatus->block_status)
		return;

	while (remain > sizeof(struct acpi_bert_region)) {
		/*
		 * Test Generic Error Status Block first,
		 * if the data(Offset, Length) is invalid, we just return,
		 * because we can't trust the length data from this block.
		 */
		if (cper_estatus_check(estatus)) {
			pr_err(FW_BUG "Invalid error record\n");
			return;
		}

		estatus_len = cper_estatus_len(estatus);
		if (remain < estatus_len) {
			pr_err(FW_BUG "Invalid status block length (%u)\n",
			       estatus_len);
			return;
		}

		pr_info_once("Error records from previous boot:\n");

		cper_estatus_print(KERN_INFO HW_ERR, estatus);

		/*
		 * Because the boot error source is "one-time polled" type,
		 * clear Block Status of current Generic Error Status Block,
		 * once it's printed.
		 */
		estatus->block_status = 0;

		estatus = (void *)estatus + estatus_len;
		if (!estatus->block_status)
			return;	/* No more error records */

		remain -= estatus_len;
	}
}

static int __init setup_bert_disable(char *str)
{
	bert_disable = 1;

	return 0;
}
__setup("bert_disable", setup_bert_disable);

static int __init bert_check_table(struct acpi_table_bert *bert_tab)
{
	if (bert_tab->header.length < sizeof(struct acpi_table_bert) ||
	    bert_tab->region_length < sizeof(struct acpi_bert_region))
		return -EINVAL;

	return 0;
}

static int __init bert_init(void)
{
	struct acpi_bert_region *boot_error_region;
	struct acpi_table_bert *bert_tab;
	unsigned int region_len;
	acpi_status status;
	int rc = 0;

	if (acpi_disabled)
		return 0;

	if (bert_disable) {
		pr_info("Boot Error Record Table support is disabled\n");
		return 0;
	}

	status = acpi_get_table(ACPI_SIG_BERT, 0, (struct acpi_table_header **)&bert_tab);
	if (status == AE_NOT_FOUND)
		return 0;
	if (ACPI_FAILURE(status)) {
		pr_err("get table failed, %s\n", acpi_format_exception(status));
		return -EINVAL;
	}

	rc = bert_check_table(bert_tab);
	if (rc) {
		pr_err(FW_BUG "table invalid\n");
		return rc;
	}

	region_len = bert_tab->region_length;
	if (!request_mem_region(bert_tab->address, region_len, "APEI BERT")) {
		pr_err("Can't request iomem region <%016llx-%016llx>\n",
		       (unsigned long long)bert_tab->address,
		       (unsigned long long)bert_tab->address + region_len - 1);
		return -EIO;
	}

	boot_error_region = ioremap_cache(bert_tab->address, region_len);
	if (boot_error_region) {
		bert_print_all(boot_error_region, region_len);
		iounmap(boot_error_region);
	} else {
		rc = -ENOMEM;
	}

	release_mem_region(bert_tab->address, region_len);

	return rc;
}

late_initcall(bert_init);
