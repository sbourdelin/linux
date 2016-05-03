/*
 * Copyright (C) 2016 Anshuman Khandual (khandual@linux.vnet.ibm.com)
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation; either version
 *  2 of the License, or (at your option) any later version.
 */
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/gfp.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/memremap.h>

#include <asm/firmware.h>
#include <asm/hvcall.h>
#include <asm/mmu.h>
#include <asm/pgalloc.h>
#include <asm/uaccess.h>
#include <linux/memory.h>
#include <asm/plpar_wrappers.h>
#include <asm/prom.h>

#define DEVM_MAP_SIZE (1UL << PA_SECTION_SHIFT) * 8

extern void dump_vmemmap(void);
extern struct resmem rmem;

unsigned long devmem_start;
unsigned long devmem_end;

void driver_test_devmem(void)
{
	unsigned long i;
	unsigned long start = devmem_start >> PAGE_SHIFT;
	unsigned long end = devmem_end >> PAGE_SHIFT;

	for(i = start; i < end; i++)
		*(unsigned long *)i = (char)i;

	for(i = start; i < end; i++) {
		if (*(unsigned long *)i != (char)i)
			printk("RMEM: Error data miscompare at %lx\n", i);
	}
	printk("RMEM: Data integrity test successful\n");
}

void driver_memory(unsigned long start_pfn, unsigned long end_pfn)
{
	printk("RMEM: Driver now owns PFN(%lx....%lx)\n", start_pfn, end_pfn);

	devmem_start = start_pfn;
	devmem_end = end_pfn;
	driver_test_devmem();
}

static void dump_reserved(void)
{
	unsigned long i;

	printk("RMEM: Reserved memory sections\n");
	for (i = 0; i < rmem.nr; i++) {
		printk("RMEM: Base %llx Size: %llx Node: %llu\n",
			rmem.mem[i][MEM_BASE], rmem.mem[i][MEM_SIZE],
			rmem.mem[i][MEM_NODE]);
	}
}

static void dump_devmap(resource_size_t start)
{
	struct vmem_altmap *altmap;
	struct dev_pagemap *pgmap;
	struct page_map *pmap;
	struct page *page;
	unsigned long pfn;

	altmap = to_vmem_altmap((unsigned long)pfn_to_page(start >> PAGE_SHIFT));
	if (altmap) {
		printk("RMEM: altmap->base_pfn %lu\n", altmap->base_pfn);
		printk("RMEM: altmap->reserve %lu\n", altmap->reserve);
		printk("RMEM: altmap->free %lu\n", altmap->free);
		printk("RMEM: altmap->align %lu\n", altmap->align);
		printk("RMEM: altmap->alloc %lu\n", altmap->alloc);
	}
	pmap = find_pagemap(start);
	rcu_read_lock();
	pgmap = find_dev_pagemap(start);
	rcu_read_unlock();
	printk("RMEM: pagemap		(%lx)\n", (unsigned long)pmap);
	printk("RMEM: dev_pagemap	(%lx)\n", (unsigned long)pgmap);
	printk("RMEM: pfn range (%lx %lx)\n", pfn_first(pmap), pfn_end(pmap));

	for (pfn = pfn_first(pmap); pfn < pfn_end(pmap); pfn++) {
		page = pfn_to_page(pfn);
		printk("DEVM: pfn(%lx) page(%lx) pagemap(%lx) flags(%lx)\n",
			pfn, (unsigned long)page, (unsigned long)page->pgmap,
			page->flags);
	}
	driver_memory(pfn_first(pmap), pfn_end(pmap));
}

static void simple_translation_test(void __pmem *vaddr)
{
	unsigned long i;

	if (vaddr) {
		unsigned long tmp;
	
		for (i = 0; i < DEVM_MAP_SIZE; i++)
			tmp = *((unsigned long *)vaddr + i);

		printk("RMEM: Read access complete (%lx %lx)\n",
				(unsigned long)vaddr, DEVM_MAP_SIZE);
	}
}

static int rmem_init(void)
{
	struct class *class;
	struct device *dev;
	struct resource *res;
	struct percpu_ref *ref;
	void __pmem *vaddr;
	struct vmem_altmap *altmap;
	struct vmem_altmap __altmap = {
		.base_pfn = rmem.mem[0][0] >> PAGE_SHIFT,
		.reserve = 0,
		.free = 0x100,
		.alloc = 0,
		.align = 0,
	};

	printk("RMEM: Driver loaded\n");
	dump_reserved();

	class = class_create(THIS_MODULE, "rmem");
	if (!class) {
		printk("RMEM: class_create() failed\n");
		goto out;
	}

	dev = device_create(class, NULL, MKDEV(100, 100), NULL, "rmem");
	if (!dev) {
		printk("RMEM: device_create() failed\n");
		goto out;
	}

	res = devm_kzalloc(dev, sizeof(*res), GFP_KERNEL);
	if (!res) {
		printk("RMEM: devm_kzalloc() failed\n");
		goto out;
	}

	ref = devm_kzalloc(dev, sizeof(*ref), GFP_KERNEL);
	if (!res) {
		printk("RMEM: devm_kzalloc() failed\n");
		goto out;
	}

	dump_vmemmap();
	altmap = &__altmap;
	res->start = rmem.mem[0][0];
	res->end = rmem.mem[0][0] + DEVM_MAP_SIZE;
	vaddr = devm_memremap_pages(dev, res, ref, altmap);
	dump_vmemmap();
	
	simple_translation_test(vaddr);
	dump_devmap(res->start);
	return 0;
out:
	return -1;
}

static void rmem_exit(void)
{
	printk("RMEM: rmem driver unloaded\n");
}

module_init(rmem_init);
module_exit(rmem_exit);

MODULE_AUTHOR("Anshuman Khandual <khandual@linux.vnet.ibm.com>");
MODULE_DESCRIPTION("Test driver for device memory");
MODULE_LICENSE("GPL");
