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

vector int vss[] = {{1, 2, 3, 4}, {5, 6, 7, 8}, {9, 10,11,12},
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
	uint8_t vsc[16];
	uint8_t vst[16];
	ucontext_t *ucp = uc;
	ucontext_t *tm_ucp = ucp->uc_link;

	signaled = 1;

	/*
	 * The other half of the VSX regs will be after v_regs.
	 *
	 * In short, vmx_reserve array holds everything. v_regs is a 16
	 * byte aligned pointer at the start of vmx_reserve (vmx_reserve
	 * may or may not be 16 aligned) where the v_regs structure exists.
	 * (half of) The VSX regsters are directly after v_regs so the
	 * easiest way to find them below.
	 */
	long *vsx_ptr = (long *)(ucp->uc_mcontext.v_regs + 1);
	long *tm_vsx_ptr = (long *)(tm_ucp->uc_mcontext.v_regs + 1);
	/* Always be 64bit, don't really care about 32bit */
	for (i = 0; i < 12 && !fail; i++) {
		memcpy(vsc, &ucp->uc_mcontext.fp_regs[i + 20], 8);
		memcpy(vsc + 8, &vsx_ptr[20 + i], 8);
		fail = memcmp(vsc, &vss[i], 16);
		memcpy(vst, &tm_ucp->uc_mcontext.fp_regs[i + 20], 8);
		memcpy(vst + 8, &tm_vsx_ptr[20 + i], 8);
		fail |= memcmp(vst, &vss[i + 12], 16);
	}
	if (fail) {
		fprintf(stderr, "Failed on %d vsx 0x", i - 1);
		for (i = 0; i < 16; i++)
			fprintf(stderr, "%02x", vsc[i]);
		fprintf(stderr, " vs 0x");
		for (i = 0; i < 16; i++)
			fprintf(stderr, "%02x", vst[i]);
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
		rc = tm_signal_self_context_load(pid, NULL, NULL, NULL, vss);
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
	return test_harness(tm_signal_context_chk, "tm_signal_context_chk_vsx");
}
