/*
 * Copyright 2016, Cyril Bur, IBM Corp.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 *
 * This test checks that the TAR (an SPR) is correctly sanitised across
 * execve()
 */
#include <asm/cputable.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/auxv.h>
#include <unistd.h>

#include "utils.h"

static char *name;
static int count;

static int exec_spr(void)
{
	unsigned long tar;
	char buffer[10];
	char *args[3];

	asm __volatile__(
			"mfspr %[tar], 815"
			: [tar] "=r" (tar)
			);
	/* Read TAR */
	FAIL_IF(tar != 0);

	tar = 1;
	asm __volatile__(
			"mtspr 815, %[tar]"
			:
			: [tar] "r" (tar)
			);

	FAIL_IF(sprintf(buffer, "%d", count + 1) == -1);
	args[0] = name;
	args[1] = buffer;
	args[2] = NULL;
	FAIL_IF(execve(name, args, NULL) == -1);

	return 0;
}

static int exec_spr_check(void)
{
	unsigned long tar;

	asm __volatile__(
			"mfspr %[tar], 815;"
			: [tar] "=r" (tar)
			);
	/* Read TAR */
	FAIL_IF(tar != 0);

	return 0;
}

int main(int argc, char *argv[])
{
	SKIP_IF(!have_hwcap2(PPC_FEATURE2_TAR));
	name = argv[0];
	/* Do this a few times to be sure isn't a false negative */
	if (argc == 1 || atoi(argv[1]) < 10) {
		if (argc > 1)
			count = atoi(argv[1]);
		return test_harness(exec_spr, "spr_exec");
	} else {
		return test_harness(exec_spr_check, "spr_exec_check");
	}
}
