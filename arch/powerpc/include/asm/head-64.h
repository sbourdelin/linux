#ifndef _ASM_POWERPC_HEAD_64_H
#define _ASM_POWERPC_HEAD_64_H

#include <asm/cache.h>

#define VECTOR_HANDLER_REAL_BEGIN(name, start, end)			\
	. = start ;							\
	.global exc_##start##_##name ;					\
exc_##start##_##name:

#define VECTOR_HANDLER_REAL_END(name, start, end)

#define VECTOR_HANDLER_VIRT_BEGIN(name, start, end)			\
	. = start ;							\
	.global exc_##start##_##name ;					\
exc_##start##_##name:

#define VECTOR_HANDLER_VIRT_END(name, start, end)

#define COMMON_HANDLER_BEGIN(name)					\
	.align	7;							\
	.global name;							\
name:

#define COMMON_HANDLER_END(name)

#define TRAMP_HANDLER_BEGIN(name)					\
	.global name ;							\
name:

#define TRAMP_HANDLER_END(name)

#ifdef CONFIG_KVM_BOOK3S_64_HANDLER
#define TRAMP_KVM_BEGIN(name)						\
	TRAMP_HANDLER_BEGIN(name)

#define TRAMP_KVM_END(name)						\
	TRAMP_HANDLER_END(name)
#else
#define TRAMP_KVM_BEGIN(name)
#define TRAMP_KVM_END(name)
#endif

#define VECTOR_HANDLER_REAL_NONE(start, end)

#define VECTOR_HANDLER_VIRT_NONE(start, end)


#define VECTOR_HANDLER_REAL(name, start, end)				\
	VECTOR_HANDLER_REAL_BEGIN(name, start, end);			\
	STD_EXCEPTION_PSERIES(start, name##_common);			\
	VECTOR_HANDLER_REAL_END(name, start, end);

#define VECTOR_HANDLER_VIRT(name, start, end, realvec)			\
	VECTOR_HANDLER_VIRT_BEGIN(name, start, end);			\
	STD_RELON_EXCEPTION_PSERIES(start, realvec, name##_common);	\
	VECTOR_HANDLER_VIRT_END(name, start, end);

#define VECTOR_HANDLER_REAL_MASKABLE(name, start, end)			\
	VECTOR_HANDLER_REAL_BEGIN(name, start, end);			\
	MASKABLE_EXCEPTION_PSERIES(start, start, name##_common);	\
	VECTOR_HANDLER_REAL_END(name, start, end);

#define VECTOR_HANDLER_VIRT_MASKABLE(name, start, end, realvec)		\
	VECTOR_HANDLER_VIRT_BEGIN(name, start, end);			\
	MASKABLE_RELON_EXCEPTION_PSERIES(start, realvec, name##_common); \
	VECTOR_HANDLER_VIRT_END(name, start, end);

#define VECTOR_HANDLER_REAL_HV(name, start, end)			\
	VECTOR_HANDLER_REAL_BEGIN(name, start, end);			\
	STD_EXCEPTION_HV(start, start + 0x2, name##_common);		\
	VECTOR_HANDLER_REAL_END(name, start, end);

#define VECTOR_HANDLER_VIRT_HV(name, start, end, realvec)		\
	VECTOR_HANDLER_VIRT_BEGIN(name, start, end);			\
	STD_RELON_EXCEPTION_HV(start, realvec + 0x2, name##_common);	\
	VECTOR_HANDLER_VIRT_END(name, start, end);

#define __VECTOR_HANDLER_REAL_OOL(name, start, end)			\
	VECTOR_HANDLER_REAL_BEGIN(name, start, end);			\
	__OOL_EXCEPTION(start, label, tramp_real_##name);		\
	VECTOR_HANDLER_REAL_END(name, start, end);

#define __TRAMP_HANDLER_REAL_OOL(name, vec)				\
	TRAMP_HANDLER_BEGIN(tramp_real_##name);				\
	STD_EXCEPTION_PSERIES_OOL(vec, name##_common);			\
	TRAMP_HANDLER_END(tramp_real_##name);

#define __VECTOR_HANDLER_REAL_OOL_MASKABLE(name, start, end)		\
	__VECTOR_HANDLER_REAL_OOL(name, start, end);

#define __TRAMP_HANDLER_REAL_OOL_MASKABLE(name, vec)			\
	TRAMP_HANDLER_BEGIN(tramp_real_##name);				\
	MASKABLE_EXCEPTION_PSERIES_OOL(vec, name##_common);		\
	TRAMP_HANDLER_END(tramp_real_##name);

#define __VECTOR_HANDLER_REAL_OOL_HV_DIRECT(name, start, end, handler)	\
	VECTOR_HANDLER_REAL_BEGIN(name, start, end);			\
	__OOL_EXCEPTION(start, label, handler);				\
	VECTOR_HANDLER_REAL_END(name, start, end);

#define __VECTOR_HANDLER_REAL_OOL_HV(name, start, end)			\
	__VECTOR_HANDLER_REAL_OOL(name, start, end);

#define __TRAMP_HANDLER_REAL_OOL_HV(name, vec)				\
	TRAMP_HANDLER_BEGIN(tramp_real_##name);				\
	STD_EXCEPTION_HV_OOL(vec + 0x2, name##_common);			\
	TRAMP_HANDLER_END(tramp_real_##name);

#define __VECTOR_HANDLER_REAL_OOL_MASKABLE_HV(name, start, end)		\
	__VECTOR_HANDLER_REAL_OOL(name, start, end);

#define __TRAMP_HANDLER_REAL_OOL_MASKABLE_HV(name, vec)			\
	TRAMP_HANDLER_BEGIN(tramp_real_##name);				\
	MASKABLE_EXCEPTION_HV_OOL(vec, name##_common);			\
	TRAMP_HANDLER_END(tramp_real_##name);

#define __VECTOR_HANDLER_VIRT_OOL(name, start, end)			\
	VECTOR_HANDLER_VIRT_BEGIN(name, start, end);			\
	__OOL_EXCEPTION(start, label, tramp_virt_##name);		\
	VECTOR_HANDLER_VIRT_END(name, start, end);

#define __TRAMP_HANDLER_VIRT_OOL(name, realvec)				\
	TRAMP_HANDLER_BEGIN(tramp_virt_##name);				\
	STD_RELON_EXCEPTION_PSERIES_OOL(realvec, name##_common);	\
	TRAMP_HANDLER_END(tramp_virt_##name);

#define __VECTOR_HANDLER_VIRT_OOL_MASKABLE(name, start, end)		\
	__VECTOR_HANDLER_VIRT_OOL(name, start, end);

#define __TRAMP_HANDLER_VIRT_OOL_MASKABLE(name, realvec)		\
	TRAMP_HANDLER_BEGIN(tramp_virt_##name);				\
	MASKABLE_RELON_EXCEPTION_PSERIES_OOL(realvec, name##_common);	\
	TRAMP_HANDLER_END(tramp_virt_##name);

#define __VECTOR_HANDLER_VIRT_OOL_HV(name, start, end)			\
	__VECTOR_HANDLER_VIRT_OOL(name, start, end);

#define __TRAMP_HANDLER_VIRT_OOL_HV(name, realvec)			\
	TRAMP_HANDLER_BEGIN(tramp_virt_##name);				\
	STD_RELON_EXCEPTION_HV_OOL(realvec, name##_common);		\
	TRAMP_HANDLER_END(tramp_virt_##name);

#define __VECTOR_HANDLER_VIRT_OOL_MASKABLE_HV(name, start, end)		\
	__VECTOR_HANDLER_VIRT_OOL(name, start, end);

#define __TRAMP_HANDLER_VIRT_OOL_MASKABLE_HV(name, realvec)		\
	TRAMP_HANDLER_BEGIN(tramp_virt_##name);				\
	MASKABLE_RELON_EXCEPTION_HV_OOL(realvec, name##_common);	\
	TRAMP_HANDLER_END(tramp_virt_##name);

#define TRAMP_KVM(area, n)						\
	TRAMP_KVM_BEGIN(do_kvm_##n);					\
	KVM_HANDLER(area, EXC_STD, n);					\
	TRAMP_KVM_END(do_kvm_##n)

#define TRAMP_KVM_SKIP(area, n)						\
	TRAMP_KVM_BEGIN(do_kvm_##n);					\
	KVM_HANDLER_SKIP(area, EXC_STD, n);				\
	TRAMP_KVM_END(do_kvm_##n)

#define TRAMP_KVM_HV(area, n)						\
	TRAMP_KVM_BEGIN(do_kvm_H##n);					\
	KVM_HANDLER(area, EXC_HV, n + 0x2);				\
	TRAMP_KVM_END(do_kvm_H##n)

#define TRAMP_KVM_HV_SKIP(area, n)					\
	TRAMP_KVM_BEGIN(do_kvm_H##n);					\
	KVM_HANDLER_SKIP(area, EXC_HV, n + 0x2);			\
	TRAMP_KVM_END(do_kvm_H##n)

#define COMMON_HANDLER(name, realvec, hdlr)				\
	COMMON_HANDLER_BEGIN(name);					\
	STD_EXCEPTION_COMMON(realvec, name, hdlr);			\
	COMMON_HANDLER_END(name);

#define COMMON_HANDLER_ASYNC(name, realvec, hdlr)			\
	COMMON_HANDLER_BEGIN(name);					\
	STD_EXCEPTION_COMMON_ASYNC(realvec, name, hdlr);		\
	COMMON_HANDLER_END(name);

#define COMMON_HANDLER_HV(name, realvec, hdlr)				\
	COMMON_HANDLER_BEGIN(name);					\
	STD_EXCEPTION_COMMON(realvec + 0x2, name, hdlr);		\
	COMMON_HANDLER_END(name);

#endif	/* _ASM_POWERPC_HEAD_64_H */
