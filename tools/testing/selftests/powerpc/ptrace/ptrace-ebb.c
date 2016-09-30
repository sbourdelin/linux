/*
 * Ptrace interface test for EBB
 *
 * Copyright (C) 2015 Anshuman Khandual, IBM Corporation.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */
#include "ebb.h"
#include "ptrace.h"
#include "ptrace-ebb.h"

/* Tracer and Tracee Shared Data */
int shm_id;
int *cptr, *pptr;

void ebb(void)
{
	struct event event;

	cptr = (int *)shmat(shm_id, NULL, 0);

	event_init_named(&event, 0x1001e, "cycles");
	event.attr.config |= (1ull << 63);
	event.attr.exclusive = 1;
	event.attr.pinned = 1;
	event.attr.exclude_kernel = 1;
	event.attr.exclude_hv = 1;
	event.attr.exclude_idle = 1;

	if (event_open(&event)) {
		perror("event_open() failed");
		exit(1);
	}

	setup_ebb_handler(standard_ebb_callee);
	mtspr(SPRN_BESCR, 0x8000000100000000ull);

	/*
	 * make sure BESCR has been set before continue
	 */
	mb();

	if (ebb_event_enable(&event)) {
		perror("ebb_event_handler() failed");
		exit(1);
	}

	mtspr(SPRN_PMC1, pmc_sample_period(SAMPLE_PERIOD));
	core_busy_loop();
	cptr[0] = 1;
	while (1)
		asm volatile("" : : : "memory");

	exit(0);
}

int validate_ebb(struct ebb_regs *regs)
{
	#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
	struct opd *opd = (struct opd *) ebb_handler;
	#endif

	printf("EBBRR: %lx\n", regs->ebbrr);
	#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
	printf("EBBHR: %lx; expected: %lx\n",
			regs->ebbhr, (unsigned long)opd->entry);
	#else
	printf("EBBHR: %lx; expected: %lx\n",
			regs->ebbhr, (unsigned long)ebb_handler);
	#endif
	printf("BESCR: %lx\n", regs->bescr);

	#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
	if (regs->ebbhr != opd->entry)
		return TEST_FAIL;
	#else
	if (regs->ebbhr != (unsigned long) ebb_handler)
		return TEST_FAIL;
	#endif

	return TEST_PASS;
}

int validate_pmu(struct pmu_regs *regs)
{
	printf("SIAR:  %lx\n", regs->siar);
	printf("SDAR:  %lx\n", regs->sdar);
	printf("SIER:  %lx; expected: %lx\n",
			regs->sier, (unsigned long)SIER_EXP);
	printf("MMCR2: %lx; expected: %lx\n",
			regs->mmcr2, (unsigned long)MMCR2_EXP);
	printf("MMCR0: %lx; expected: %lx\n",
			regs->mmcr0, (unsigned long)MMCR0_EXP);

	/* Validate SIER */
	if (regs->sier != SIER_EXP)
		return TEST_FAIL;

	/* Validate MMCR2 */
	if (regs->mmcr2 != MMCR2_EXP)
		return TEST_FAIL;

	/* Validate MMCR0 */
	if (regs->mmcr0 != MMCR0_EXP)
		return TEST_FAIL;

	return TEST_PASS;
}

int trace_ebb_pmu(pid_t child)
{
	struct ebb_regs ebb_regs;
	struct pmu_regs pmu_regs;
	int ret;

	ret = start_trace(child);
	if (ret)
		return TEST_FAIL;

	ret = show_ebb_registers(child, &ebb_regs);
	if (ret)
		return TEST_FAIL;

	ret = validate_ebb(&ebb_regs);
	if (ret)
		return TEST_FAIL;

	ret = show_pmu_registers(child, &pmu_regs);
	if (ret)
		return TEST_FAIL;

	ret = validate_pmu(&pmu_regs);
	if (ret)
		return TEST_FAIL;

	ret = stop_trace(child);
	if (ret)
		return TEST_FAIL;

	return TEST_PASS;
}

int ptrace_ebb_pmu(void)
{
	pid_t pid;
	int ret, status;

	shm_id = shmget(IPC_PRIVATE, sizeof(int) * 1, 0777|IPC_CREAT);
	pid = fork();
	if (pid < 0) {
		perror("fork() failed");
		return TEST_FAIL;
	}

	if (pid == 0)
		ebb();

	if (pid) {
		pptr = (int *)shmat(shm_id, NULL, 0);
		while (!pptr[0])
			asm volatile("" : : : "memory");

		ret = trace_ebb_pmu(pid);
		if (ret)
			return TEST_FAIL;

		shmctl(shm_id, IPC_RMID, NULL);
		kill(pid, SIGKILL);
		ret = wait(&status);
		if (ret != pid) {
			printf("Child's exit status not captured\n");
			return TEST_FAIL;
		}

		return (WIFEXITED(status) && WEXITSTATUS(status)) ? TEST_FAIL :
			TEST_PASS;
	}
	return TEST_PASS;
}

int main(int argc, char *argv[])
{
	return test_harness(ptrace_ebb_pmu, "ptrace_ebb_pmu");
}
