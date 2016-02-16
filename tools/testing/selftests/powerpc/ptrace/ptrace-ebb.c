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
#include "../pmu/ebb/ebb.h"
#include "ptrace.h"
#include "ptrace-ebb.h"

void ebb(void)
{
	struct event event;

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

	mb();

	if (ebb_event_enable(&event)) {
		perror("ebb_event_handler() failed");
		exit(1);
	}

	mtspr(SPRN_PMC1, pmc_sample_period(SAMPLE_PERIOD));
	while(1)
		core_busy_loop();
	exit(0);
}

int validate_ebb(struct ebb_regs *regs)
{
	#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
	struct opd *opd = (struct opd *) ebb_handler;
	#endif

	printf("EBBRR: %lx\n", regs->ebbrr);
	printf("EBBHR: %lx\n", regs->ebbhr);
	printf("BESCR: %lx\n", regs->bescr);
	printf("SIAR:  %lx\n", regs->siar);
	printf("SDAR:  %lx\n", regs->sdar);
	printf("SIER:  %lx\n", regs->sier);
	printf("MMCR2: %lx\n", regs->mmcr2);
	printf("MMCR0: %lx\n", regs->mmcr0);

	/* Validate EBBHR */
	#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
	if (regs->ebbhr != opd->entry)
		return TEST_FAIL;
	#else
	if (regs->ebbhr != (unsigned long) ebb_handler)
		return TEST_FAIL;
	#endif

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

int trace_ebb(pid_t child)
{
	struct ebb_regs regs;
	int ret;

	sleep(2);
	ret = start_trace(child);
	if (ret)
		return TEST_FAIL;

	ret = show_ebb_registers(child, &regs);
	if (ret)
		return TEST_FAIL;

	ret = validate_ebb(&regs);
	if (ret)
		return TEST_FAIL;

	ret = stop_trace(child);
	if (ret)
		return TEST_FAIL;

	return TEST_PASS;
}

int ptrace_ebb(void)
{
	pid_t pid;
	int ret, status;

	pid = fork();
	if (pid < 0) {
		perror("fork() failed");
		return TEST_FAIL;
	}

	if (pid == 0)
		ebb();

	if (pid) {
		ret = trace_ebb(pid);
		if (ret)
			return TEST_FAIL;

		kill(pid, SIGKILL);
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
	return test_harness(ptrace_ebb, "ptrace_ebb");
}
