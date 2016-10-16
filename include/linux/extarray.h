#ifndef LINUX_EXTARRAY_H
#define LINUX_EXTARRAY_H

#include <linux/compiler.h>

/*
 * A common pattern in the kernel is to put certain objects in a specific
 * named section and then create variables in the linker script pointing
 * to the start and the end of this section. These variables are declared
 * as extern arrays to allow C code to iterate over the list of objects.
 *
 * In C, comparing pointers to objects in two different arrays is undefined.
 * GCC version 7.0 and newer (commit 73447cc5d17) will aggressively optimize
 * out such comparisons if it can prove that the two pointers point to
 * different arrays (which is the case when the arrays are declared as two
 * separate variables). This breaks the typical code used to iterate over
 * such arrays.
 *
 * One way to get around this limitation is to force GCC to lose any array
 * information about the pointers before we compare them. We can use e.g.
 * OPTIMIZER_HIDE_VAR() for this.
 *
 * This file defines a few helpers to deal with declaring and accessing
 * such linker-script-defined arrays.
 */


#define DECLARE_EXTARRAY(type, name)					\
	extern type __start_##name[];					\
	extern type __stop_##name[];					\

#define _ext_start(name, tmp) \
	({								\
		typeof(*__start_##name) *tmp = __start_##name;		\
		OPTIMIZER_HIDE_VAR(tmp);				\
		tmp;							\
	})

#define ext_start(name) _ext_start(name, __UNIQUE_ID(ext_start_))

#define _ext_end(name, tmp)						\
	({								\
		typeof(*__stop_##name) *tmp = __stop_##name;		\
		OPTIMIZER_HIDE_VAR(tmp);				\
		tmp;							\
	})

#define ext_end(name) _ext_end(name, __UNIQUE_ID(ext_end_))

#define _ext_size(name, tmp1, tmp2)					\
	({								\
		typeof(*__start_##name) *tmp1 = __start_##name;		\
		typeof(*__stop_##name) *tmp2 = __stop_##name;		\
		OPTIMIZER_HIDE_VAR(tmp1);				\
		OPTIMIZER_HIDE_VAR(tmp2);				\
		tmp2 - tmp1;						\
	})

#define ext_size(name) \
	_ext_size(name, __UNIQUE_ID(ext_size1_), __UNIQUE_ID(ext_size2_))

#define ext_for_each(var, name) \
	for (var = ext_start(name); var != ext_end(name); var++)

#endif
