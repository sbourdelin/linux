#ifndef _LINUX_PERF_REGS_H
#define _LINUX_PERF_REGS_H

struct perf_regs {
	__u64		abi;
	struct pt_regs	*regs;
	struct perf_arch_regs	*arch_regs;
	u64		arch_regs_mask;
};

#ifndef perf_arch_regs
struct perf_arch_regs { };
#endif

#ifdef CONFIG_HAVE_PERF_REGS
#include <asm/perf_regs.h>
u64 perf_reg_value(struct pt_regs *regs, int idx);
int perf_reg_validate(u64 mask);
u64 perf_reg_abi(struct task_struct *task);
void perf_get_regs_user(struct perf_regs *regs_user,
			struct pt_regs *regs,
			struct pt_regs *regs_user_copy);

u64 perf_get_arch_regs_mask(void);
struct perf_arch_regs *perf_get_arch_reg(void);
u64 perf_arch_reg_value(struct perf_arch_regs *regs, int idx);

#else
static inline u64 perf_reg_value(struct pt_regs *regs, int idx)
{
	return 0;
}

static inline int perf_reg_validate(u64 mask)
{
	return mask ? -ENOSYS : 0;
}

static inline u64 perf_reg_abi(struct task_struct *task)
{
	return PERF_SAMPLE_REGS_ABI_NONE;
}

static inline void perf_get_regs_user(struct perf_regs *regs_user,
				      struct pt_regs *regs,
				      struct pt_regs *regs_user_copy)
{
	regs_user->regs = task_pt_regs(current);
	regs_user->abi = perf_reg_abi(current);
}

u64 perf_get_arch_regs_mask(void)
{
	return 0;
}

struct perf_arch_regs *perf_get_arch_reg(void)
{
	return 0;
}

u64 perf_arch_reg_value(struct perf_arch_regs *regs, int idx)
{
	return 0;
}
#endif /* CONFIG_HAVE_PERF_REGS */
#endif /* _LINUX_PERF_REGS_H */
