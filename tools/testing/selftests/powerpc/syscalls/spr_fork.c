/*
 * Copyright 2016, Cyril Bur, IBM Corp.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 *
 * This test checks that the TAR register (an SPR) is correctly preserved
 * across a fork()
 */
#include <asm/cputable.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/auxv.h>
#include <sys/wait.h>
#include <unistd.h>

#include "utils.h"

static int fork_spr(void)
{
	int child_ret;
	unsigned long tar;
	pid_t pid;
	int i;


	/* Do it a few times as there is a chance that one might luckily pass */
	i = 0;
	while (i < 10) {
		/* What are the odds... */
		tar = 0x123456;
		asm __volatile__(
				"mtspr 815, %[tar]"
				:
				: [tar] "r" (tar)
				);

		pid = fork();
		FAIL_IF(pid == -1);
		asm __volatile__(
				"mfspr %[tar], 815"
				: [tar] "=r" (tar)
				);

		FAIL_IF(tar != 0x123456);

		if (pid == 0)
			exit(0);

		FAIL_IF(waitpid(pid, &child_ret, 0) == -1);

		/* Child haddn't exited ? */
		FAIL_IF(!WIFEXITED(child_ret));

		/* Child detected a bad tar */
		FAIL_IF(WEXITSTATUS(child_ret));

		/* Reset it */
		tar = 0;
		asm __volatile__(
				"mtspr 815, %[tar]"
				:
				: [tar] "r" (tar)
				);

		i++;
	}

	return 0;
}

int main(int argc, char *argv[])
{
	SKIP_IF(!have_hwcap2(PPC_FEATURE2_TAR));
	return test_harness(fork_spr, "spr_fork");
}
