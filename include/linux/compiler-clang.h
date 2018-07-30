/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LINUX_COMPILER_TYPES_H
#error "Please don't include <linux/compiler-clang.h> directly, include <linux/compiler.h> instead."
#endif

/* Some compiler specific definitions are overwritten here
 * for Clang compiler
 */

#define CLANG_VERSION (__clang_major__ * 10000	\
		       + __clang_minor__ * 100	\
		       + __clang_patchlevel__)

#ifdef uninitialized_var
#undef uninitialized_var
#define uninitialized_var(x) x = *(&(x))
#endif

/* same as gcc, this was present in clang-2.6 so we can assume it works
 * with any version that can compile the kernel
 */
#define __UNIQUE_ID(prefix) __PASTE(__PASTE(__UNIQUE_ID_, prefix), __COUNTER__)

/* all clang versions usable with the kernel support KASAN ABI version 5 */
#define KASAN_ABI_VERSION 5

/* emulate gcc's __SANITIZE_ADDRESS__ flag */
#if __has_feature(address_sanitizer)
#define __SANITIZE_ADDRESS__
#endif

#undef __no_sanitize_address
#define __no_sanitize_address __attribute__((no_sanitize("address")))

/* Clang doesn't have a way to turn it off per-function, yet. */
#ifdef __noretpoline
#undef __noretpoline
#endif

/*
 * Not all versions of clang implement the the type-generic versions
 * of the builtin overflow checkers. Fortunately, clang implements
 * __has_builtin allowing us to avoid awkward version
 * checks. Unfortunately, we don't know which version of gcc clang
 * pretends to be, so the macro may or may not be defined.
 */
#undef COMPILER_HAS_GENERIC_BUILTIN_OVERFLOW
#if __has_builtin(__builtin_mul_overflow) && \
    __has_builtin(__builtin_add_overflow) && \
    __has_builtin(__builtin_sub_overflow)
#define COMPILER_HAS_GENERIC_BUILTIN_OVERFLOW 1
#endif

#define __diag_str1(s) #s
#define __diag_str(s) __diag_str1(s)
#define __diag(s) _Pragma(__diag_str(clang diagnostic s))
#define __diag_CLANG_ignore ignored
#define __diag_CLANG_warn warning
#define __diag_CLANG_error error
#define __diag_CLANG(version, severity, s) \
	__diag_CLANG_ ## version(__diag_CLANG_ ## severity s)

#if CLANG_VERSION >= 70000
#define __diag_CLANG_7(s) __diag(s)
#else
#define __diag_CLANG_7(s)
#endif
