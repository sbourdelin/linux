#ifndef _ASM_X86_IRQ_VECTORS_H
#define _ASM_X86_IRQ_VECTORS_H

#include <linux/threads.h>
/*
 * Linux IRQ vector layout.
 *
 * There are 256 IDT entries (per CPU - each entry is 8 bytes) which can
 * be defined by Linux. They are used as a jump table by the CPU when a
 * given vector is triggered - by a CPU-external, CPU-internal or
 * software-triggered event.
 *
 * Linux sets the kernel code address each entry jumps to early during
 * bootup, and never changes them. This is the general layout of the
 * IDT entries:
 *
 *  Vectors   0 ...  31 : system traps and exceptions - hardcoded events
 *  Vectors  32 ... 127 : device interrupts
 *  Vector  128         : legacy int80 syscall interface
 *  Vectors 129 ... 238 : device interrupts
 *  Vectors 239(0xef)   : special(system) interrupt LOCAL_TIMER_VECTOR
 *  Vectors 240 ... 255 : special(system) interrupts, see definition below for details.
 *
 * 64-bit x86 has per CPU IDT tables, 32-bit has one shared IDT table.
 *
 * This file enumerates the exact layout of them:
 */

#define NMI_VECTOR			0x02
#define MCE_VECTOR			0x12

/*
 * IDT vectors usable for external interrupt sources start at 0x20.
 * (0x80 is the syscall vector, 0x30-0x3f are for ISA)
 */
#define FIRST_EXTERNAL_VECTOR		0x20
/*
 * We start allocating at 0x21 to spread out vectors evenly between
 * priority levels. (0x80 is the syscall vector)
 */
#define VECTOR_OFFSET_START		1

/*
 * Reserve the lowest usable vector (and hence lowest priority)  0x20 for
 * triggering cleanup after irq migration. 0x21-0x2f will still be used
 * for device interrupts.
 */
#define IRQ_MOVE_CLEANUP_VECTOR		FIRST_EXTERNAL_VECTOR

#define IA32_SYSCALL_VECTOR		0x80

/*
 * Vectors 0x30-0x3f are used for ISA interrupts.
 *   round up to the next 16-vector boundary
 */
#define ISA_IRQ_VECTOR(irq)		(((FIRST_EXTERNAL_VECTOR + 16) & ~15) + irq)

/*
 * Special IRQ vectors: 0xef - 0xff, for system vectors.
 *
 *  some of the following vectors are 'rare', they are merged
 *  into a single vector (CALL_FUNCTION_VECTOR) to save vector space.
 *  TLB, reschedule and local APIC vectors are performance-critical.
 *
 *  Layout:
 *  0xff, 0xfe:
 *	Two highest vectors, granted for spurious vector and error vector.
 *  0xfd - 0xf9:
 *	CONFIG_SMP dependent vectors. On morden machines these are achieved
 *	via local APIC, but not neccessary.
 *  0xf8 - 0xf0:
 *      Local APIC dependent vectors. Some are only depending on Local ACPI,
 *      but some are depending on more.
 *  0xef:
 *      Local APIC timer vector.
 */

/*
 * Grant highest 2 vectors for two special vectors:
 * Spurious Vector and Error Vector.
 */
#define SPURIOUS_APIC_VECTOR		0xff
#define ERROR_APIC_VECTOR		0xfe

#if SPURIOUS_APIC_VECTOR != 0xff
# error SPURIOUS_APIC_VECTOR definition error, should grant it: 0xff
#endif

#if ERROR_APIC_VECTOR  != 0xfe
# error ERROR_APIC_VECTOR definition error, should grant it: 0xfe
#endif


/*
 * SMP dependent vectors
 */
/* CPU-to-CPU reschedule-helper IPI, driven by wakeup.*/
#define RESCHEDULE_VECTOR		0xfd

/* IPI for generic function call */
#define CALL_FUNCTION_VECTOR		0xfc

/* IPI for generic single function call */
#define CALL_FUNCTION_SINGLE_VECTOR	0xfb

/* IPI used for rebooting/stopping */
#define REBOOT_VECTOR			0xfa

/* IPI for X86 platform specific use */
#define X86_PLATFORM_IPI_VECTOR		0xf9

/*
 * Local APCI dependent only vectors, these may or may not depend on SMP.
 */
/* IRQ work vector: a mechanism that allows running code in IRQ context */
#define IRQ_WORK_VECTOR			0xf8

/*
 * Local APCI dependent vectors, but also depend on other configurations
 * (MCE, virtualization, etc)
 */
#define THERMAL_APIC_VECTOR		0xf7
#define THRESHOLD_APIC_VECTOR		0xf6
#define UV_BAU_MESSAGE			0xf5
#define DEFERRED_ERROR_VECTOR		0xf4

/* Vector on which hypervisor callbacks will be delivered */
#define HYPERVISOR_CALLBACK_VECTOR	0xf3

/* Vector for KVM to deliver posted interrupt IPI */
#ifdef CONFIG_HAVE_KVM
#define POSTED_INTR_VECTOR		0xf2
#endif
#define POSTED_INTR_WAKEUP_VECTOR	0xf1

/* Vector 0xf0 is not used yet, reserved */

/*
 * Local APIC timer IRQ vector is on a different priority level,
 * to work around the 'lost local interrupt if more than 2 IRQ
 * sources per level' errata.
 */
#define LOCAL_TIMER_VECTOR		0xef

/* --- end of special vectors definitions ---  */


#define NR_VECTORS			 256

#ifdef CONFIG_X86_LOCAL_APIC
#define FIRST_SYSTEM_VECTOR		LOCAL_TIMER_VECTOR
#else
#define FIRST_SYSTEM_VECTOR		NR_VECTORS
#endif

#define FPU_IRQ				  13

/*
 * Size the maximum number of interrupts.
 *
 * If the irq_desc[] array has a sparse layout, we can size things
 * generously - it scales up linearly with the maximum number of CPUs,
 * and the maximum number of IO-APICs, whichever is higher.
 *
 * In other cases we size more conservatively, to not create too large
 * static arrays.
 */

#define NR_IRQS_LEGACY			16

#define CPU_VECTOR_LIMIT		(64 * NR_CPUS)
#define IO_APIC_VECTOR_LIMIT		(32 * MAX_IO_APICS)

#if defined(CONFIG_X86_IO_APIC) && defined(CONFIG_PCI_MSI)
#define NR_IRQS						\
	(CPU_VECTOR_LIMIT > IO_APIC_VECTOR_LIMIT ?	\
		(NR_VECTORS + CPU_VECTOR_LIMIT)  :	\
		(NR_VECTORS + IO_APIC_VECTOR_LIMIT))
#elif defined(CONFIG_X86_IO_APIC)
#define	NR_IRQS				(NR_VECTORS + IO_APIC_VECTOR_LIMIT)
#elif defined(CONFIG_PCI_MSI)
#define NR_IRQS				(NR_VECTORS + CPU_VECTOR_LIMIT)
#else
#define NR_IRQS				NR_IRQS_LEGACY
#endif

#endif /* _ASM_X86_IRQ_VECTORS_H */
