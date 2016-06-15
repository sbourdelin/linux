/*
 * Copyright 2016, Cyril Bur, IBM Corp.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 *
 * Syscalls can be performed provided the transactions are suspended.
 * The exec() class of syscall is unique as a new process is loaded.
 *
 * It makes little sense for after an exec() call for the previously
 * suspended transaction to still exist.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <inttypes.h>
#include <libgen.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "utils.h"
#include "tm.h"

static char *path;

int test_exec(void)
{
	char *file;

	SKIP_IF(!have_htm());

	FAIL_IF(asprintf(&file, "%s/%s", path, "tm-execed") == -1);

	asm __volatile__(
		"tbegin.;"
		"blt    1f; "
		"tsuspend.;"
		"1: ;"
		: : : "memory");

	execl(file, "tm-execed", NULL);
	/* Shouldn't get here */
	perror("execl() failed");
	return 1;
}

int main(int argc, char *argv[])
{
	path = dirname(argv[0]);
	return test_harness(test_exec, "tm_exec");
}
