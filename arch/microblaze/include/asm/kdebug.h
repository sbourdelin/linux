#include <asm-generic/kdebug.h>

static inline void arch_breakpoint(void)
{
	__asm__ __volatile__("brki r16, 0x18;");
}
