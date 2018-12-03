/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_INDIRECT_CALL_WRAPPER_H
#define _LINUX_INDIRECT_CALL_WRAPPER_H

#ifdef CONFIG_RETPOLINE

/*
 * INDIRECT_CALL_$NR - wrapper for indirect calls with $NR known builtin
 *  @f: function pointer
 *  @name: base name for builtin functions, see INDIRECT_CALLABLE_DECLARE_$NR
 *  @__VA_ARGS__: arguments for @f
 *
 * Avoid retpoline overhead for known builtin, checking @f vs each of them and
 * eventually invoking directly the builtin function. Fallback to the indirect
 * call
 */
#define INDIRECT_CALL_1(f, name, ...)					\
	({								\
		likely(f == name ## 1) ? name ## 1(__VA_ARGS__) :	\
					 f(__VA_ARGS__);		\
	})
#define INDIRECT_CALL_2(f, name, ...)					\
	({								\
		likely(f == name ## 2) ? name ## 2(__VA_ARGS__) :	\
				 INDIRECT_CALL_1(f, name, __VA_ARGS__);	\
	})

/*
 * INDIRECT_CALLABLE_DECLARE_$NR - declare $NR known builtin for
 * INDIRECT_CALL_$NR usage
 *  @type: return type for the builtin function
 *  @name: base name for builtin functions, the full list is generated appending
 *	   the numbers in the 1..@NR range
 *  @__VA_ARGS__: arguments type list for the builtin function
 *
 * Builtin with higher $NR will be checked first by INDIRECT_CALL_$NR
 */
#define INDIRECT_CALLABLE_DECLARE_1(type, name, ...)			\
	extern type name ## 1(__VA_ARGS__)
#define INDIRECT_CALLABLE_DECLARE_2(type, name, ...)			\
	extern type name ## 2(__VA_ARGS__);				\
	INDIRECT_CALLABLE_DECLARE_1(type, name, __VA_ARGS__)

/*
 * INDIRECT_CALLABLE - allow usage of a builtin function from INDIRECT_CALL_$NR
 *  @f: builtin function name
 *  @nr: id associated with this builtin, higher values will be checked first by
 *	 INDIRECT_CALL_$NR
 *  @type: function return type
 *  @name: base name used by INDIRECT_CALL_ to access the builtin list
 *  @__VA_ARGS__: arguments type list for @f
 */
#define INDIRECT_CALLABLE(f, nr, type, name, ...)		\
	__alias(f) type name ## nr(__VA_ARGS__)

#else
#define INDIRECT_CALL_1(f, name, ...) f(__VA_ARGS__)
#define INDIRECT_CALL_2(f, name, ...) f(__VA_ARGS__)
#define INDIRECT_CALLABLE_DECLARE_1(type, name, ...)
#define INDIRECT_CALLABLE_DECLARE_2(type, name, ...)
#define INDIRECT_CALLABLE(f, nr, type, name, ...)
#endif

/*
 * We can use INDIRECT_CALL_$NR for ipv6 related functions only if ipv6 is
 * builtin, this macro simplify dealing with indirect calls with only ipv4/ipv6
 * alternatives
 */
#if IS_BUILTIN(CONFIG_IPV6)
#define INDIRECT_CALL_INET INDIRECT_CALL_2
#elif IS_ENABLED(CONFIG_INET)
#define INDIRECT_CALL_INET INDIRECT_CALL_1
#else
#define INDIRECT_CALL_INET(...)
#endif

#endif
