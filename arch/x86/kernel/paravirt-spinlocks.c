/*
 * Split spinlock implementation out into its own file, so it can be
 * compiled in a FTRACE-compatible way.
 */
#include <linux/spinlock.h>
#include <linux/export.h>
#include <linux/jump_label.h>

#include <asm/paravirt.h>

__visible void __native_queued_spin_unlock(struct qspinlock *lock)
{
	native_queued_spin_unlock(lock);
}
PV_CALLEE_SAVE_REGS_THUNK(__native_queued_spin_unlock);

bool pv_is_native_spin_unlock(void)
{
	return pv_lock_ops.queued_spin_unlock.func ==
		__raw_callee_save___native_queued_spin_unlock;
}

__visible bool __native_virt_spin_lock(struct qspinlock *lock)
{
	return native_virt_spin_lock(lock);
}
PV_CALLEE_SAVE_REGS_THUNK(__native_virt_spin_lock);

struct pv_lock_ops pv_lock_ops = {
#ifdef CONFIG_SMP
	.queued_spin_lock_slowpath = native_queued_spin_lock_slowpath,
	.queued_spin_unlock = PV_CALLEE_SAVE(__native_queued_spin_unlock),
	.wait = paravirt_nop,
	.kick = paravirt_nop,
	.vcpu_is_preempted = __PV_IS_CALLEE_SAVE(_paravirt_false),
	.virt_spin_lock = PV_CALLEE_SAVE(__native_virt_spin_lock),
#endif /* SMP */
};
EXPORT_SYMBOL(pv_lock_ops);

void __init native_pv_lock_init(void)
{
	if (!static_cpu_has(X86_FEATURE_HYPERVISOR))
		pv_lock_ops.virt_spin_lock =
			__PV_IS_CALLEE_SAVE(_paravirt_false);
}
