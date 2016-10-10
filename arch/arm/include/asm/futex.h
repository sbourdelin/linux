#ifndef _ASM_ARM_FUTEX_H
#define _ASM_ARM_FUTEX_H

#ifdef __KERNEL__

#include <linux/futex.h>
#include <linux/uaccess.h>
#include <asm/errno.h>

#define __futex_atomic_ex_table(err_reg)			\
	"3:\n"							\
	"	.pushsection __ex_table,\"a\"\n"		\
	"	.align	3\n"					\
	"	.long	1b, 4f, 2b, 4f\n"			\
	"	.popsection\n"					\
	"	.pushsection .text.fixup,\"ax\"\n"		\
	"	.align	2\n"					\
	"4:	mov	%0, " err_reg "\n"			\
	"	b	3b\n"					\
	"	.popsection"

#ifdef CONFIG_SMP

#define __futex_atomic_op(insn, ret, oldval, tmp, uaddr, oparg)	\
({								\
	unsigned int __ua_flags;				\
	smp_mb();						\
	prefetchw(uaddr);					\
	__ua_flags = uaccess_save_and_enable();			\
	__asm__ __volatile__(					\
	"1:	ldrex	%1, [%3]\n"				\
	"	" insn "\n"					\
	"2:	strex	%2, %0, [%3]\n"				\
	"	teq	%2, #0\n"				\
	"	bne	1b\n"					\
	"	mov	%0, #0\n"				\
	__futex_atomic_ex_table("%5")				\
	: "=&r" (ret), "=&r" (oldval), "=&r" (tmp)		\
	: "r" (uaddr), "r" (oparg), "Ir" (-EFAULT)		\
	: "cc", "memory");					\
	uaccess_restore(__ua_flags);				\
	smp_mb();						\
})

#define __futex_atomic_cmpxchg_op(ret, val, uaddr, oldval, newval)		\
({										\
	unsigned int __ua_flags;						\
	smp_mb();								\
	prefetchw(uaddr);							\
	__ua_flags = uaccess_save_and_enable();					\
	__asm__ __volatile__(							\
	"1:	ldrex	%1, [%4]\n"						\
	"	teq	%1, %2\n"						\
	"	ite	eq	@ explicit IT needed for the 2b label\n"	\
	"2:	strexeq	%0, %3, [%4]\n"						\
	"	movne	%0, #0\n"						\
	"	teq	%0, #0\n"						\
	"	bne	1b\n"							\
	__futex_atomic_ex_table("%5")						\
	: "=&r" (ret), "=&r" (val)						\
	: "r" (oldval), "r" (newval), "r" (uaddr), "Ir" (-EFAULT)		\
	: "cc", "memory");							\
	uaccess_restore(__ua_flags);						\
	smp_mb();								\
})

#else /* !SMP, we can work around lack of atomic ops by disabling preemption */

#include <linux/preempt.h>
#include <asm/domain.h>

#define __futex_atomic_op(insn, ret, oldval, tmp, uaddr, oparg)	\
({								\
	unsigned int __ua_flags;				\
	preempt_disable();					\
	__ua_flags = uaccess_save_and_enable();			\
	__asm__ __volatile__(					\
	"1:	" TUSER(ldr) "	%1, [%3]\n"			\
	"	" insn "\n"					\
	"2:	" TUSER(str) "	%0, [%3]\n"			\
	"	mov	%0, #0\n"				\
	__futex_atomic_ex_table("%5")				\
	: "=&r" (ret), "=&r" (oldval), "=&r" (tmp)		\
	: "r" (uaddr), "r" (oparg), "Ir" (-EFAULT)		\
	: "cc", "memory");					\
	uaccess_restore(__ua_flags);				\
	preempt_disable();					\
})

#define __futex_atomic_cmpxchg_op(ret, val, uaddr, oldval, newval)		\
({										\
	unsigned int __ua_flags;						\
	preempt_disable();							\
	__ua_flags = uaccess_save_and_enable();					\
	__asm__ __volatile__(							\
	"@futex_atomic_cmpxchg_inatomic\n"					\
	"1:	" TUSER(ldr) "	%1, [%4]\n"					\
	"	teq	%1, %2\n"						\
	"	it	eq	@ explicit IT needed for the 2b label\n"	\
	"2:	" TUSER(streq) "	%3, [%4]\n"				\
	__futex_atomic_ex_table("%5")						\
	: "+r" (ret), "=&r" (val)						\
	: "r" (oldval), "r" (newval), "r" (uaddr), "Ir" (-EFAULT)		\
	: "cc", "memory");							\
	uaccess_restore(__ua_flags);						\
	preempt_enable();							\
})

#endif /* !SMP */

#define __futex_atomic_op_inuser(op, oldval, uaddr, oparg)		\
({									\
	int __ret, tmp;							\
	pagefault_disable();						\
	switch (op) {							\
	case FUTEX_OP_SET:						\
		__futex_atomic_op("mov	%0, %4",			\
				__ret, oldval, tmp, uaddr, oparg);	\
		break;							\
	case FUTEX_OP_ADD:						\
		__futex_atomic_op("add	%0, %1, %4",			\
				__ret, oldval, tmp, uaddr, oparg);	\
		break;							\
	case FUTEX_OP_OR:						\
		__futex_atomic_op("orr	%0, %1, %4",			\
				__ret, oldval, tmp, uaddr, oparg);	\
		break;							\
	case FUTEX_OP_ANDN:						\
		__futex_atomic_op("and	%0, %1, %4",			\
				__ret, oldval, tmp, uaddr, ~oparg);	\
		break;							\
	case FUTEX_OP_XOR:						\
		__futex_atomic_op("eor	%0, %1, %4",			\
				__ret, oldval, tmp, uaddr, oparg);	\
		break;							\
	default:							\
		ret = -ENOSYS;						\
	}								\
	pagefault_enable();						\
	__ret;								\
})

#define __futex_atomic_cmpxchg_inatomic(uval, uaddr, oldval, newval)	\
({									\
	int __ret;							\
	u32 val;							\
	__futex_atomic_cmpxchg_op(__ret, val, uaddr, oldval, newval);	\
	*uval = val;							\
	__ret;								\
})

#include <asm-generic/futex.h>

#endif /* __KERNEL__ */
#endif /* _ASM_ARM_FUTEX_H */
