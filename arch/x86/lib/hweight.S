#include <linux/linkage.h>

#include <asm/asm.h>

/*
 * unsigned int __sw_hweight32(unsigned int w)
 * %rdi: w
 */
ENTRY(__sw_hweight32)

	__ASM_SIZE(push,) %__ASM_REG(dx)

	movl %edi, %edx

	shrl %edx
	andl $0x55555555, %edx
	subl %edx, %edi
	movl %edi, %eax

	shrl $2, %edi
	andl $0x33333333, %eax
	andl $0x33333333, %edi
	addl %eax, %edi

	movl %edi, %eax
	shrl $4, %eax
	addl %edi, %eax
	andl  $0x0f0f0f0f, %eax
	imull $0x01010101, %eax, %eax
	shrl $24, %eax
	__ASM_SIZE(pop,) %__ASM_REG(dx)
	ret
ENDPROC(__sw_hweight32)

ENTRY(__sw_hweight64)

	pushq   %rdx

	movq    %rdi, %rdx
	movabsq $0x5555555555555555, %rax
	shrq    %rdx
	andq    %rax, %rdx
	subq    %rdx, %rdi
	movabsq $0x3333333333333333, %rdx

	movq    %rdi, %rax
	shrq    $2, %rdi
	andq    %rdx, %rax
	andq    %rdx, %rdi
	movabsq $0x0f0f0f0f0f0f0f0f, %rdx
	addq    %rax, %rdi

	movq    %rdi, %rax
	shrq    $4, %rax
	addq    %rdi, %rax
	andq    %rdx, %rax
	movabsq $0x0101010101010101, %rdx
	imulq   %rdx, %rax
	shrq    $56, %rax

	popq    %rdx
	ret
#else /* CONFIG_X86_32 */
	/* We're getting an u64 arg in (%eax,%edx): unsigned long hweight64(__u64 w) */
	pushl   %ecx

	call    __sw_hweight32
	movl    %eax, %ecx                      # stash away result
	movl    %edx, %eax                      # second part of input
	call    __sw_hweight32
	addl    %ecx, %eax                      # result

	popl    %ecx
	ret
#endif
ENDPROC(__sw_hweight64)
