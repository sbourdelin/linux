/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_DYNAMIC_CALL_H
#define _LINUX_DYNAMIC_CALL_H

/*
 * Dynamic call (optpoline) support
 *
 * Dynamic calls use code patching and runtime learning to promote indirect
 * calls into direct calls using the static_call machinery.  They give the
 * flexibility of function pointers, but with improved performance.  This is
 * especially important for cases where retpolines would otherwise be used, as
 * retpolines can significantly impact performance.
 * The two callees learned to be most common will be made through static_calls,
 * while for any other callee the trampoline will fall back to an indirect call
 * (or a retpoline, if those are enabled).
 * Patching of newly learned callees into the fast-path relies on RCU to ensure
 * the fast-path is not in use on any CPU; thus the calls must be made under
 * the RCU read lock.
 *
 *
 * A dynamic call table must be defined in file scope with
 *	DYNAMIC_CALL_$NR(ret, name, type1, ..., type$NR);
 * where $NR is from 1 to 4, ret is the return type of the function and type1
 * through type$NR are the argument types.  Then, calls can be made through a
 * matching function pointer 'func' with
 *	x = dynamic_name(func, arg1, ..., arg$NR);
 * which will behave equivalently to
 *	(*func)(arg1, ..., arg$NR);
 * except hopefully with higher performance.  It is allowed for multiple
 * callsites to use the same dynamic call table, in which case they will share
 * statistics for learning.  This will perform well as long as the callsites
 * typically have the same set of common callees.
 *
 * Usage example:
 *
 *	struct foo {
 *		int x;
 *		int (*f)(int);
 *	}
 *	DYNAMIC_CALL_1(int, handle_foo, int);
 *
 *	int handle_foo(struct foo *f)
 *	{
 *		return dynamic_handle_foo(f->f, f->x);
 *	}
 *
 * This should behave the same as if the function body were changed to:
 *		return (f->f)(f->x);
 * but potentially with improved performance.
 */

#define DEFINE_DYNAMIC_CALL_1(_ret, _name, _type1)			       \
_ret dynamic_##_name(_ret (*func)(_type1), _type1 arg1);

#define DEFINE_DYNAMIC_CALL_2(_ret, _name, _type1, _name2, _type2)	       \
_ret dynamic_##_name(_ret (*func)(_type1, _type2), _type1 arg1, _type2 arg2);

#define DEFINE_DYNAMIC_CALL_3(_ret, _name, _type1, _type2, _type3)	       \
_ret dynamic_##_name(_ret (*func)(_type1, _type2, _type3), _type1 arg1,	       \
		     _type2 arg2, _type3 arg3);

#define DEFINE_DYNAMIC_CALL_4(_ret, _name, _type1, _type2, _type3, _type4)     \
_ret dynamic_##_name(_ret (*func)(_type1, _type2, _type3, _type4), _type1 arg1,\
		     _type2 arg2, _type3 arg3, _type4 arg4);

#ifdef CONFIG_DYNAMIC_CALLS

#include <linux/jump_label.h>
#include <linux/mutex.h>
#include <linux/percpu.h>
#include <linux/static_call.h>
#include <linux/string.h>
#include <linux/workqueue.h>

/* Number of callees from the slowpath to track on each CPU */
#define DYNAMIC_CALL_CANDIDATES	4
/*
 * Number of fast-path callees; to change this, much of the macrology below
 * must also be changed.
 */
#define DYNAMIC_CALL_BRANCHES	2
struct dynamic_call_candidate {
	void *func;
	unsigned long hit_count;
};
struct dynamic_call_percpu {
	struct dynamic_call_candidate candidates[DYNAMIC_CALL_CANDIDATES];
	unsigned long hit_count[DYNAMIC_CALL_BRANCHES];
	unsigned long miss_count;
};
struct dynamic_call {
	struct work_struct update_work;
	struct static_key_false *skip_stats;
	struct static_key_true *skip_fast;
	struct static_call_key *key[DYNAMIC_CALL_BRANCHES];
	struct __percpu dynamic_call_percpu *percpu;
	struct mutex update_lock;
};

void dynamic_call_update(struct work_struct *work);


#define __DYNAMIC_CALL_BITS(_ret, _name, ...)				       \
static _ret dummy_##_name(__VA_ARGS__)					       \
{									       \
	BUG();								       \
}									       \
DEFINE_STATIC_KEY_TRUE(_name##_skip_fast);				       \
DEFINE_STATIC_KEY_FALSE(_name##_skip_stats);				       \
DEFINE_STATIC_CALL(dynamic_##_name##_1, dummy_##_name);			       \
DEFINE_STATIC_CALL(dynamic_##_name##_2, dummy_##_name);			       \
DEFINE_PER_CPU(struct dynamic_call_percpu, _name##_dc_pc);		       \
									       \
static struct dynamic_call _name##_dc = {				       \
	.update_work = __WORK_INITIALIZER(_name##_dc.update_work,	       \
					  dynamic_call_update),		       \
	.skip_stats = &_name##_skip_stats,				       \
	.skip_fast = &_name##_skip_fast,				       \
	.key = {&dynamic_##_name##_1, &dynamic_##_name##_2},		       \
	.percpu = &_name##_dc_pc,					       \
	.update_lock = __MUTEX_INITIALIZER(_name##_dc.update_lock),	       \
};

#define __DYNAMIC_CALL_STATS(_name)					       \
	if (static_branch_unlikely(&_name##_skip_stats))		       \
		goto skip_stats;					       \
	for (i = 0; i < DYNAMIC_CALL_CANDIDATES; i++)			       \
		if (func == thiscpu->candidates[i].func) {		       \
			thiscpu->candidates[i].hit_count++;		       \
			break;						       \
		}							       \
	if (i == DYNAMIC_CALL_CANDIDATES) /* no match */		       \
		for (i = 0; i < DYNAMIC_CALL_CANDIDATES; i++)		       \
			if (!thiscpu->candidates[i].func) {		       \
				thiscpu->candidates[i].func = func;	       \
				thiscpu->candidates[i].hit_count = 1;	       \
				break;					       \
			}						       \
	if (i == DYNAMIC_CALL_CANDIDATES) /* no space */		       \
		thiscpu->miss_count++;					       \
									       \
	for (i = 0; i < DYNAMIC_CALL_CANDIDATES; i++)			       \
		total_count += thiscpu->candidates[i].hit_count;	       \
	if (total_count > 1000) /* Arbitrary threshold */		       \
		schedule_work(&_name##_dc.update_work);			       \
	else if (thiscpu->miss_count > 1000) {				       \
		/* Many misses, few hits: let's roll the dice again for a      \
		 * fresh set of candidates.				       \
		 */							       \
		memset(thiscpu->candidates, 0, sizeof(thiscpu->candidates));   \
		thiscpu->miss_count = 0;				       \
	}								       \
skip_stats:


#define DYNAMIC_CALL_1(_ret, _name, _type1)				       \
__DYNAMIC_CALL_BITS(_ret, _name, _type1 arg1)				       \
									       \
_ret dynamic_##_name(_ret (*func)(_type1), _type1 arg1)			       \
{									       \
	struct dynamic_call_percpu *thiscpu = this_cpu_ptr(_name##_dc.percpu); \
	unsigned long total_count = 0;					       \
	int i;								       \
									       \
	WARN_ON_ONCE(!rcu_read_lock_held());					       \
	if (static_branch_unlikely(&_name##_skip_fast))			       \
		goto skip_fast;						       \
	if (func == dynamic_##_name##_1.func) {				       \
		thiscpu->hit_count[0]++;				       \
		return static_call(dynamic_##_name##_1, arg1);		       \
	}								       \
	if (func == dynamic_##_name##_2.func) {				       \
		thiscpu->hit_count[1]++;				       \
		return static_call(dynamic_##_name##_2, arg1);		       \
	}								       \
									       \
skip_fast:								       \
	__DYNAMIC_CALL_STATS(_name)					       \
	return func(arg1);						       \
}

#define DYNAMIC_CALL_2(_ret, _name, _type1, _type2)			       \
__DYNAMIC_CALL_BITS(_ret, _name, _type1 arg1, _type2 arg2)		       \
									       \
_ret dynamic_##_name(_ret (*func)(_type1, _type2), _type1 arg1,	_type2 arg2)   \
{									       \
	struct dynamic_call_percpu *thiscpu = this_cpu_ptr(_name##_dc.percpu); \
	unsigned long total_count = 0;					       \
	int i;								       \
									       \
	WARN_ON_ONCE(!rcu_read_lock_held());					       \
	if (static_branch_unlikely(&_name##_skip_fast))			       \
		goto skip_fast;						       \
	if (func == dynamic_##_name##_1.func) {				       \
		thiscpu->hit_count[0]++;				       \
		return static_call(dynamic_##_name##_1, arg1, arg2);	       \
	}								       \
	if (func == dynamic_##_name##_2.func) {				       \
		thiscpu->hit_count[1]++;				       \
		return static_call(dynamic_##_name##_2, arg1, arg2);	       \
	}								       \
									       \
skip_fast:								       \
	__DYNAMIC_CALL_STATS(_name)					       \
	return func(arg1, arg2);					       \
}

#define DYNAMIC_CALL_3(_ret, _name, _type1, _type2, _type3)		       \
__DYNAMIC_CALL_BITS(_ret, _name, _type1 arg1, _type2 arg2, _type3 arg3)        \
									       \
_ret dynamic_##_name(_ret (*func)(_type1, _type2, _type3), _type1 arg1,	       \
		     _type2 arg2, _type3 arg3)				       \
{									       \
	struct dynamic_call_percpu *thiscpu = this_cpu_ptr(_name##_dc.percpu); \
	unsigned long total_count = 0;					       \
	int i;								       \
									       \
	WARN_ON_ONCE(!rcu_read_lock_held());					       \
	if (static_branch_unlikely(&_name##_skip_fast))			       \
		goto skip_fast;						       \
	if (func == dynamic_##_name##_1.func) {				       \
		thiscpu->hit_count[0]++;				       \
		return static_call(dynamic_##_name##_1, arg1, arg2, arg3);  \
	}								       \
	if (func == dynamic_##_name##_2.func) {				       \
		thiscpu->hit_count[1]++;				       \
		return static_call(dynamic_##_name##_2, arg1, arg2, arg3);  \
	}								       \
									       \
skip_fast:								       \
	__DYNAMIC_CALL_STATS(_name)					       \
	return func(arg1, arg2, arg3);					       \
}

#define DYNAMIC_CALL_4(_ret, _name, _type1, _type2, _type3, _type4)	       \
__DYNAMIC_CALL_BITS(_ret, _name, _type1 arg1, _type2 arg2, _type3 arg3,	       \
		    _type4 arg4)					       \
									       \
_ret dynamic_##_name(_ret (*func)(_type1, _type2, _type3, _type4), _type1 arg1,\
		     _type2 arg2, _type3 arg3, _type4 arg4)		       \
{									       \
	struct dynamic_call_percpu *thiscpu = this_cpu_ptr(_name##_dc.percpu); \
	unsigned long total_count = 0;					       \
	int i;								       \
									       \
	WARN_ON_ONCE(!rcu_read_lock_held());					       \
	if (static_branch_unlikely(&_name##_skip_fast))			       \
		goto skip_fast;						       \
	if (func == dynamic_##_name##_1.func) {				       \
		thiscpu->hit_count[0]++;				       \
		return static_call(dynamic_##_name##_1, arg1, arg2, arg3, arg4);\
	}								       \
	if (func == dynamic_##_name##_2.func) {				       \
		thiscpu->hit_count[1]++;				       \
		return static_call(dynamic_##_name##_2, arg1, arg2, arg3, arg4);\
	}								       \
									       \
skip_fast:								       \
	__DYNAMIC_CALL_STATS(_name)					       \
	return func(arg1, arg2, arg3, arg4);				       \
}

#else /* !CONFIG_DYNAMIC_CALLS */

/* Implement as simple indirect calls */

#define DYNAMIC_CALL_1(_ret, _name, _type1)				       \
_ret dynamic_##_name(_ret (*func)(_type1), _type1 arg1)			       \
{									       \
	WARN_ON_ONCE(!rcu_read_lock_held());					       \
	return func(arg1);						       \
}									       \

#define DYNAMIC_CALL_2(_ret, _name, _type1, _name2, _type2)		       \
_ret dynamic_##_name(_ret (*func)(_type1, _type2), _type1 arg1, _type2 arg2)   \
{									       \
	WARN_ON_ONCE(!rcu_read_lock_held());					       \
	return func(arg1, arg2);					       \
}									       \

#define DYNAMIC_CALL_3(_ret, _name, _type1, _type2, _type3)		       \
_ret dynamic_##_name(_ret (*func)(_type1, _type2, _type3), _type1 arg1,	       \
		     _type2 arg2, _type3 arg3)				       \
{									       \
	WARN_ON_ONCE(!rcu_read_lock_held());					       \
	return func(arg1, arg2, arg3);					       \
}									       \

#define DYNAMIC_CALL_4(_ret, _name, _type1, _type2, _type3, _type4)	       \
_ret dynamic_##_name(_ret (*func)(_type1, _type2, _type3, _type4), _type1 arg1,\
		     _type2 arg2, _type3 arg3, _type4 arg4)		       \
{									       \
	WARN_ON_ONCE(!rcu_read_lock_held());					       \
	return func(arg1, arg2, arg3, arg4);				       \
}									       \


#endif /* CONFIG_DYNAMIC_CALLS */

#endif /* _LINUX_DYNAMIC_CALL_H */
