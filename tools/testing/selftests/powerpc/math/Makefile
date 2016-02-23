TEST_PROGS := fpu_syscall fpu_preempt vmx_syscall vmx_preempt

all: $(TEST_PROGS)

#The general powerpc makefile adds -flto. This isn't interacting well with the custom ASM.
#filter-out -flto to avoid false failures.
$(TEST_PROGS): ../harness.c
$(TEST_PROGS): CFLAGS = $(filter-out -flto,$(CFLAGS) -O2 -g -pthread -m64 -maltivec)

fpu_syscall: fpu_asm.S
fpu_preempt: fpu_asm.S

vmx_syscall: vmx_asm.S
vmx_preempt: vmx_asm.S

include ../../lib.mk

clean:
	rm -f $(TEST_PROGS) *.o
