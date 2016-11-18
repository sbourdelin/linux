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
		/*
		 * __virt_to_phys should not be used on symbol addresses.
		 * This should be changed to a BUG once all basic bad uses have
		 * been cleaned up.
		 */
		WARN(1, "Do not use virt_to_phys on symbol addresses");
		return __phys_addr_symbol(x);
	}
}
EXPORT_SYMBOL(__virt_to_phys);

unsigned long __phys_addr_symbol(unsigned long x)
{
	phys_addr_t __x = (phys_addr_t)x;

	/*
	 * This is bounds checking against the kernel image only.
	 * __pa_symbol should only be used on kernel symbol addresses.
	 */
	VIRTUAL_BUG_ON(x < (unsigned long) KERNEL_START || x > (unsigned long) KERNEL_END);
	return (__x - kimage_voffset);
}
EXPORT_SYMBOL(__phys_addr_symbol);
