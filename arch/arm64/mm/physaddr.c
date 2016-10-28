#include <linux/mm.h>

#include <asm/memory.h>

unsigned long __virt_to_phys(unsigned long x)
{
	phys_addr_t __x = (phys_addr_t)x;

	if (__x & BIT(VA_BITS - 1)) {
		/* The bit check ensures this is the right range */
		return (__x & ~PAGE_OFFSET) + PHYS_OFFSET;
	} else {
		VIRTUAL_BUG_ON(x < kimage_vaddr || x > (unsigned long)_end);
		return (__x - kimage_voffset);
	}
}
EXPORT_SYMBOL(__virt_to_phys);
