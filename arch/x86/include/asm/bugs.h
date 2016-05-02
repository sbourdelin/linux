#ifndef _ASM_X86_BUGS_H
#define _ASM_X86_BUGS_H

extern void check_bugs(void);

#if defined(CONFIG_CPU_SUP_INTEL)
void check_mpx_erratum(struct cpuinfo_x86 *c);
#else
static inline void check_mpx_erratum(struct cpuinfo_x86 *c) {}
#if defined(CONFIG_X86_32)
int ppro_with_ram_bug(void);
#else
static inline int ppro_with_ram_bug(void) { return 0; }
#endif /* CONFIG_X86_32 */
#endif /* CONFIG_CPU_SUP_INTEL */

#endif /* _ASM_X86_BUGS_H */
