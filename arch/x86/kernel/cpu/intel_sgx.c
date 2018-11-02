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

/* A per-cpu cache for the last known values of IA32_SGXLEPUBKEYHASHx MSRs. */
static DEFINE_PER_CPU(u64 [4], sgx_lepubkeyhash_cache);

/**
 * sgx_alloc_page - Allocate an EPC page
 *
 * Try to grab a page from the free EPC page list.
 *
 * Return:
 *   a pointer to a &struct sgx_epc_page instance,
 *   -errno on error
 */
struct sgx_epc_page *sgx_alloc_page(void)
{
	struct sgx_epc_section *section;
	struct sgx_epc_page *page;
	int i;

	for (i = 0; i < sgx_nr_epc_sections; i++) {
		section = &sgx_epc_sections[i];
		spin_lock(&section->lock);
		if (section->free_cnt) {
			page = section->pages[section->free_cnt - 1];
			section->free_cnt--;
		}
		spin_unlock(&section->lock);

		if (page)
			return page;
	}

	return ERR_PTR(-ENOMEM);
}
EXPORT_SYMBOL_GPL(sgx_alloc_page);

/**
 * __sgx_free_page - Free an EPC page
 * @page:	pointer a previously allocated EPC page
 *
 * EREMOVE an EPC page and insert it back to the list of free pages.
 *
 * Return:
 *   0 on success
 *   SGX error code if EREMOVE fails
 */
int __sgx_free_page(struct sgx_epc_page *page)
{
	struct sgx_epc_section *section = sgx_epc_section(page);
	int ret;

	ret = __eremove(sgx_epc_addr(page));
	if (ret)
		return ret;

	spin_lock(&section->lock);
	section->pages[section->free_cnt++] = page;
	spin_unlock(&section->lock);

	return 0;
}
EXPORT_SYMBOL_GPL(__sgx_free_page);

/**
 * sgx_free_page - Free an EPC page and WARN on failure
 * @page:	pointer to a previously allocated EPC page
 *
 * EREMOVE an EPC page and insert it back to the list of free pages, and WARN
 * if EREMOVE fails.  For use when the call site cannot (or chooses not to)
 * handle failure, i.e. the page is leaked on failure.
 */
void sgx_free_page(struct sgx_epc_page *page)
{
	int ret;

	ret = __sgx_free_page(page);
	WARN(ret > 0, "sgx: EREMOVE returned %d (0x%x)", ret, ret);
}
EXPORT_SYMBOL_GPL(sgx_free_page);

static void sgx_update_lepubkeyhash_msrs(u64 *lepubkeyhash, bool enforce)
{
	u64 __percpu *cache;
	int i;

	cache = per_cpu(sgx_lepubkeyhash_cache, smp_processor_id());
	for (i = 0; i < 4; i++) {
		if (enforce || (lepubkeyhash[i] != cache[i])) {
			wrmsrl(MSR_IA32_SGXLEPUBKEYHASH0 + i, lepubkeyhash[i]);
			cache[i] = lepubkeyhash[i];
		}
	}
}

/**
 * sgx_einit - initialize an enclave
 * @sigstruct:		a pointer a SIGSTRUCT
 * @token:		a pointer an EINITTOKEN (optional)
 * @secs:		a pointer a SECS
 * @lepubkeyhash:	the desired value for IA32_SGXLEPUBKEYHASHx MSRs
 *
 * Execute ENCLS[EINIT], writing the IA32_SGXLEPUBKEYHASHx MSRs according
 * to @lepubkeyhash (if possible and necessary).
 *
 * Return:
 *   0 on success,
 *   -errno or SGX error on failure
 */
int sgx_einit(struct sgx_sigstruct *sigstruct, struct sgx_einittoken *token,
	      struct sgx_epc_page *secs, u64 *lepubkeyhash)
{
	int ret;

	if (!boot_cpu_has(X86_FEATURE_SGX_LC))
		return __einit(sigstruct, token, sgx_epc_addr(secs));

	preempt_disable();
	sgx_update_lepubkeyhash_msrs(lepubkeyhash, false);
	ret = __einit(sigstruct, token, sgx_epc_addr(secs));
	if (ret == SGX_INVALID_EINITTOKEN) {
		sgx_update_lepubkeyhash_msrs(lepubkeyhash, true);
		ret = __einit(sigstruct, token, sgx_epc_addr(secs));
	}
	preempt_enable();
	return ret;
}
EXPORT_SYMBOL(sgx_einit);

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
