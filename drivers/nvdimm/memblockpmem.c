// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (c) 2018 IBM Corporation
 */

#define pr_fmt(fmt) "memblock pmem: " fmt

#include <linux/libnvdimm.h>
#include <linux/bootmem.h>
#include <linux/memblock.h>
#include <linux/mmzone.h>
#include <linux/cpu.h>
#include <linux/platform_device.h>
#include <linux/init.h>
#include <linux/ioport.h>
#include <linux/ctype.h>
#include <linux/slab.h>

/*
 * Align pmem reservations to the section size so we don't have issues with
 * memory hotplug
 */
#ifdef CONFIG_SPARSEMEM
#define BOOTPMEM_ALIGN (1UL << SECTION_SIZE_BITS)
#else
#define BOOTPMEM_ALIGN PFN_DEFAULT_ALIGNMENT
#endif

static __initdata u64 pmem_size;
static __initdata phys_addr_t pmem_stolen_memory;

static void alloc_pmem_from_memblock(void)
{

	pmem_stolen_memory = memblock_alloc_base(pmem_size,
						 BOOTPMEM_ALIGN,
						 MEMBLOCK_ALLOC_ACCESSIBLE);
	if (!pmem_stolen_memory) {
		pr_err("Failed to allocate memory for PMEM from memblock\n");
		return;
	}

	/*
	 * Remove from the memblock reserved range
	 */
	memblock_free(pmem_stolen_memory, pmem_size);

	/*
	 * Remove from the memblock memory range.
	 */
	memblock_remove(pmem_stolen_memory, pmem_size);
	pr_info("Allocated %ld memory at 0x%lx\n", (unsigned long)pmem_size,
		(unsigned long)pmem_stolen_memory);
	return;
}

/*
 * pmemmap=ss[KMG]
 *
 * This is similar to the memremap=offset[KMG]!size[KMG] paramater
 * for adding a legacy pmem range to the e820 map on x86, but it's
 * platform agnostic.
 *
 * e.g. pmemmap=16G allocates 16G pmem region
 */
static int __init parse_pmemmap(char *p)
{
	char *old_p = p;

	if (!p)
		return -EINVAL;

	pmem_size = memparse(p, &p);
	if (p == old_p)
		return -EINVAL;

	alloc_pmem_from_memblock();
	return 0;
}
early_param("pmemmap", parse_pmemmap);

static __init int register_e820_pmem(void)
{
	struct resource *res, *conflict;
        struct platform_device *pdev;

	if (!pmem_stolen_memory)
		return 0;

	res = kzalloc(sizeof(*res), GFP_KERNEL);
	if (!res)
		return -1;

	memset(res, 0, sizeof(*res));
	res->start = pmem_stolen_memory;
	res->end = pmem_stolen_memory + pmem_size - 1;
	res->name = "Persistent Memory (legacy)";
	res->desc = IORES_DESC_PERSISTENT_MEMORY_LEGACY;
	res->flags = IORESOURCE_MEM;

	conflict = insert_resource_conflict(&iomem_resource, res);
	if (conflict) {
		pr_err("%pR conflicts, try insert below %pR\n", res, conflict);
		kfree(res);
		return -1;
	}
	/*
	 * See drivers/nvdimm/e820.c for the implementation, this is
	 * simply here to trigger the module to load on demand.
	 */
	pdev = platform_device_alloc("e820_pmem", -1);

	return platform_device_add(pdev);
}
device_initcall(register_e820_pmem);
