#ifndef __LINUX_COMPILER_ATTRIBUTES_H
#define __LINUX_COMPILER_ATTRIBUTES_H

/* This file is meant to be sorted. */

/*
 * Required attributes: your compiler must support these.
 */
#define __alias(symbol)		__attribute__((alias(#symbol)))
#define __aligned(x)		__attribute__((aligned(x)))
#define __aligned_largest	__attribute__((aligned))
#define __always_inline         inline __attribute__((always_inline))
#define __always_unused		__attribute__((unused))
#define __attribute_const__     __attribute__((const))
#define __cold			__attribute__((cold))
#define __gnu_inline            __attribute__((gnu_inline))
#define __malloc		__attribute__((malloc))
#define __maybe_unused		__attribute__((unused))
#define __mode(x)		__attribute__((mode(x)))
#define   noinline              __attribute__((noinline))
#define __noreturn		__attribute__((noreturn))
#define __packed		__attribute__((packed))
#define __printf(a, b)		__attribute__((format(printf, a, b)))
#define __pure			__attribute__((pure))
#define __scanf(a, b)		__attribute__((format(scanf, a, b)))
#define __section(S)		__attribute__((section(#S)))
#define __used			__attribute__((used))
#define __weak			__attribute__((weak))

/*
 * Optional attributes: your compiler may or may not support them.
 *
 * To check for them, we use __has_attribute, which is supported on gcc >= 5,
 * clang >= 2.9 and icc >= 17. In the meantime, to support 4.6 <= gcc < 5,
 * we implement it by hand.
 */
#ifndef __has_attribute
#define __has_attribute(x) __GCC46_has_attribute_##x
#define __GCC46_has_attribute_assume_aligned 0
#define __GCC46_has_attribute_designated_init 0
#define __GCC46_has_attribute_externally_visible 1
#define __GCC46_has_attribute_noclone 1
#define __GCC46_has_attribute_optimize 1
#define __GCC46_has_attribute_no_sanitize_address 0
#endif

/*
 * __assume_aligned(n, k): Tell the optimizer that the returned
 * pointer can be assumed to be k modulo n. The second argument is
 * optional (default 0), so we use a variadic macro to make the
 * shorthand.
 *
 * Beware: Do not apply this to functions which may return
 * ERR_PTRs. Also, it is probably unwise to apply it to functions
 * returning extra information in the low bits (but in that case the
 * compiler should see some alignment anyway, when the return value is
 * massaged by 'flags = ptr & 3; ptr &= ~3;').
 */
#if __has_attribute(assume_aligned)
#define __assume_aligned(a, ...) __attribute__((assume_aligned(a, ## __VA_ARGS__)))
#else
#define __assume_aligned(a, ...)
#endif

/*
 * Mark structures as requiring designated initializers.
 * https://gcc.gnu.org/onlinedocs/gcc/Designated-Inits.html
 */
#if __has_attribute(designated_init)
#define __designated_init __attribute__((designated_init))
#else
#define __designated_init
#endif

/*
 * When used with Link Time Optimization, gcc can optimize away C functions or
 * variables which are referenced only from assembly code.  __visible tells the
 * optimizer that something else uses this function or variable, thus preventing
 * this.
 */
#if __has_attribute(externally_visible)
#define __visible __attribute__((externally_visible))
#else
#define __visible
#endif

/* Mark a function definition as prohibited from being cloned. */
#if __has_attribute(noclone) && __has_attribute(optimize)
#define __noclone __attribute__((noclone, optimize("no-tracer")))
#else
#define __noclone
#endif

/*
 * Tell the compiler that address safety instrumentation (KASAN)
 * should not be applied to that function.
 * Conflicts with inlining: https://gcc.gnu.org/bugzilla/show_bug.cgi?id=67368
 */
#if __has_attribute(no_sanitize_address)
#define __no_sanitize_address __attribute__((no_sanitize_address))
#else
#define __no_sanitize_address
#endif

#endif /* __LINUX_COMPILER_ATTRIBUTES_H */
