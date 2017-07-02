/*
 * Copyright 2017, Gustavo Romero and Breno Leitao, IBM Corp.
 * Licensed under GPLv2.
 *
 * Force VSX unavailable exception during a transaction and see
 * if it corrupts the checkpointed FP registers state after the abort.
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <inttypes.h>
#include <pthread.h>
#include <sched.h>

#include "tm.h"
#include "utils.h"

int passed;

void *ping(void *not_used)
{
	asm goto(
		// r3 = 0x5555555555555555
		"lis  3, 0x5555    ;"
		"ori  3, 3, 0x5555 ;"
		"sldi 3, 3, 32     ;"
		"oris 3, 3, 0x5555 ;"
		"ori  3, 3, 0x5555 ;"

		//r4 = 0xFFFFFFFFFFFFFFFF
		"lis  4, 0xFFFF    ;"
		"ori  4, 4, 0xFFFF ;"
		"sldi 4, 4, 32     ;"
		"oris 4, 4, 0xFFFF ;"
		"ori  4, 4, 0xFFFF ;"

		// vs33 and vs34 will just be used to construct vs0 from r3 and
		// r4. Both won't be used in any other place after that.
		"mtvsrd 33, 3      ;"
		"mtvsrd 34, 4      ;"

		// vs0 = (r3 || r4) = 0x5555555555555555FFFFFFFFFFFFFFFF
		"xxmrghd 0, 33, 34 ;"


		// Wait ~8s so we have a sufficient amount of context
		// switches so load_fp and load_vec overflow and MSR.FP, MSR.VEC
		// and MSR.VSX are disabled.
		"       lis	 7, 0x1       ;"
		"       ori      7, 7, 0xBFFE ;"
		"       sldi     7, 7, 15     ;"
		"1:	addi     7, 7, -1     ;"
		"       cmpdi    7, 0         ;"
		"       bne      1b           ;"

		// Any floating-point instruction in here.
		// N.B. 'fmr' is *not touching* any previously set register,
		// i.e. it's not touching vs0.
		"fmr    10, 10     ;"

		// vs0 is *still* 0x5555555555555555FFFFFFFFFFFFFFFF, right?
		// Get in a transaction and cause a VSX unavailable exception.
		"2:	tbegin.               ;" // Begin HTM
		"       beq      3f           ;" // Failure handler
		"       xxmrghd  10, 10, 10   ;" // VSX unavailable in TM
		"       tend.                 ;" // End HTM
		"3:     nop                   ;" // Fall through to code below

		// Immediately after a transaction failure we save vs0 to two
		// general purpose registers to check its value. We need to have
		// the same value as before we entered in transactional state.

		// vs0 should be *still* 0x5555555555555555FFFFFFFFFFFFFFFF

		// Save high half - MSB (64bit).
		"mfvsrd  5, 0                 ;"

		// Save low half - LSB (64bit).
		// We mess with vs3, but it's not important.
		"xxsldwi 3, 0, 0, 2           ;"
		"mfvsrd  6, 3                 ;"

		// N.B. r3 and r4 never changed since they were used to
		// construct the initial vs0 value, hence we can use them to do
		// the comparison. r3 and r4 will be destroy but it's ok.
		"cmpd  3, 5                ;" // compare r3 to r5
		"bne   %[value_mismatch]      ;"
		"cmpd  4, 6                ;" // compare r4 to r6
		"bne   %[value_mismatch]      ;"
		"b     %[value_ok]            ;"
		:
		:
		: "r3", "r4", "vs33", "vs34", "vs0",
		  "vs10", "fr10", "r7", "r5", "r6", "vs3"
		: value_mismatch, value_ok
		);
value_mismatch:
		passed = 0;
		return NULL;
value_ok:
		passed = 1;
		return NULL;
}

void *pong(void *not_used)
{
	while (1)
		sched_yield(); // will be classed as interactive-like thread
}

int tm_vsx_unavail_test(void)
{
	pthread_t t0, t1;
	pthread_attr_t attr;
	cpu_set_t cpuset;

	// Set only CPU 0 in the mask. Both threads will be bound to cpu 0
	CPU_ZERO(&cpuset);
	CPU_SET(0, &cpuset);

	// Init pthread attribute
	pthread_attr_init(&attr);

	// Set CPU 0 mask into the pthread attribute
	pthread_attr_setaffinity_np(&attr, sizeof(cpu_set_t), &cpuset);

	// 'pong' thread used to induce context switches on 'ping' thread
	pthread_create(&t1, &attr /* bound to cpu 0 */, pong, NULL);

	printf("Checking if FP/VSX is sane after a VSX exception in TM...\n");

	pthread_create(&t0, &attr /* bound to cpu 0 as well */, ping, NULL);
	pthread_join(t0, NULL);

	return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}

int main(int argc, char **argv)
{
	return test_harness(tm_vsx_unavail_test, "tm_vsx_unavail_test");
}
