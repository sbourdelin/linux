
static inline void arch_breakpoint(void)
{
	asm("EXCPT 2;");
}
