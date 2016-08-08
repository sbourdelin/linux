#include <linux/linkage.h>

#include <asm/asm.h>

/*
 * unsigned int __sw_hweight32(unsigned int w)
 * %rdi: w
 */
ENTRY(__sw_hweight32)

	__ASM_SIZE(push,) %__ASM_REG(dx)
#ifdef CONFIG_X86_64
	movl %edi, %edx				# w -> t
#endif
	shrl %edx				# w >>= 1
	andl $0x55555555, %edx			# w &= 0x55555555
	subl %edx, %edi				# w -= t
	movl %edi, %eax				# w -> t

	shrl $2, %edi				# w_tmp >>= 2
	andl $0x33333333, %eax			# t	&= 0x33333333
	andl $0x33333333, %edi			# w_tmp &= 0x33333333
	addl %eax, %edi				# w = w_tmp + t

	movl %edi, %eax				# w -> t
	shrl $4, %eax				# t >>= 4
	addl %edi, %eax				# w_tmp += t
	andl  $0x0f0f0f0f, %eax			# w_tmp &= 0x0f0f0f0f
	imull $0x01010101, %eax, %eax		# w_tmp *= 0x01010101
	shrl $24, %eax				# w = w_tmp >> 24
	__ASM_SIZE(pop,) %__ASM_REG(dx)
	ret
ENDPROC(__sw_hweight32)

ENTRY(__sw_hweight64)
#ifdef CONFIG_X86_64
	pushq   %rdx

	movq    %rdi, %rdx                      # w -> t
	movabsq $0x5555555555555555, %rax
	shrq    %rdx                            # t >>= 1
	andq    %rax, %rdx                      # t &= 0x5555555555555555
	subq    %rdx, %rdi                      # w -= t
	movabsq $0x3333333333333333, %rdx

	movq    %rdi, %rax                      # w -> t
	shrq    $2, %rdi                        # w_tmp >>= 2
	andq    %rdx, %rax                      # t     &= 0x3333333333333333
	andq    %rdx, %rdi                      # w_tmp &= 0x3333333333333333
	movabsq $0x0f0f0f0f0f0f0f0f, %rdx
	addq    %rax, %rdi                      # w = w_tmp + t

	movq    %rdi, %rax                      # w -> t
	shrq    $4, %rax                        # t >>= 4
	addq    %rdi, %rax                      # w_tmp += t
	andq    %rdx, %rax                      # w_tmp &= 0x0f0f0f0f0f0f0f0f
	movabsq $0x0101010101010101, %rdx
	imulq   %rdx, %rax                      # w_tmp *= 0x0101010101010101
	shrq    $56, %rax                       # w = w_tmp >> 56

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
