// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2012 Intel Corporation
 * Author: Josh Triplett <josh@joshtriplett.org>
 *
 * Based on the bgrt driver:
 * Copyright 2012 Red Hat, Inc <mjg@redhat.com>
 * Author: Matthew Garrett
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/acpi.h>
#include <linux/efi.h>
#include <linux/efi-bgrt.h>

struct acpi_table_bgrt bgrt_tab;
size_t bgrt_image_size;

struct bmp_header {
	u16 id;
	u32 size;
} __packed;

void __init efi_bgrt_init(unsigned long rsdp_phys)
{
	void *image;
	struct bmp_header bmp_header;
	struct acpi_table_bgrt *bgrt = &bgrt_tab;
	struct acpi_table_bgrt *table = NULL;
	struct acpi_table_rsdp *rsdp;
	struct acpi_table_header *hdr;
	u64 xsdt_phys = 0;
	u32 rsdt_phys = 0;
	size_t len;

	if (!efi_enabled(EFI_MEMMAP))
		return;

	/* map the root pointer table to find the xsdt/rsdt values */
	rsdp = early_memremap_ro(rsdp_phys, sizeof(*rsdp));
	if (rsdp) {
		if (ACPI_VALIDATE_RSDP_SIG(rsdp->signature)) {
			xsdt_phys = rsdp->xsdt_physical_address;
			rsdt_phys = rsdp->rsdt_physical_address;
		}
		early_memunmap(rsdp, sizeof(*rsdp));
	}

	if (WARN_ON(!xsdt_phys && !rsdt_phys))
		return;

	/* obtain the length of whichever table we will be using */
	hdr = early_memremap_ro(xsdt_phys ?: rsdt_phys, sizeof(*hdr));
	if (WARN_ON(!hdr))
		return;
	len = hdr->length;
	early_memunmap(hdr, sizeof(*hdr));

	/* remap with the correct length */
	hdr = early_memremap_ro(xsdt_phys ?: rsdt_phys, len);
	if (WARN_ON(!hdr))
		return;

	if (xsdt_phys) {
		struct acpi_table_xsdt *xsdt = (void *)hdr;
		int i;

		for (i = 0; i < (len - sizeof(*hdr)) / sizeof(u64); i++) {
			table = early_memremap_ro(xsdt->table_offset_entry[i],
						  sizeof(*table));
			if (WARN_ON(!table))
				break;

			if (ACPI_COMPARE_NAME(table->header.signature,
					      ACPI_SIG_BGRT))
				break;
			early_memunmap(table, sizeof(*table));
			table = NULL;
		}
	} else if (rsdt_phys) {
		struct acpi_table_rsdt *rsdt = (void *)hdr;
		int i;

		for (i = 0; i < (len - sizeof(*hdr)) / sizeof(u32); i++) {
			table = early_memremap_ro(rsdt->table_offset_entry[i],
						  sizeof(*table));
			if (WARN_ON(!table))
				break;

			if (ACPI_COMPARE_NAME(table->header.signature,
					      ACPI_SIG_BGRT))
				break;
			early_memunmap(table, sizeof(*table));
			table = NULL;
		}
	}
	early_memunmap(hdr, len);

	if (!table)
		return;

	len = table->header.length;
	memcpy(bgrt, table, min(len, sizeof(bgrt_tab)));
	early_memunmap(table, sizeof(*table));

	if (len < sizeof(bgrt_tab)) {
		pr_notice("Ignoring BGRT: invalid length %lu (expected %zu)\n",
		       len, sizeof(bgrt_tab));
		goto out;
	}

	if (bgrt->version != 1) {
		pr_notice("Ignoring BGRT: invalid version %u (expected 1)\n",
		       bgrt->version);
		goto out;
	}
	if (bgrt->status & 0xfe) {
		pr_notice("Ignoring BGRT: reserved status bits are non-zero %u\n",
		       bgrt->status);
		goto out;
	}
	if (bgrt->image_type != 0) {
		pr_notice("Ignoring BGRT: invalid image type %u (expected 0)\n",
		       bgrt->image_type);
		goto out;
	}
	if (!bgrt->image_address) {
		pr_notice("Ignoring BGRT: null image address\n");
		goto out;
	}

	if (efi_mem_type(bgrt->image_address) != EFI_BOOT_SERVICES_DATA) {
		pr_notice("Ignoring BGRT: invalid image address\n");
		goto out;
	}
	image = early_memremap(bgrt->image_address, sizeof(bmp_header));
	if (!image) {
		pr_notice("Ignoring BGRT: failed to map image header memory\n");
		goto out;
	}

	memcpy(&bmp_header, image, sizeof(bmp_header));
	early_memunmap(image, sizeof(bmp_header));
	if (bmp_header.id != 0x4d42) {
		pr_notice("Ignoring BGRT: Incorrect BMP magic number 0x%x (expected 0x4d42)\n",
			bmp_header.id);
		goto out;
	}
	bgrt_image_size = bmp_header.size;
	efi_mem_reserve(bgrt->image_address, bgrt_image_size);

	return;
out:
	memset(bgrt, 0, sizeof(bgrt_tab));
}
