#ifndef _ASM_ARM_XEN_EVENTS_H
#define _ASM_ARM_XEN_EVENTS_H

#include <asm/ptrace.h>
#include <asm/atomic.h>

enum ipi_vector {
	XEN_PLACEHOLDER_VECTOR,

	/* Xen IPIs go here */
	XEN_NR_IPIS,
};

static inline int xen_irqs_disabled(struct pt_regs *regs)
{
	return raw_irqs_disabled_flags(regs->ARM_cpsr);
}

#ifdef CONFIG_GENERIC_ATOMIC64
/* if CONFIG_GENERIC_ATOMIC64 is defined we cannot use the generic
 * atomic64_xchg function because it is implemented using spin locks.
 * Here we need proper atomic instructions to read and write memory
 * shared with the hypervisor.
 */
static inline u64 xen_atomic64_xchg(atomic64_t *ptr, u64 new)
{
	u64 result;
	unsigned long tmp;

	smp_mb();

	__asm__ __volatile__("@ xen_atomic64_xchg\n"
"1:	ldrexd	%0, %H0, [%3]\n"
"	strexd	%1, %4, %H4, [%3]\n"
"	teq	%1, #0\n"
"	bne	1b"
	: "=&r" (result), "=&r" (tmp), "+Qo" (ptr->counter)
	: "r" (&ptr->counter), "r" (new)
	: "cc");

	smp_mb();

	return result;
}
#else
#define xen_atomic64_xchg atomic64_xchg
#endif

#define xchg_xen_ulong(ptr, val) xen_atomic64_xchg(container_of((ptr),	\
							    atomic64_t,	\
							    counter), (val))

/* Rebind event channel is supported by default */
static inline bool xen_support_evtchn_rebind(void)
{
	return true;
}

#endif /* _ASM_ARM_XEN_EVENTS_H */
