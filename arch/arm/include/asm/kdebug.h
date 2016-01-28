
static inline void arch_breakpoint(void)
{
	asm(__inst_arm(0xe7ffdeff));
}
