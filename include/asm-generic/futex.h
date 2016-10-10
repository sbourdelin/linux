#ifndef _ASM_GENERIC_FUTEX_H
#define _ASM_GENERIC_FUTEX_H

#include <linux/futex.h>
#include <linux/uaccess.h>
#include <asm/errno.h>

#ifndef CONFIG_SMP

/*
 * The following implementations are for uniprocessor machines.
 * They rely on preempt_disable() to ensure mutual exclusion.
 */

#ifndef __futex_atomic_op_inuser
#define __futex_atomic_op_inuser(op, oldval, uaddr, oparg)	\
({								\
	int __ret;						\
	u32 tmp;						\
								\
	preempt_disable();					\
	pagefault_disable();					\
								\
	__ret = -EFAULT;					\
	if (unlikely(get_user(oldval, uaddr) != 0))		\
		goto out_pagefault_enable;			\
								\
	__ret = 0;						\
	tmp = oldval;						\
								\
	switch (op) {						\
	case FUTEX_OP_SET:					\
		tmp = oparg;					\
		break;						\
	case FUTEX_OP_ADD:					\
		tmp += oparg;					\
		break;						\
	case FUTEX_OP_OR:					\
		tmp |= oparg;					\
		break;						\
	case FUTEX_OP_ANDN:					\
		tmp &= ~oparg;					\
		break;						\
	case FUTEX_OP_XOR:					\
		tmp ^= oparg;					\
		break;						\
	default:						\
		__ret = -ENOSYS;				\
	}							\
								\
	if (__ret == 0 && unlikely(put_user(tmp, uaddr) != 0))	\
		__ret = -EFAULT;				\
								\
out_pagefault_enable:						\
	pagefault_enable();					\
	preempt_enable();					\
								\
	__ret;							\
})
#endif

#ifndef __futex_atomic_cmpxchg_inatomic
#define __futex_atomic_cmpxchg_inatomic(uval, uaddr, oldval, newval)	\
({									\
	int __ret = 0;							\
	u32 tmp;							\
									\
	preempt_disable();						\
	if (unlikely(get_user(tmp, uaddr) != 0))			\
		__ret = -EFAULT;					\
									\
	if (__ret == 0 && tmp == oldval &&				\
			unlikely(put_user(newval, uaddr) != 0))		\
		__ret = -EFAULT;					\
									\
	*uval = tmp;							\
	preempt_enable();						\
									\
	__ret;								\
})
#endif

#else

/*
 * For multiprocessor machines, these macro should be overloaded with
 * implementations based on arch-specific atomic instructions to ensure proper
 * mutual exclusion
 */
#ifndef __futex_atomic_op_inuser
#define __futex_atomic_op_inuser(op, oldval, uaddr, oparg)	\
({								\
	int __ret;						\
	switch (op) {						\
	case FUTEX_OP_SET:					\
	case FUTEX_OP_ADD:					\
	case FUTEX_OP_OR:					\
	case FUTEX_OP_ANDN:					\
	case FUTEX_OP_XOR:					\
	default:						\
		__ret = -ENOSYS;				\
	}							\
	__ret;							\
})
#endif

#ifndef __futex_atomic_cmpxchg_inatomic
#define __futex_atomic_cmpxchg_inatomic(uval, uaddr, oldval, newval)	\
({									\
	int __ret = -ENOSYS;						\
	__ret;								\
})
#endif

#endif

/**
 * futex_atomic_op_inuser() - Atomic arithmetic operation with constant
 *			  argument and comparison of the previous
 *			  futex value with another constant.
 *
 * @encoded_op:	encoded operation to execute
 * @uaddr:	pointer to user space address
 *
 * Return:
 * 0 - On success
 * <0 - On error
 */
static inline int
futex_atomic_op_inuser(int encoded_op, u32 __user *uaddr)
{
	int op = (encoded_op >> 28) & 7;
	int cmp = (encoded_op >> 24) & 15;
	int oparg = (encoded_op << 8) >> 20;
	int cmparg = (encoded_op << 20) >> 20;
	int oldval = 0, ret;

	if (encoded_op & (FUTEX_OP_OPARG_SHIFT << 28))
		oparg = 1 << oparg;

	if (!access_ok(VERIFY_WRITE, uaddr, sizeof(u32)))
		return -EFAULT;

	ret = __futex_atomic_op_inuser(op, oldval, uaddr, oparg);

	if (ret == 0) {
		switch (cmp) {
		case FUTEX_OP_CMP_EQ: ret = (oldval == cmparg); break;
		case FUTEX_OP_CMP_NE: ret = (oldval != cmparg); break;
		case FUTEX_OP_CMP_LT: ret = (oldval < cmparg); break;
		case FUTEX_OP_CMP_GE: ret = (oldval >= cmparg); break;
		case FUTEX_OP_CMP_LE: ret = (oldval <= cmparg); break;
		case FUTEX_OP_CMP_GT: ret = (oldval > cmparg); break;
		default: ret = -ENOSYS;
		}
	}
	return ret;
}

/**
 * futex_atomic_cmpxchg_inatomic() - Compare and exchange the content of the
 *				uaddr with newval if the current value is
 *				oldval.
 * @uval:	pointer to store content of @uaddr
 * @uaddr:	pointer to user space address
 * @oldval:	old value
 * @newval:	new value to store to @uaddr
 *
 * Return:
 * 0 - On success
 * <0 - On error
 */
static inline int
futex_atomic_cmpxchg_inatomic(u32 *uval, u32 __user *uaddr,
		u32 oldval, u32 newval)
{
	if (!access_ok(VERIFY_WRITE, uaddr, sizeof(u32)))
		return -EFAULT;

	return __futex_atomic_cmpxchg_inatomic(uval, uaddr, oldval, newval);
}

#endif
