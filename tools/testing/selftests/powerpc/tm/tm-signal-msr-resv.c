/*
 * Copyright 2015, Michael Neuling, IBM Corp.
 * Licensed under GPLv2.
 *
 * Test the kernel's signal return code to ensure that it doesn't
 * crash when both the transactional and suspend MSR bits are set in
 * the signal context.
 */

#include <stdlib.h>
#include <stdio.h>
#include <signal.h>

#include "utils.h"

struct sigaction act;

void signal_segv(int signum, siginfo_t *info, void *uc)
{
	printf("PASSED\n");
	exit(0);
}

void signal_usr1(int signum, siginfo_t *info, void *uc)
{
	ucontext_t *ucp = uc;

	/* link tm checkpointed context to normal context */
	ucp->uc_link = ucp;
	/* set all TM bits */
#ifdef __powerpc64__
	ucp->uc_mcontext.gp_regs[PT_MSR] |= (7ULL << 32);
#else
	ucp->uc_mcontext.regs->gpr[PT_MSR] |= (7ULL);
#endif
	/* Should segv on return becuase of invalid context */
	act.sa_sigaction = signal_segv;
	if (sigaction(SIGSEGV, &act, NULL) < 0) {
		perror("sigaction sigsegv");
		exit(1);
	}
}

int tm_signal_msr_resv()
{

	act.sa_sigaction = signal_usr1;
	sigemptyset(&act.sa_mask);
	act.sa_flags = SA_SIGINFO;
	if (sigaction(SIGUSR1, &act, NULL) < 0) {
		perror("sigaction sigusr1");
		exit(1);
	}

	raise(SIGUSR1);

	printf("FAILED\n");
	return 1;
}

int main(void)
{
	return test_harness(tm_signal_msr_resv, "tm_signal_msr_resv");
}
