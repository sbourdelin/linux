#include <linux/mm.h>

#include <asm/memory.h>

unsigned long __virt_to_phys(unsigned long x)
{
	phys_addr_t __x = (phys_addr_t)x;

	if (__x & BIT(VA_BITS - 1)) {
		/*
		 * The linear kernel range starts in the middle of the virtual
		 * adddress space. Testing the top bit for the start of the
		 * region is a sufficient check.
		 */
		return (__x & ~PAGE_OFFSET) + PHYS_OFFSET;
	} else {
		VIRTUAL_BUG_ON(x < kimage_vaddr || x >= (unsigned long)_end);
		return (__x - kimage_voffset);
	}
}
EXPORT_SYMBOL(__virt_to_phys);

unsigned long __phys_addr_symbol(unsigned long x)
{
	phys_addr_t __x = (phys_addr_t)x;

	/*
	 * This is intentionally different than above to be a tighter check
	 * for symbols.
	 */
	VIRTUAL_BUG_ON(x < kimage_vaddr + TEXT_OFFSET || x > (unsigned long) _end);
	return (__x - kimage_voffset);
}
EXPORT_SYMBOL(__phys_addr_symbol);
