/*
 * Copyright 2016, Cyril Bur, IBM Corp.
 * Licensed under GPLv2.
 *
 * Test the kernel's signal frame code.
 *
 * The kernel sets up two sets of ucontexts if the signal was to be delivered
 * while the thread was in a transaction. Expected behaviour is that the
 * currently executing code is in the first and the checkpointed state (the
 * state that will be rolled back to) is in the uc_link ucontext.
 *
 * The reason for this is that code which is not TM aware and installs a signal
 * handler will expect to see/modify its currently running state in the uc,
 * this code may have dynamicially linked against code which is TM aware and is
 * doing HTM under the hood.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>

#include <altivec.h>

#include "utils.h"
#include "tm.h"

#define TBEGIN          ".long 0x7C00051D ;"
#define TSUSPEND        ".long 0x7C0005DD ;"
#define TRESUME         ".long 0x7C2005DD ;"
#define MAX_ATTEMPT 100

extern long tm_signal_self_context_load(pid_t pid, long *gps, double *fps, vector int *vms, vector int *vss);

static int signaled;
static int fail;

vector int vms[] = {{1, 2, 3, 4}, {5, 6, 7, 8}, {9, 10,11,12},
	{13,14,15,16},{17,18,19,20},{21,22,23,24},
	{25,26,27,28},{29,30,31,32},{33,34,35,36},
	{37,38,39,40},{41,42,43,44},{45,46,47,48},
	{-1, -2, -3, -4}, {-5, -6, -7, -8}, {-9, -10,-11,-12},
	{-13,-14,-15,-16},{-17,-18,-19,-20},{-21,-22,-23,-24},
	{-25,-26,-27,-28},{-29,-30,-31,-32},{-33,-34,-35,-36},
	{-37,-38,-39,-40},{-41,-42,-43,-44},{-45,-46,-47,-48}};

static void signal_usr1(int signum, siginfo_t *info, void *uc)
{
	int i;
	ucontext_t *ucp = uc;
	ucontext_t *tm_ucp = ucp->uc_link;

	signaled = 1;

	/* Always be 64bit, don't really care about 32bit */
	for (i = 0; i < 12 && !fail; i++) {
		fail = memcmp(ucp->uc_mcontext.v_regs->vrregs[i + 20], &vms[i], 16);
		fail |= memcmp(tm_ucp->uc_mcontext.v_regs->vrregs[i + 20], &vms[i + 12], 16);
	}
	if (fail) {
		int j;

		fprintf(stderr, "Failed on %d vmx 0x", i - 1);
		for (j = 0; j < 4; j++)
			fprintf(stderr, "%08x", ucp->uc_mcontext.v_regs->vrregs[i + 19][j]);
		fprintf(stderr, " vs 0x");
		for (j = 0 ; j < 4; j++)
			fprintf(stderr, "%08x", tm_ucp->uc_mcontext.v_regs->vrregs[i + 19][j]);
		fprintf(stderr, "\n");
		return;
	}
}

static int tm_signal_context_chk()
{
	struct sigaction act;
	int i;
	long rc;
	pid_t pid = getpid();

	SKIP_IF(!have_htm());

	act.sa_sigaction = signal_usr1;
	sigemptyset(&act.sa_mask);
	act.sa_flags = SA_SIGINFO;
	if (sigaction(SIGUSR1, &act, NULL) < 0) {
		perror("sigaction sigusr1");
		exit(1);
	}

	i = 0;
	while (!signaled && i < MAX_ATTEMPT) {
		rc = tm_signal_self_context_load(pid, NULL, NULL, vms, NULL);
		if (!rc) {
			fprintf(stderr, "Transaction was not doomed...\n");
			FAIL_IF(!rc);
		}
		i++;
	}

	if (i == MAX_ATTEMPT) {
		fprintf(stderr, "Tried to signal %d times and didn't work, failing!\n", MAX_ATTEMPT);
		fail = 1;
	}
	return fail;
}

int main(void)
{
	return test_harness(tm_signal_context_chk, "tm_signal_context_chk_vmx");
}
