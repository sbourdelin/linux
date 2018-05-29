// SPDX-License-Identifier: GPL-2.0

/*
 * This file contains the routines for mapping IO areas
 *
 *  Derived from arch/powerpc/mm/pgtable_32.c and
 *  arch/powerpc/mm/pgtable_64.c
 *
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/mm.h>
#include <linux/vmalloc.h>
#include <linux/init.h>
#include <linux/highmem.h>
#include <linux/memblock.h>
#include <linux/slab.h>

#include <asm/pgtable.h>
#include <asm/pgalloc.h>
#include <asm/fixmap.h>
#include <asm/io.h>
#include <asm/setup.h>
#include <asm/sections.h>
#include <asm/machdep.h>

#include "mmu_decl.h"

#if defined(CONFIG_PPC_BOOK3S_64) || defined(CONFIG_PPC32)
unsigned long ioremap_bot;
#else
unsigned long ioremap_bot = IOREMAP_BASE;
#endif
EXPORT_SYMBOL(ioremap_bot);	/* aka VMALLOC_END */

/**
 * __ioremap_at - Low level function to establish the page tables
 *                for an IO mapping
 */
void __iomem * __ioremap_at(phys_addr_t pa, void *ea, unsigned long size,
			    unsigned long flags)
{
	unsigned long i;

	/* Make sure we have the base flags */
	if ((flags & _PAGE_PRESENT) == 0)
		flags |= pgprot_val(PAGE_KERNEL);

	/* Non-cacheable page cannot be coherent */
	if (flags & _PAGE_NO_CACHE)
		flags &= ~_PAGE_COHERENT;

	/* We don't support the 4K PFN hack with ioremap */
	if (flags & H_PAGE_4K_PFN)
		return NULL;

	WARN_ON(pa & ~PAGE_MASK);
	WARN_ON(((unsigned long)ea) & ~PAGE_MASK);
	WARN_ON(size & ~PAGE_MASK);

	for (i = 0; i < size; i += PAGE_SIZE)
		if (map_kernel_page((unsigned long)ea+i, pa+i, flags))
			return NULL;

	return (void __iomem *)ea;
}
EXPORT_SYMBOL(__ioremap_at);

/**
 * __iounmap_from - Low level function to tear down the page tables
 *                  for an IO mapping. This is used for mappings that
 *                  are manipulated manually, like partial unmapping of
 *                  PCI IOs or ISA space.
 */
void __iounmap_at(void *ea, unsigned long size)
{
	WARN_ON(((unsigned long)ea) & ~PAGE_MASK);
	WARN_ON(size & ~PAGE_MASK);

	unmap_kernel_range((unsigned long)ea, size);
}
EXPORT_SYMBOL(__iounmap_at);

void __iomem * __ioremap_caller(phys_addr_t addr, unsigned long size,
				unsigned long flags, void *caller)
{
	phys_addr_t paligned;
	void __iomem *ret;

	/*
	 * Choose an address to map it to.
	 * Once the imalloc system is running, we use it.
	 * Before that, we map using addresses going
	 * up from ioremap_bot.  imalloc will use
	 * the addresses from ioremap_bot through
	 * IMALLOC_END
	 *
	 */
	paligned = addr & PAGE_MASK;
	size = PAGE_ALIGN(addr + size) - paligned;

	if ((size == 0) || (paligned == 0))
		return NULL;

	/*
	 * If the address lies within the first 16 MB, assume it's in ISA
	 * memory space
	 */
	if (IS_ENABLED(CONFIG_PPC32) && paligned < 16*1024*1024)
		paligned += _ISA_MEM_BASE;

	/*
	 * Don't allow anybody to remap normal RAM that we're using.
	 * mem_init() sets high_memory so only do the check after that.
	 */
	if (!IS_ENABLED(CONFIG_CRASH_DUMP) &&
	    slab_is_available() && (paligned < virt_to_phys(high_memory)) &&
	    page_is_ram(__phys_to_pfn(paligned))) {
		printk("__ioremap(): phys addr 0x%llx is RAM lr %ps\n",
		       (u64)paligned, __builtin_return_address(0));
		return NULL;
	}

	/*
	 * Is it already mapped?  Perhaps overlapped by a previous
	 * mapping.
	 */
	ret = (void __iomem *)p_block_mapped(paligned);
	if (ret)
		goto out;

	if (slab_is_available()) {
		struct vm_struct *area;

#ifdef CONFIG_PPC_GUARDED_PAGE_IN_PMD
		if (!(flags & _PAGE_GUARDED))
			area = __get_vm_area_caller(size, VM_IOREMAP,
						    VMALLOC_START, VMALLOC_END,
						    caller);
		else
#endif
			area = __get_vm_area_caller(size, VM_IOREMAP,
						    ioremap_bot, IOREMAP_END,
						    caller);
		if (area == NULL)
			return NULL;

		area->phys_addr = paligned;
		ret = __ioremap_at(paligned, area->addr, size, flags);
		if (!ret)
			vunmap(area->addr);
	} else {
		ret = __ioremap_at(paligned, (void *)ioremap_bot, size, flags);
		if (ret)
			ioremap_bot += size;
	}
out:
	if (ret)
		ret += (unsigned long)addr & ~PAGE_MASK;
	return ret;
}

void __iomem * __ioremap(phys_addr_t addr, unsigned long size,
			 unsigned long flags)
{
	return __ioremap_caller(addr, size, flags, __builtin_return_address(0));
}
EXPORT_SYMBOL(__ioremap);

void __iomem * ioremap(phys_addr_t addr, unsigned long size)
{
	unsigned long flags = pgprot_val(pgprot_noncached(__pgprot(0)));
	void *caller = __builtin_return_address(0);

	if (ppc_md.ioremap)
		return ppc_md.ioremap(addr, size, flags, caller);
	return __ioremap_caller(addr, size, flags, caller);
}
EXPORT_SYMBOL(ioremap);

void __iomem * ioremap_wc(phys_addr_t addr, unsigned long size)
{
	unsigned long flags = pgprot_val(pgprot_noncached_wc(__pgprot(0)));
	void *caller = __builtin_return_address(0);

	if (ppc_md.ioremap)
		return ppc_md.ioremap(addr, size, flags, caller);
	return __ioremap_caller(addr, size, flags, caller);
}
EXPORT_SYMBOL(ioremap_wc);

void __iomem * ioremap_prot(phys_addr_t addr, unsigned long size,
			     unsigned long flags)
{
	void *caller = __builtin_return_address(0);

	/* writeable implies dirty for kernel addresses */
	if ((flags & (_PAGE_WRITE | _PAGE_RO)) != _PAGE_RO)
		flags |= _PAGE_DIRTY | _PAGE_HWWRITE;

	/* we don't want to let _PAGE_EXEC leak out */
	flags &= ~_PAGE_EXEC;
	/*
	 * Force kernel mapping.
	 */
	flags &= ~_PAGE_USER;
	flags |= _PAGE_PRIVILEGED;

	if (ppc_md.ioremap)
		return ppc_md.ioremap(addr, size, flags, caller);
	return __ioremap_caller(addr, size, flags, caller);
}
EXPORT_SYMBOL(ioremap_prot);

/*
 * Unmap an IO region and remove it from imalloc'd list.
 * Access to IO memory should be serialized by driver.
 */
void __iounmap(volatile void __iomem *token)
{
	void *addr;

	if (!slab_is_available())
		return;

	addr = (void *) ((unsigned long __force)
			 PCI_FIX_ADDR(token) & PAGE_MASK);

	/*
	 * If mapped by BATs then there is nothing to do.
	 * Calling vfree() generates a benign warning.
	 */
	if (v_block_mapped((unsigned long)addr))
		return;

	if ((unsigned long)addr < ioremap_bot) {
		printk(KERN_WARNING "Attempt to iounmap early bolted mapping"
		       " at 0x%p\n", addr);
		return;
	}
	vunmap(addr);
}
EXPORT_SYMBOL(__iounmap);

void iounmap(volatile void __iomem *token)
{
	if (ppc_md.iounmap)
		ppc_md.iounmap(token);
	else
		__iounmap(token);
}
EXPORT_SYMBOL(iounmap);
