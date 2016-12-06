#include <linux/bug.h>
#include <linux/export.h>
#include <linux/types.h>
#include <linux/mmdebug.h>
#include <linux/mm.h>

#include <asm/sections.h>
#include <asm/memory.h>
#include <asm/fixmap.h>

#include "mm.h"

static inline bool __virt_addr_valid(unsigned long x)
{
	if (x < PAGE_OFFSET)
		return false;
	if (arm_lowmem_limit && is_vmalloc_or_module_addr((void *)x))
		return false;
	if (x >= FIXADDR_START && x < FIXADDR_END)
		return false;
	return true;
}

phys_addr_t __virt_to_phys(unsigned long x)
{
	WARN(!__virt_addr_valid(x),
	     "virt_to_phys used for non-linear address :%pK\n", (void *)x);

	return __virt_to_phys_nodebug(x);
}
EXPORT_SYMBOL(__virt_to_phys);

static inline bool __phys_addr_valid(unsigned long x)
{
	/* This is bounds checking against the kernel image only.
	 * __pa_symbol should only be used on kernel symbol addresses.
	 */
	if (x < (unsigned long)KERNEL_START ||
	    x > (unsigned long)KERNEL_END)
		return false;

	return true;
}

phys_addr_t __phys_addr_symbol(unsigned long x)
{
	VIRTUAL_BUG_ON(!__phys_addr_valid(x));

	return __pa_symbol_nodebug(x);
}
EXPORT_SYMBOL(__phys_addr_symbol);
