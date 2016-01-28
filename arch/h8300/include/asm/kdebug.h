
static inline void arch_breakpoint(void)
{
	__asm__ __volatile__("trapa #2");
}
