#include <ppc-asm.h>
#include <asm/unistd.h>

#if defined(_CALL_ELF) && _CALL_ELF == 2
#define STACK_FRAME_MIN_SIZE 32
#define STACK_FRAME_TOC_POS  24
#define __STACK_FRAME_PARAM(_param)  (32 + ((_param)*8))
#define __STACK_FRAME_LOCAL(_num_params,_var_num)  ((STACK_FRAME_PARAM(_num_params)) + ((_var_num)*8))
#else
#define STACK_FRAME_MIN_SIZE 112
#define STACK_FRAME_TOC_POS  40
#define __STACK_FRAME_PARAM(i)  (48 + ((i)*8))
/*
 * Caveat: if a function passed more than 8 params, the caller will have
 * made more space... this should be reflected by this C code.
 * if (_num_params > 8)
 *     total = 112 + ((_num_params - 8) * 8)
 *
 * And substitute the '112' for 'total' in the macro. Doable in preprocessor for ASM?
 */
#define __STACK_FRAME_LOCAL(_num_params,_var_num)  (112 + ((_var_num)*8))
#endif
/* Parameter x saved to the stack */
#define STACK_FRAME_PARAM(var)    __STACK_FRAME_PARAM(var)
/* Local variable x saved to the stack after x parameters */
#define STACK_FRAME_LOCAL(num_params,var)    __STACK_FRAME_LOCAL(num_params,var)
#define STACK_FRAME_LR_POS   16
#define STACK_FRAME_CR_POS   8

#define LOAD_REG_IMMEDIATE(reg,expr) \
	lis	reg,(expr)@highest;	\
	ori	reg,reg,(expr)@higher;	\
	rldicr	reg,reg,32,31;	\
	oris	reg,reg,(expr)@high;	\
	ori	reg,reg,(expr)@l;

/* It is very important to note here that _extra is the extra amount of
 * stack space needed.
 * This space must be accessed using STACK_FRAME_PARAM() or
 * STACK_FRAME_LOCAL() macros!
 *
 * r1 and r2 are not defined in ppc-asm.h (instead they are defined as sp
 * and toc). Kernel programmers tend to prefer rX even for r1 and r2, hence
 * %1 and %r2. r0 is defined in ppc-asm.h and therefore %r0 gets
 * preprocessed incorrectly, hence r0.
 */
#define PUSH_BASIC_STACK(_extra) \
	mflr	r0; \
	std	r0,STACK_FRAME_LR_POS(%r1); \
	stdu	%r1,-(_extra + STACK_FRAME_MIN_SIZE)(%r1); \
	mfcr	r0; \
	stw	r0,STACK_FRAME_CR_POS(%r1); \
	std	%r2,STACK_FRAME_TOC_POS(%r1);

#define POP_BASIC_STACK(_extra) \
	ld	%r2,STACK_FRAME_TOC_POS(%r1); \
	lwz	r0,STACK_FRAME_CR_POS(%r1); \
	mtcr	r0; \
	addi	%r1,%r1,(_extra + STACK_FRAME_MIN_SIZE); \
	ld	r0,STACK_FRAME_LR_POS(%r1); \
	mtlr	r0;

