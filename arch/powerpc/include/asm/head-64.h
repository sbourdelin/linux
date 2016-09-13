#ifndef _ASM_POWERPC_HEAD_64_H
#define _ASM_POWERPC_HEAD_64_H

#include <asm/cache.h>

/*
 * We can't do CPP stringification and concatination directly into the section
 * name for some reason, so these macros can do it for us.
 */
.macro define_ftsec name
	.section ".head.text.\name\()","ax",@progbits
.endm
.macro define_data_ftsec name
	.section ".head.data.\name\()","a",@progbits
.endm
.macro use_ftsec name
	.section ".head.text.\name\()"
.endm

#define OPEN_FIXED_SECTION(sname, start, end)			\
	sname##_start = (start);				\
	sname##_end = (end);					\
	sname##_len = (end) - (start);				\
	define_ftsec sname;					\
	. = 0x0;						\
start_##sname:

#define OPEN_TEXT_SECTION(start)				\
	text_start = (start);					\
	.section ".text","ax",@progbits;			\
	. = 0x0;						\
start_text:

#define ZERO_FIXED_SECTION(sname, start, end)			\
	sname##_start = (start);				\
	sname##_end = (end);					\
	sname##_len = (end) - (start);				\
	define_data_ftsec sname;				\
	. = 0x0;						\
	. = sname##_len;

#define USE_FIXED_SECTION(sname)				\
	fs_label = start_##sname;				\
	fs_start = sname##_start;				\
	use_ftsec sname;

#define USE_TEXT_SECTION()					\
	fs_label = start_text;					\
	fs_start = text_start;					\
	.text

#define UNUSE_FIXED_SECTION(sname)				\
	.previous;

#define CLOSE_FIXED_SECTION(sname)				\
	USE_FIXED_SECTION(sname);				\
	. = sname##_len;					\
end_##sname:


#define __FIXED_SECTION_ENTRY_BEGIN(sname, name, __align)	\
	USE_FIXED_SECTION(sname);				\
	.align __align;						\
	.global name;						\
name:

#define FIXED_SECTION_ENTRY_BEGIN(sname, name)			\
	__FIXED_SECTION_ENTRY_BEGIN(sname, name, 0)

#define FIXED_SECTION_ENTRY_S_BEGIN(sname, name, start)		\
	USE_FIXED_SECTION(sname);				\
	name##_start = (start);					\
	.if (start) < sname##_start;				\
	.error "Fixed section underflow";			\
	.abort;							\
	.endif;							\
	. = (start) - sname##_start;				\
	.global name;						\
name:

#define FIXED_SECTION_ENTRY_END(sname, name)			\
	UNUSE_FIXED_SECTION(sname);

#define FIXED_SECTION_ENTRY_E_END(sname, name, end)		\
	.if (end) > sname##_end;				\
	.error "Fixed section overflow";			\
	.abort;							\
	.endif;							\
	.if (. - name > end - name##_start);			\
	.error "Fixed entry overflow";				\
	.abort;							\
	.endif;							\
	. = ((end) - sname##_start);				\
	UNUSE_FIXED_SECTION(sname);

#define FIXED_SECTION_ENTRY_S(sname, name, start, entry)	\
	FIXED_SECTION_ENTRY_S_BEGIN(sname, name, start);	\
	entry;							\
	FIXED_SECTION_ENTRY_END(sname, name);			\

#define FIXED_SECTION_ENTRY(sname, name, start, end, entry)	\
	FIXED_SECTION_ENTRY_S_BEGIN(sname, name, start);	\
	entry;							\
	FIXED_SECTION_ENTRY_E_END(sname, name, end);


/*
 * These macros are used to change symbols in other fixed sections to be
 * absolute or related to our current fixed section.
 *
 * GAS makes things as painful as it possibly can.
 */
/* ABS_ADDR: absolute address of a label within same section */
#define ABS_ADDR(label) (label - fs_label + fs_start)

/* FIXED_SECTION_ABS_ADDR: absolute address of a label in another setcion */
#define FIXED_SECTION_ABS_ADDR(sname, target)				\
	(target - start_##sname + sname##_start)

/* FIXED_SECTION_REL_ADDR: relative address of a label in another setcion */
#define FIXED_SECTION_REL_ADDR(sname, target)				\
	(FIXED_SECTION_ABS_ADDR(sname, target) + fs_label - fs_start)


#define VECTOR_HANDLER_REAL_BEGIN(name, start, end)			\
	FIXED_SECTION_ENTRY_S_BEGIN(real_vectors, exc_##start##_##name, start)

#define VECTOR_HANDLER_REAL_END(name, start, end)			\
	FIXED_SECTION_ENTRY_E_END(real_vectors, exc_##start##_##name, end)

#define VECTOR_HANDLER_VIRT_BEGIN(name, start, end)			\
	FIXED_SECTION_ENTRY_S_BEGIN(virt_vectors, exc_##start##_##name, start)

#define VECTOR_HANDLER_VIRT_END(name, start, end)			\
	FIXED_SECTION_ENTRY_E_END(virt_vectors, exc_##start##_##name, end)

#define COMMON_HANDLER_BEGIN(name)					\
	USE_TEXT_SECTION();						\
	.align	7;							\
	.global name;							\
name:

#define COMMON_HANDLER_END(name)					\
	.previous

#define TRAMP_HANDLER_BEGIN(name)					\
	FIXED_SECTION_ENTRY_BEGIN(real_trampolines, name)

#define TRAMP_HANDLER_END(name)						\
	FIXED_SECTION_ENTRY_END(real_trampolines, name)

#define VTRAMP_HANDLER_BEGIN(name)					\
	FIXED_SECTION_ENTRY_BEGIN(virt_trampolines, name)

#define VTRAMP_HANDLER_END(name)					\
	FIXED_SECTION_ENTRY_END(virt_trampolines, name)

#ifdef CONFIG_KVM_BOOK3S_64_HANDLER
#define TRAMP_KVM_BEGIN(name)						\
	TRAMP_HANDLER_BEGIN(name)

#define TRAMP_KVM_END(name)						\
	TRAMP_HANDLER_END(name)
#else
#define TRAMP_KVM_BEGIN(name)
#define TRAMP_KVM_END(name)
#endif

#define VECTOR_HANDLER_REAL_NONE(start, end)				\
	FIXED_SECTION_ENTRY_S_BEGIN(real_vectors, exc_##start##_##unused, start); \
	FIXED_SECTION_ENTRY_E_END(real_vectors, exc_##start##_##unused, end)

#define VECTOR_HANDLER_VIRT_NONE(start, end)				\
	FIXED_SECTION_ENTRY_S_BEGIN(virt_vectors, exc_##start##_##unused, start); \
	FIXED_SECTION_ENTRY_E_END(virt_vectors, exc_##start##_##unused, end);


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

#define VECTOR_HANDLER_REAL_OOL(name, start, end)			\
	__VECTOR_HANDLER_REAL_OOL(name, start, end);			\
	__TRAMP_HANDLER_REAL_OOL(name, start);

#define __VECTOR_HANDLER_REAL_OOL_MASKABLE(name, start, end)		\
	__VECTOR_HANDLER_REAL_OOL(name, start, end);

#define __TRAMP_HANDLER_REAL_OOL_MASKABLE(name, vec)			\
	TRAMP_HANDLER_BEGIN(tramp_real_##name);				\
	MASKABLE_EXCEPTION_PSERIES_OOL(vec, name##_common);		\
	TRAMP_HANDLER_END(tramp_real_##name);

#define VECTOR_HANDLER_REAL_OOL_MASKABLE(name, start, end)		\
	__VECTOR_HANDLER_REAL_OOL_MASKABLE(name, start, end);		\
	__TRAMP_HANDLER_REAL_OOL_MASKABLE(name, start);

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

#define VECTOR_HANDLER_REAL_OOL_HV(name, start, end)			\
	__VECTOR_HANDLER_REAL_OOL_HV(name, start, end);			\
	__TRAMP_HANDLER_REAL_OOL_HV(name, start);

#define __VECTOR_HANDLER_REAL_OOL_MASKABLE_HV(name, start, end)		\
	__VECTOR_HANDLER_REAL_OOL(name, start, end);

#define __TRAMP_HANDLER_REAL_OOL_MASKABLE_HV(name, vec)			\
	TRAMP_HANDLER_BEGIN(tramp_real_##name);				\
	MASKABLE_EXCEPTION_HV_OOL(vec, name##_common);			\
	TRAMP_HANDLER_END(tramp_real_##name);

#define VECTOR_HANDLER_REAL_OOL_MASKABLE_HV(name, start, end)		\
	__VECTOR_HANDLER_REAL_OOL_MASKABLE_HV(name, start, end);	\
	__TRAMP_HANDLER_REAL_OOL_MASKABLE_HV(name, start);

#define __VECTOR_HANDLER_VIRT_OOL(name, start, end)			\
	VECTOR_HANDLER_VIRT_BEGIN(name, start, end);			\
	__OOL_EXCEPTION(start, label, tramp_virt_##name);		\
	VECTOR_HANDLER_VIRT_END(name, start, end);

#define __TRAMP_HANDLER_VIRT_OOL(name, realvec)				\
	VTRAMP_HANDLER_BEGIN(tramp_virt_##name);			\
	STD_RELON_EXCEPTION_PSERIES_OOL(realvec, name##_common);	\
	VTRAMP_HANDLER_END(tramp_virt_##name);

#define VECTOR_HANDLER_VIRT_OOL(name, start, end, realvec)		\
	__VECTOR_HANDLER_VIRT_OOL(name, start, end);			\
	__TRAMP_HANDLER_VIRT_OOL(name, realvec);

#define __VECTOR_HANDLER_VIRT_OOL_MASKABLE(name, start, end)		\
	__VECTOR_HANDLER_VIRT_OOL(name, start, end);

#define __TRAMP_HANDLER_VIRT_OOL_MASKABLE(name, realvec)		\
	VTRAMP_HANDLER_BEGIN(tramp_virt_##name);			\
	MASKABLE_RELON_EXCEPTION_PSERIES_OOL(realvec, name##_common);	\
	VTRAMP_HANDLER_END(tramp_virt_##name);

#define VECTOR_HANDLER_VIRT_OOL_MASKABLE(name, start, end, realvec)	\
	__VECTOR_HANDLER_VIRT_OOL_MASKABLE(name, start, end);		\
	__TRAMP_HANDLER_VIRT_OOL_MASKABLE(name, realvec);

#define __VECTOR_HANDLER_VIRT_OOL_HV(name, start, end)			\
	__VECTOR_HANDLER_VIRT_OOL(name, start, end);

#define __TRAMP_HANDLER_VIRT_OOL_HV(name, realvec)			\
	VTRAMP_HANDLER_BEGIN(tramp_virt_##name);			\
	STD_RELON_EXCEPTION_HV_OOL(realvec, name##_common);		\
	VTRAMP_HANDLER_END(tramp_virt_##name)

#define VECTOR_HANDLER_VIRT_OOL_HV(name, start, end, realvec)		\
	__VECTOR_HANDLER_VIRT_OOL_HV(name, start, end);			\
	__TRAMP_HANDLER_VIRT_OOL_HV(name, realvec);

#define __VECTOR_HANDLER_VIRT_OOL_MASKABLE_HV(name, start, end)		\
	__VECTOR_HANDLER_VIRT_OOL(name, start, end);

#define __TRAMP_HANDLER_VIRT_OOL_MASKABLE_HV(name, realvec)		\
	VTRAMP_HANDLER_BEGIN(tramp_virt_##name);			\
	MASKABLE_RELON_EXCEPTION_HV_OOL(realvec, name##_common);	\
	VTRAMP_HANDLER_END(tramp_virt_##name);

#define VECTOR_HANDLER_VIRT_OOL_MASKABLE_HV(name, start, end, realvec)	\
	__VECTOR_HANDLER_VIRT_OOL_MASKABLE_HV(name, start, end);	\
	__TRAMP_HANDLER_VIRT_OOL_MASKABLE_HV(name, realvec);

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
