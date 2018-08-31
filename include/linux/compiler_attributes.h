#ifndef __LINUX_COMPILER_ATTRIBUTES_H
#define __LINUX_COMPILER_ATTRIBUTES_H

/*
 * This file is meant to be sorted (by actual attribute name,
 * not by #define identifier).
 *
 * Do not add here attributes which depend on others or require extra logic.
 *
 * If an attribute is optional, state the reason in the comment.
 */

/*
 * To check for optional attributes, we use __has_attribute, which is supported
 * on gcc >= 5, clang >= 2.9 and icc >= 17. In the meantime, to support
 * 4.6 <= gcc < 5, we implement __has_attribute by hand.
 */
#ifndef __has_attribute
#define __has_attribute(x) __GCC4_has_attribute_##x
#define __GCC4_has_attribute_externally_visible 1
#define __GCC4_has_attribute_noclone 1
#define __GCC4_has_attribute_optimize 1
#if __GNUC_MINOR__ >= 8
#define __GCC4_has_attribute_no_sanitize_address 1
#endif
#if __GNUC_MINOR__ >= 9
#define __GCC4_has_attribute_assume_aligned 1
#endif
#endif

/*
 *   gcc: https://gcc.gnu.org/onlinedocs/gcc/Common-Function-Attributes.html#index-alias-function-attribute
 */
#define __alias(symbol)         __attribute__((alias(#symbol)))

/*
 *   gcc: https://gcc.gnu.org/onlinedocs/gcc/Common-Function-Attributes.html#index-aligned-function-attribute
 *   gcc: https://gcc.gnu.org/onlinedocs/gcc/Common-Type-Attributes.html#index-aligned-type-attribute
 *   gcc: https://gcc.gnu.org/onlinedocs/gcc/Common-Variable-Attributes.html#index-aligned-variable-attribute
 */
#define __aligned(x)            __attribute__((aligned(x)))
#define __aligned_largest       __attribute__((aligned))

/*
 *   gcc: https://gcc.gnu.org/onlinedocs/gcc/Common-Function-Attributes.html#index-always_005finline-function-attribute
 * clang: mentioned but no proper documentation
 */
#define __always_inline         inline __attribute__((always_inline))

/*
 * The second argument is optional (default 0), so we use a variadic macro
 * to make the shorthand.
 *
 * Beware: Do not apply this to functions which may return
 * ERR_PTRs. Also, it is probably unwise to apply it to functions
 * returning extra information in the low bits (but in that case the
 * compiler should see some alignment anyway, when the return value is
 * massaged by 'flags = ptr & 3; ptr &= ~3;').
 *
 * Optional: only supported since gcc >= 4.9
 * Optional: not supported by icc
 *
 *   gcc: https://gcc.gnu.org/onlinedocs/gcc/Common-Function-Attributes.html#index-assume_005faligned-function-attribute
 * clang: https://clang.llvm.org/docs/AttributeReference.html#assume-aligned
 */
#if __has_attribute(assume_aligned)
#define __assume_aligned(a, ...) __attribute__((assume_aligned(a, ## __VA_ARGS__)))
#else
#define __assume_aligned(a, ...)
#endif

/*
 *   gcc: https://gcc.gnu.org/onlinedocs/gcc/Common-Function-Attributes.html#index-cold-function-attribute
 *   gcc: https://gcc.gnu.org/onlinedocs/gcc/Label-Attributes.html#index-cold-label-attribute
 */
#define __cold                  __attribute__((cold))

/*
 * Note the long name.
 *
 *   gcc: https://gcc.gnu.org/onlinedocs/gcc/Common-Function-Attributes.html#index-const-function-attribute
 */
#define __attribute_const__     __attribute__((const))

/*
 * Don't. Just don't. See commit 771c035372a0 ("deprecate the '__deprecated'
 * attribute warnings entirely and for good") for more information.
 *
 *   gcc: https://gcc.gnu.org/onlinedocs/gcc/Common-Function-Attributes.html#index-deprecated-function-attribute
 *   gcc: https://gcc.gnu.org/onlinedocs/gcc/Common-Type-Attributes.html#index-deprecated-type-attribute
 *   gcc: https://gcc.gnu.org/onlinedocs/gcc/Common-Variable-Attributes.html#index-deprecated-variable-attribute
 *   gcc: https://gcc.gnu.org/onlinedocs/gcc/Enumerator-Attributes.html#index-deprecated-enumerator-attribute
 * clang: https://clang.llvm.org/docs/AttributeReference.html#deprecated
 */
#define __deprecated

/*
 * Optional: only supported since gcc >= 5.1
 * Optional: not supported by clang
 * Optional: not supported by icc
 *
 *   gcc: https://gcc.gnu.org/onlinedocs/gcc/Common-Type-Attributes.html#index-designated_005finit-type-attribute
 */
#if __has_attribute(designated_init)
#define __designated_init       __attribute__((designated_init))
#else
#define __designated_init
#endif

/*
 * Optional: not supported by clang
 *
 *   gcc: https://gcc.gnu.org/onlinedocs/gcc/Common-Function-Attributes.html#index-externally_005fvisible-function-attribute
 */
#if __has_attribute(externally_visible)
#define __visible               __attribute__((externally_visible))
#else
#define __visible
#endif

/*
 *   gcc: https://gcc.gnu.org/onlinedocs/gcc/Common-Function-Attributes.html#index-format-function-attribute
 * clang: https://clang.llvm.org/docs/AttributeReference.html#format
 */
#define __printf(a, b)          __attribute__((format(printf, a, b)))
#define __scanf(a, b)           __attribute__((format(scanf, a, b)))

/*
 *   gcc: https://gcc.gnu.org/onlinedocs/gcc/Common-Function-Attributes.html#index-gnu_005finline-function-attribute
 * clang: https://clang.llvm.org/docs/AttributeReference.html#gnu-inline
 */
#define __gnu_inline            __attribute__((gnu_inline))

/*
 *   gcc: https://gcc.gnu.org/onlinedocs/gcc/Common-Function-Attributes.html#index-malloc-function-attribute
 */
#define __malloc                __attribute__((malloc))

/*
 *   gcc: https://gcc.gnu.org/onlinedocs/gcc/Common-Type-Attributes.html#index-mode-type-attribute
 *   gcc: https://gcc.gnu.org/onlinedocs/gcc/Common-Variable-Attributes.html#index-mode-variable-attribute
 */
#define __mode(x)               __attribute__((mode(x)))

/*
 * Optional: not supported by clang
 * Note: icc does not recognize gcc's no-tracer
 *
 *  gcc: https://gcc.gnu.org/onlinedocs/gcc/Common-Function-Attributes.html#index-noclone-function-attribute
 *  gcc: https://gcc.gnu.org/onlinedocs/gcc/Common-Function-Attributes.html#index-optimize-function-attribute
 */
#if __has_attribute(noclone) && __has_attribute(optimize)
#define __noclone               __attribute__((noclone, optimize("no-tracer")))
#else
#define __noclone
#endif

/*
 * Note the missing underscores.
 *
 *   gcc: https://gcc.gnu.org/onlinedocs/gcc/Common-Function-Attributes.html#index-noinline-function-attribute
 * clang: mentioned but no proper documentation
 */
#define   noinline              __attribute__((noinline))

/*
 *   gcc: https://gcc.gnu.org/onlinedocs/gcc/Common-Function-Attributes.html#index-noreturn-function-attribute
 * clang: https://clang.llvm.org/docs/AttributeReference.html#noreturn
 * clang: https://clang.llvm.org/docs/AttributeReference.html#id1
 */
#define __noreturn              __attribute__((noreturn))

/*
 * Optional: only supported since gcc >= 4.8
 * Optional: not supported by icc
 *
 *   gcc: https://gcc.gnu.org/onlinedocs/gcc/Common-Function-Attributes.html#index-no_005fsanitize_005faddress-function-attribute
 * clang: https://clang.llvm.org/docs/AttributeReference.html#no-sanitize-address-no-address-safety-analysis
 */
#if __has_attribute(no_sanitize_address)
#define __no_sanitize_address   __attribute__((no_sanitize_address))
#else
#define __no_sanitize_address
#endif

/*
 *   gcc: https://gcc.gnu.org/onlinedocs/gcc/Common-Type-Attributes.html#index-packed-type-attribute
 * clang: https://gcc.gnu.org/onlinedocs/gcc/Common-Variable-Attributes.html#index-packed-variable-attribute
 */
#define __packed                __attribute__((packed))

/*
 *   gcc: https://gcc.gnu.org/onlinedocs/gcc/Common-Function-Attributes.html#index-pure-function-attribute
 */
#define __pure                  __attribute__((pure))

/*
 *   gcc: https://gcc.gnu.org/onlinedocs/gcc/Common-Function-Attributes.html#index-section-function-attribute
 *   gcc: https://gcc.gnu.org/onlinedocs/gcc/Common-Variable-Attributes.html#index-section-variable-attribute
 * clang: https://clang.llvm.org/docs/AttributeReference.html#section-declspec-allocate
 */
#define __section(S)            __attribute__((section(#S)))

/*
 *   gcc: https://gcc.gnu.org/onlinedocs/gcc/Common-Function-Attributes.html#index-unused-function-attribute
 *   gcc: https://gcc.gnu.org/onlinedocs/gcc/Common-Type-Attributes.html#index-unused-type-attribute
 *   gcc: https://gcc.gnu.org/onlinedocs/gcc/Common-Variable-Attributes.html#index-unused-variable-attribute
 *   gcc: https://gcc.gnu.org/onlinedocs/gcc/Label-Attributes.html#index-unused-label-attribute
 * clang: https://clang.llvm.org/docs/AttributeReference.html#maybe-unused-unused
 */
#define __always_unused         __attribute__((unused))
#define __maybe_unused          __attribute__((unused))

/*
 *   gcc: https://gcc.gnu.org/onlinedocs/gcc/Common-Function-Attributes.html#index-used-function-attribute
 *   gcc: https://gcc.gnu.org/onlinedocs/gcc/Common-Variable-Attributes.html#index-used-variable-attribute
 */
#define __used                  __attribute__((used))

/*
 *   gcc: https://gcc.gnu.org/onlinedocs/gcc/Common-Function-Attributes.html#index-weak-function-attribute
 *   gcc: https://gcc.gnu.org/onlinedocs/gcc/Common-Variable-Attributes.html#index-weak-variable-attribute
 */
#define __weak                  __attribute__((weak))

#endif /* __LINUX_COMPILER_ATTRIBUTES_H */
