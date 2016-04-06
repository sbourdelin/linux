/*
 * arch_prctl_set_compat.c - tests switching to compatible mode from 64-bit
 * Copyright (c) 2016 Dmitry Safonov
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * This switches to compatible mode with the help from arch_prctl friend.
 * Switching is simple syscall, but one need unmap every vma that is
 * higher than 32-bit TASK_SIZE, make raw 32/64-bit syscalls.
 * So this is also a really good example. By the end tracee is
 * compatible task that makes 32-bit syscalls to stop itself.
 * For returning in some 32-bit code it may be handy to use sigreturn
 * there with formed frame.
 *
 * Switching from 32-bit compatible application to native is just one
 * arch_prctl syscall, so this is for harder task: switching from native to
 * compat mode.
 */
#define _GNU_SOURCE

#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <stdio.h>
#include <signal.h>
#include <stdint.h>

#include <sys/syscall.h>
#include <asm/prctl.h>
#include <sys/prctl.h>
#include <sys/personality.h>
#include <sys/mman.h>

#include <sys/uio.h>
#include <sys/ptrace.h>
#include <linux/elf.h>
#include <sys/wait.h>

#include <sys/stat.h>
#include <fcntl.h>

#define __stringify_1(x...)	#x
#define __stringify(x...)	__stringify_1(x)

#ifndef ARCH_SET_COMPAT
#define ARCH_SET_COMPAT		0x2001
#define ARCH_SET_NATIVE		0x2002
#define ARCH_GET_PERSONALITY	0x2003
#endif

#define PAGE_SIZE		4096
#define TASK_SIZE_MAX		((1UL << 47) - PAGE_SIZE)
#define IA32_PAGE_OFFSET	0xFFFFe000

/* Just a typical random stack on x86_64 compatible task */
#define STACK_START		0xffdb8000
#define STACK_END		0xffdd9000

/* Some empty randoms inside compatible address space */
#define ARG_START 0xf77c8000
#define ARG_END 0xf77c8000
#define ENV_START 0xf77c8000
#define ENV_END 0xf77c8000

/*
 * After removing all mappings higher than compatible TASK_SIZE,
 * we remove libc mapping. That's the reason for plain syscalls
 */
#define __NR_munmap		11
#define __NR_arch_prctl		158

#define __NR32_getpid		20
#define __NR32_kill		37

/* unmaps everything above IA32_PAGE_OFFSET */
static inline void unmap_uncompat_mappings(void)
{
	unsigned long addr = IA32_PAGE_OFFSET;
	unsigned long len = TASK_SIZE_MAX - IA32_PAGE_OFFSET;

	asm volatile(
		"	movq $"__stringify(__NR_munmap)", %%rax\n"
		"	syscall\n"
		:
		: "D" (addr), "S" (len)
	);
}

static inline void sys_arch_prctl(int code, unsigned long addr)
{
	asm volatile(
		"	movq $"__stringify(__NR_arch_prctl)", %%rax\n"
		"	syscall\n"
		:
		: "D" (code), "S" (addr)
	);
}

static inline void
prctl_print(int opcode, unsigned long arg1, unsigned long arg2)
{
	long ret = syscall(SYS_prctl, opcode, arg1, arg2, 0, 0);

	if (ret)
		fprintf(stderr, "[ERR]\tprctl failed with %ld : %m\n", ret);
}

/*
 * Runed in different task just for test purposes:
 * tracer with the help of PTRACE_GETREGS will fetch it's registers set size
 * and determine, if it's compatible task.
 * Then tracer will kill tracee, sorry for it.
 */
void tracee_func(void)
{
	personality(PER_LINUX32);

	/* emptify arg & env, moving them to compatible address space */
	prctl_print(PR_SET_MM, PR_SET_MM_ARG_START, ARG_START);
	prctl_print(PR_SET_MM, PR_SET_MM_ARG_END, ARG_END);
	prctl_print(PR_SET_MM, PR_SET_MM_ENV_START, ENV_START);
	prctl_print(PR_SET_MM, PR_SET_MM_ENV_END, ENV_END);

	/* stack: get a new one */
	if (mmap((void *)STACK_START, STACK_END - STACK_START,
				PROT_READ | PROT_WRITE,
				MAP_PRIVATE | MAP_FIXED | MAP_ANONYMOUS, -1, 0)
			== MAP_FAILED) {
		fprintf(stderr, "[ERR]\tfailed to mmap new stack : %m\n");
	} else {
		prctl_print(PR_SET_MM, PR_SET_MM_START_STACK, STACK_START);
		asm volatile (
				"	mov %%rax,%%rsp\n"
				:
				: "a" (STACK_END));
	}
	/* we are cool guys: we have our own stack */

	unmap_uncompat_mappings();
	/*
	 * we are poor boys: unmapped everything with glibc,
	 * do not use it from now - we are on our own!
	 */

	sys_arch_prctl(ARCH_SET_COMPAT, 0);

	/* Now switch to compatibility mode */
	asm volatile (
		"	pushq $0x23\n"	/* USER32_CS */
		"	pushq $1f\n"
		"	lretq\n"
		/* here we are: ready to execute some 32-bit code */
		"1:\n"
		".code32\n"
		/* getpid() */
		"	movl $"__stringify(__NR32_getpid)", %eax\n"
		"	int $0x80\n"
		"	movl %eax, %ebx\n" /* pid */
		/* raise SIGSTOP */
		"	movl $"__stringify(__NR32_kill)", %eax\n"
		"	movl $19, %ecx\n"
		"	int $0x80\n"
		".code64\n"
	);

}

typedef struct {
	uint64_t	r15, r14, r13, r12, bp, bx, r11, r10, r9, r8;
	uint64_t	ax, cx, dx, si, di, orig_ax, ip, cs, flags;
	uint64_t	sp, ss, fs_base, gs_base, ds, es, fs, gs;
} user_regs_64;

typedef struct {
	uint32_t	bx, cx, dx, si, di, bp, ax, ds, es, fs, gs;
	uint32_t	orig_ax, ip, cs, flags, sp, ss;
} user_regs_32;

typedef union {
	user_regs_64 native;
	user_regs_32 compat;
} user_regs_struct_t;

int ptrace_task_compatible(pid_t pid)
{
	struct iovec iov;
	user_regs_struct_t r;
	size_t reg_size = sizeof(user_regs_64);

	iov.iov_base = &r;
	iov.iov_len = reg_size;

	errno = 0;
	if (ptrace(PTRACE_GETREGSET, pid, NT_PRSTATUS, &iov)) {
		fprintf(stderr, "[NOTE]\tCan't get register set: PTRACE_GETREGSET failed for pid %d : %m\n",
			pid);
		return 0;
	}

	return iov.iov_len == sizeof(user_regs_32);
}

void dump_proc_maps(pid_t pid)
{
#define BUF_SIZE	1024
	char buf[BUF_SIZE];
	int fd;
	size_t nread;

	snprintf(buf, BUF_SIZE, "/proc/%d/maps", pid);
	fd = open(buf, O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "[NOTE]\tCant open %s to dump : %s\n", buf);
		return;
	}

	while ((nread = read(fd, buf, sizeof(buf))) > 0)
		fwrite(buf, 1, nread, stdout);

	close(fd);
}

int main(int argc, char **argv)
{
	pid_t pid;
	int ret = 0;
	int status;
	int in_compat = syscall(SYS_arch_prctl, 0x2003, 0);
	int dump_maps = 0;

	if (in_compat < 0) {
		fprintf(stderr,
			"[ERR]\tSYS_arch_prctl returned %d : %m\n", in_compat);
	}

	if (in_compat == 1) {
		fprintf(stderr, "[SKIP]\tRun in 64-bit x86 userspace\n");
		return 0;
	}

	if (argc > 1)
		dump_maps = !(strcmp(argv[1], "--dump-proc"));

	if (dump_maps)
		dump_proc_maps(getpid());

	fflush(NULL);
	pid = fork();
	if (pid < 0) {
		fprintf(stderr, "[SKIP]\tCan't fork : %m\n");
		return 1;
	}

	if (pid == 0) {/* child */
		ptrace(PTRACE_TRACEME, 0, 0, 0);
		tracee_func();
	}

	/* parent, the tracer */
	waitpid(pid, &status, 0);
	if (WIFEXITED(status)) {
		fprintf(stderr, "[FAIL]\tTest was suddenly killed\n");
		return 2;
	}

	if (WIFSIGNALED(status)) {
		fprintf(stderr, "[FAIL]\tTest killed with signal %d\n",
				WTERMSIG(status));
		return 3;
	}

	if (!WIFSTOPPED(status))
		fprintf(stderr, "[NOTE]\twaitpid() returned, but tracee wasn't stopped\n");

	if (!ptrace_task_compatible) {
		fprintf(stderr, "[FAIL]\tTask didn't become compatible\n");
		ret = 4;
	}

	if (dump_maps)
		dump_proc_maps(pid);

	kill(pid, SIGKILL);
	fprintf(stderr, "[OK]\tSuccessfuly changed mode to compatible\n");

	return ret;
}

