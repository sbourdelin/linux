/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_FSGSBASE_H
#define _ASM_FSGSBASE_H

#ifndef __ASSEMBLY__

#ifdef CONFIG_X86_64

#include <asm/msr-index.h>

/*
 * Read/write a task's FSBASE or GSBASE. This returns the value that
 * the FS/GS base would have (if the task were to be resumed). These
 * work on the current task or on a non-running (typically stopped
 * ptrace child) task.
 */
extern unsigned long x86_fsbase_read_task(struct task_struct *task);
extern unsigned long x86_gsbase_read_task(struct task_struct *task);
extern void x86_fsbase_write_task(struct task_struct *task, unsigned long fsbase);
extern void x86_gsbase_write_task(struct task_struct *task, unsigned long gsbase);

/* Must be protected by X86_FEATURE_FSGSBASE check. */

static __always_inline unsigned long rdfsbase(void)
{
	unsigned long fsbase;

	asm volatile("rdfsbase %0" : "=r" (fsbase) :: "memory");

	return fsbase;
}

static __always_inline unsigned long rdgsbase(void)
{
	unsigned long gsbase;

	asm volatile("rdgsbase %0" : "=r" (gsbase) :: "memory");

	return gsbase;
}

static __always_inline void wrfsbase(unsigned long fsbase)
{
	asm volatile("wrfsbase %0" :: "r" (fsbase) : "memory");
}

static __always_inline void wrgsbase(unsigned long gsbase)
{
	asm volatile("wrgsbase %0" :: "r" (gsbase) : "memory");
}

#include <asm/cpufeature.h>

/* Helper functions for reading/writing FS/GS base */

static inline unsigned long x86_fsbase_read_cpu(void)
{
	unsigned long fsbase;

	if (static_cpu_has(X86_FEATURE_FSGSBASE))
		fsbase = rdfsbase();
	else
		rdmsrl(MSR_FS_BASE, fsbase);

	return fsbase;
}

static inline void x86_fsbase_write_cpu(unsigned long fsbase)
{
	if (static_cpu_has(X86_FEATURE_FSGSBASE))
		wrfsbase(fsbase);
	else
		wrmsrl(MSR_FS_BASE, fsbase);
}

extern unsigned long x86_gsbase_read_cpu_inactive(void);
extern void x86_gsbase_write_cpu_inactive(unsigned long gsbase);

#endif /* CONFIG_X86_64 */

#else /* __ASSEMBLY__ */

#ifdef CONFIG_X86_64

#include <asm/inst.h>

#if CONFIG_SMP

/*
 * CPU/node NR is loaded from the limit (size) field of a special segment
 * descriptor entry in GDT.
 */
.macro LOAD_CPU_AND_NODE_SEG_LIMIT reg:req
	movq	$__CPUNODE_SEG, \reg
	lsl	\reg, \reg
.endm

/*
 * Fetch the per-CPU GSBASE value for this processor and put it in @reg.
 * We normally use %gs for accessing per-CPU data, but we are setting up
 * %gs here and obviously can not use %gs itself to access per-CPU data.
 */
.macro FIND_PERCPU_BASE reg:req
	/*
	 * The CPU/node NR is initialized earlier, directly in cpu_init().
	 * The CPU NR is extracted from it.
	 */
	ALTERNATIVE \
		"LOAD_CPU_AND_NODE_SEG_LIMIT \reg", \
		"RDPID	\reg", \
		X86_FEATURE_RDPID
	andq	$VDSO_CPUNODE_MASK, \reg
	movq	__per_cpu_offset(, \reg, 8), \reg
.endm

#else

.macro FIND_PERCPU_BASE reg:req
	/* Tracking the base offset value */
	movq	pcpu_unit_offsets(%rip), \reg
.endm

#endif /* CONFIG_SMP */

#endif /* CONFIG_X86_64 */

#endif /* __ASSEMBLY__ */

#endif /* _ASM_FSGSBASE_H */
