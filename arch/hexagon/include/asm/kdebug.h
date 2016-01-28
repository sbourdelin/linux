
static inline void arch_breakpoint(void)
{
	asm("trap0(#0xDB)");
}
