/*
 * Copyright 2016, Cyril Bur, IBM Corp.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 *
 * Syscalls can be done provided the transactions are suspended. The
 * exec() class of syscall is unique as a new program is loaded.
 *
 * It makes little sence for after an exec() call for the previously
 * suspended transaction to still exist.
 *
 * This program also as by product confirms that a process exiting
 * with a suspended transaction doesn't do anything strange.
 */

#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "utils.h"
#include "tm.h"

int test_execed(void)
{
	SKIP_IF(!have_htm());

	asm __volatile__(
		"tbegin.;"
		"blt    1f;"
		"tsuspend.;"
		"1: ;"
		: : : "memory");

	FAIL_IF(failure_is_nesting());
	return 0;
}

int main(int argc, char *argv[])
{
	return test_harness(test_execed, "tm_execed");
}
