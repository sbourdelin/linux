#include <ppc-asm.h>
#include <asm/unistd.h>

#define LOAD_REG_IMMEDIATE(reg,expr) \
	lis	reg,(expr)@highest;	\
	ori	reg,reg,(expr)@higher;	\
	rldicr	reg,reg,32,31;	\
	oris	reg,reg,(expr)@high;	\
	ori	reg,reg,(expr)@l;

/* It is very important to note here that _extra is the extra amount of
 * stack space needed.
 * This space must be accessed at sp + 32!
 */
#define PUSH_BASIC_STACK(_extra) \
	mflr	r0; \
	std	r0,16(sp); \
	stdu	sp,-(_extra + 32)(sp); \
	mfcr	r0; \
	stw	r0,8(sp); \
	std	2,24(sp);

#define POP_BASIC_STACK(_extra) \
	ld	2,24(sp); \
	lwz	r0,8(sp); \
	mtcr	r0; \
	addi	sp,sp,(_extra + 32); \
	ld	r0,16(sp); \
	mtlr	r0;

