/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_JUMP_FUNCTION_H
#define _LINUX_JUMP_FUNCTION_H


//// This all should be in arch/x86/include/asm

typedef long dynfunc_t;

struct dynfunc_struct;

#define arch_dynfunc_trampoline(name, def)	\
	asm volatile (				\
	".globl dynfunc_" #name "; \n\t"	\
	"dynfunc_" #name ": \n\t"		\
	"jmp " #def " \n\t"			\
	".balign 8 \n \t"			\
	: : : "memory" )

int arch_assign_dynamic_function(const struct dynfunc_struct *dynfunc, void *func);

//////////////// The below should be in include/linux

#ifndef PARAMS
#define PARAMS(x...) x
#endif

#ifndef ARGS
#define ARGS(x...) x
#endif

struct dynfunc_struct {
	const void		*dynfunc;
	void			*func;
};

int assign_dynamic_function(const struct dynfunc_struct *dynfunc, void *func);

/*
 * DECLARE_DYNAMIC_FUNCTION - Declaration to create a dynamic function call
 * @name: The name of the function call to create
 * @proto: The proto-type of the function (up to 4 args)
 * @args: The arguments used by @proto
 *
 * This macro creates the function that can by used to create a dynamic
 * function call later. It also creates the function to modify what is
 * called:
 *
 *   dynfunc_[name](args);
 *
 * This is placed in the code where the dynamic function should be called
 * from.
 *
 *   assign_dynamic_function_[name](func);
 *
 * This is used to make the dynfunc_[name]() call a different function.
 * It will then call (func) instead.
 *
 * This must be added in a header for users of the above two functions.
 */
#define DECLARE_DYNAMIC_FUNCTION(name, proto, args)			\
	extern struct dynfunc_struct ___dyn_func__##name;		\
	static inline int assign_dynamic_function_##name(int(*func)(proto)) { \
		return assign_dynamic_function(&___dyn_func__##name, func); \
	}								\
	extern int dynfunc_##name(proto)

/*
 * DEFINE_DYNAMIC_FUNCTION - Define the dynamic function and default
 * @name: The name of the function call to create
 * @def: The default function to call
 * @proto: The proto-type of the function (up to 4 args)
 *
 * Must be placed in a C file.
 *
 * This sets up the dynamic function that other places may call
 * dynfunc_[name]().
 *
 * It defines the default function that the dynamic function will start
 * out calling at boot up.
 */
#define DEFINE_DYNAMIC_FUNCTION(name, def, proto)			\
	static void __used __dyn_func_trampoline_##name(void)		\
	{								\
		arch_dynfunc_trampoline(name, def);			\
		unreachable();						\
	}								\
	struct dynfunc_struct ___dyn_func__##name __used = {		\
		.dynfunc	= (void *)dynfunc_##name,		\
		.func		= def,					\
	}

#endif	/*  _LINUX_JUMP_FUNCTION_H */
