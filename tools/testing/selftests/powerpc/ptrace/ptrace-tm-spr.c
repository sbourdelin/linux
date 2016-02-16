/*
 * Ptrace test TM SPR registers
 *
 * Copyright (C) 2015 Anshuman Khandual, IBM Corporation.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */
#include "ptrace.h"

/* Tracee and tracer shared data */
struct shared {
	int flag;
	struct tm_spr_regs regs;
};
unsigned long tfhar;

int shm_id;
volatile struct shared *cptr, *pptr;

#define TM_SCHED	0xde0000018c000001
#define TM_KVM_SCHED	0xe0000001ac000001

int validate_tm_spr(struct tm_spr_regs *regs)
{
	if (regs->tm_tfhar != (tfhar - 32))
		return TEST_FAIL;

	if ((regs->tm_texasr != TM_SCHED) && (regs->tm_texasr != TM_KVM_SCHED))
		return TEST_FAIL;

	if ((regs->tm_texasr == TM_KVM_SCHED) && (regs->tm_tfiar != 0))
		return TEST_FAIL;

	return TEST_PASS;
}

void tm_spr(void)
{
	unsigned long result, texasr;
	int ret;

	cptr = (struct shared *)shmat(shm_id, NULL, 0);
trans:
	asm __volatile__(
		"1: ;"
		TBEGIN
		"beq 2f;"

		"b .;"

		TEND
		"li 0, 0;"
		"ori %[res], 0, 0;"
		"b 3f;"

		"2: ;"
		"mflr 31;"
		"bl 4f;"	/* $ = TFHAR + 2 */
		"4: ;"
		"mflr %[tfhar];"
		"mtlr 31;"

		"li 0, 1;"
		"ori %[res], 0, 0;"
		"mfspr %[texasr], %[sprn_texasr];"

		"3: ;"
		: [tfhar] "=r" (tfhar), [res] "=r" (result), [texasr] "=r" (texasr)
		: [sprn_texasr] "i"  (SPRN_TEXASR)
		: "memory", "r0", "r1", "r2", "r3", "r4", "r8", "r9", "r10", "r11"
		);

	if (result) {
		if (!cptr->flag)
			goto trans;

		ret = validate_tm_spr((struct tm_spr_regs *)&cptr->regs);
		shmdt((void *)cptr);
		if (ret)
			exit(1);
		exit(0);
	}
	shmdt((void *)cptr);
	exit(1);
}

int trace_tm_spr(pid_t child)
{
	int ret;

	sleep(1);
	ret = start_trace(child);
	if (ret)
		return TEST_FAIL;

	ret = show_tm_spr(child, (struct tm_spr_regs *)&pptr->regs);
	if (ret)
		return TEST_FAIL;

	printf("TFHAR: %lx TEXASR: %lx TFIAR: %lx\n", pptr->regs.tm_tfhar,
				pptr->regs.tm_texasr, pptr->regs.tm_tfiar);

	ret = stop_trace(child);
	if (ret)
		return TEST_FAIL;

	return TEST_PASS;
}

int ptrace_tm_spr(void)
{
	pid_t pid;
	int ret, status;

	SKIP_IF(!((long)get_auxv_entry(AT_HWCAP2) & PPC_FEATURE2_HTM));
	shm_id = shmget(IPC_PRIVATE, sizeof(struct shared), 0777|IPC_CREAT);
	pid = fork();
	if (pid < 0) {
		perror("fork() failed");
		return TEST_FAIL;
	}

	if (pid == 0)
		tm_spr();

	if (pid) {
		pptr = (struct shared *)shmat(shm_id, NULL, 0);
		ret = trace_tm_spr(pid);
		if (ret) {
			kill(pid, SIGKILL);
			return TEST_FAIL;
		}

		pptr->flag = 1;
		shmdt((void *)pptr);
		ret = wait(&status);
		if (ret != pid) {
			printf("Child's exit status not captured\n");
			return TEST_FAIL;
		}

		if (WIFEXITED(status)) {
			if(WEXITSTATUS(status))
				return TEST_FAIL;
		}
		return TEST_PASS;
	}
	return TEST_PASS;
}

int main(int argc, char *argv[])
{
	return test_harness(ptrace_tm_spr, "ptrace_tm_spr");
}
