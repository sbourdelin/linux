#ifndef _ASM_NMI_H
#define _ASM_NMI_H

#ifdef CONFIG_HARDLOCKUP_DETECTOR
void arch_touch_nmi_watchdog(void);
#else
static inline void arch_touch_nmi_watchdog(void)
{
}
#endif

#endif /* _ASM_NMI_H */
