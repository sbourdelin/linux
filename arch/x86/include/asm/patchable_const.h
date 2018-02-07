#ifndef __X86_PATCHABLE_CONST
#define __X86_PATCHABLE_CONST

#ifndef __ASSEMBLY__

#include <linux/types.h>
#include <linux/stringify.h>
#include <asm/asm.h>

void module_patch_const_u64(const char *name,
	       unsigned long **start, unsigned long **stop);

#define DECLARE_PATCHABLE_CONST_U64(id_str)					\
extern int id_str ## _SET(u64 value);						\
static __always_inline __attribute_const__ u64 id_str ## _READ(void)		\
{										\
       u64 ret;									\
       asm (									\
	       "1: movabsq $(" __stringify(id_str ## _DEFAULT) "), %0\n"	\
	       ".pushsection \"const_u64_" __stringify(id_str) "\",\"a\"\n"	\
	       _ASM_PTR "1b\n"							\
	       ".popsection\n" : "=r" (ret));					\
       return ret;								\
}

#endif /* __ASSEMBLY__ */

#endif
