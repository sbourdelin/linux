// SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause)
// Copyright(c) 2016-17 Intel Corporation.

#include <linux/freezer.h>
#include <linux/highmem.h>
#include <linux/kthread.h>
#include <linux/pagemap.h>
#include <linux/ratelimit.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <asm/sgx.h>

struct sgx_epc_section sgx_epc_sections[SGX_MAX_EPC_SECTIONS];
EXPORT_SYMBOL_GPL(sgx_epc_sections);

static int sgx_nr_epc_sections;

static __init void sgx_free_epc_section(struct sgx_epc_section *section)
{
	int i;

	for (i = 0; i < section->free_cnt && section->pages[i]; i++)
		kfree(section->pages[i]);
	kfree(section->pages);
	iounmap(section->va);
}

static __init int sgx_init_epc_section(u64 addr, u64 size, unsigned long index,
				       struct sgx_epc_section *section)
{
	unsigned long nr_pages = size >> PAGE_SHIFT;
	unsigned long i;

	section->va = ioremap_cache(addr, size);
	if (!section->va)
		return -ENOMEM;

	section->pa = addr;
	section->free_cnt = nr_pages;
	spin_lock_init(&section->lock);

	section->pages = kcalloc(nr_pages, sizeof(struct sgx_epc_page *),
				 GFP_KERNEL);
	if (!section->pages)
		goto out;

	for (i = 0; i < nr_pages; i++) {
		section->pages[i] = kzalloc(sizeof(struct sgx_epc_page),
					    GFP_KERNEL);
		if (!section->pages[i])
			goto out;

		section->pages[i]->desc = (addr + (i << PAGE_SHIFT)) | index;
	}

	return 0;
out:
	sgx_free_epc_section(section);
	return -ENOMEM;
}

static __init void sgx_page_cache_teardown(void)
{
	int i;

	for (i = 0; i < sgx_nr_epc_sections; i++)
		sgx_free_epc_section(&sgx_epc_sections[i]);
}

/**
 * A section metric is concatenated in a way that @low bits 12-31 define the
 * bits 12-31 of the metric and @high bits 0-19 define the bits 32-51 of the
 * metric.
 */
static inline u64 sgx_calc_section_metric(u64 low, u64 high)
{
	return (low & GENMASK_ULL(31, 12)) +
	       ((high & GENMASK_ULL(19, 0)) << 32);
}

static __init int sgx_page_cache_init(void)
{
	u32 eax, ebx, ecx, edx, type;
	u64 pa, size;
	int ret;
	int i;

	BUILD_BUG_ON(SGX_MAX_EPC_SECTIONS > (SGX_EPC_SECTION_MASK + 1));

	for (i = 0; i < SGX_MAX_EPC_SECTIONS; i++) {
		cpuid_count(SGX_CPUID, i + SGX_CPUID_FIRST_VARIABLE_SUB_LEAF,
			    &eax, &ebx, &ecx, &edx);

		type = eax & SGX_CPUID_SUB_LEAF_TYPE_MASK;
		if (type == SGX_CPUID_SUB_LEAF_INVALID)
			break;
		if (type != SGX_CPUID_SUB_LEAF_EPC_SECTION) {
			pr_err_once("sgx: Unknown sub-leaf type: %u\n", type);
			continue;
		}

		pa = sgx_calc_section_metric(eax, ebx);
		size = sgx_calc_section_metric(ecx, edx);
		pr_info("sgx: EPC section 0x%llx-0x%llx\n", pa, pa + size - 1);

		ret = sgx_init_epc_section(pa, size, i, &sgx_epc_sections[i]);
		if (ret) {
			sgx_page_cache_teardown();
			return ret;
		}

		sgx_nr_epc_sections++;
	}

	if (!sgx_nr_epc_sections) {
		pr_err("sgx: There are zero EPC sections.\n");
		return -ENODEV;
	}

	return 0;
}

static __init int sgx_init(void)
{
	int ret;

	if (!boot_cpu_has(X86_FEATURE_SGX))
		return false;

	ret = sgx_page_cache_init();
	if (ret)
		return ret;

	return 0;
}

arch_initcall(sgx_init);
