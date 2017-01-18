/*
 * test_rodata.c: functional test for mark_rodata_ro function
 *
 * (C) Copyright 2017 Jinbum Park <jinb.park7@gmail.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; version 2
 * of the License.
 */
#include <asm/cacheflush.h>
#include <asm/sections.h>

int rodata_test(void)
{
	unsigned long result;
	unsigned long start, end;

	/* test 1: read the value */
	/* If this test fails, some previous testrun has clobbered the state */

	if (!rodata_test_data) {
		pr_err("rodata_test: test 1 fails (start data)\n");
		return -ENODEV;
	}

	/* test 2: write to the variable; this should fault */
	/*
	 * If this test fails, we managed to overwrite the data
	 *
	 * This is written in assembly to be able to catch the
	 * exception that is supposed to happen in the correct
	 * case
	*/

	result = 1;
	asm volatile(
		"0:	str %[zero], [%[rodata_test]]\n"
		"	mov %[rslt], %[zero]\n"
		"1:\n"
		".pushsection .text.fixup,\"ax\"\n"
		".align 2\n"
		"2:\n"
		"b 1b\n"
		".popsection\n"
		".pushsection __ex_table,\"a\"\n"
		".align 3\n"
		".long 0b, 2b\n"
		".popsection\n"
		: [rslt] "=r" (result)
		: [zero] "r" (0UL), [rodata_test] "r" (&rodata_test_data)
	);

	if (!result) {
		pr_err("rodata_test: test data was not read only\n");
		return -ENODEV;
	}

	/* test 3: check the value hasn't changed */
	/* If this test fails, we managed to overwrite the data */
	if (!rodata_test_data) {
		pr_err("rodata_test: Test 3 fails (end data)\n");
		return -ENODEV;
	}

	/* test 4: check if the rodata section is 4Kb aligned */
	start = (unsigned long)__start_rodata;
	end = (unsigned long)__end_rodata;
	if (start & (PAGE_SIZE - 1)) {
		pr_err("rodata_test: .rodata is not 4k aligned\n");
		return -ENODEV;
	}
	if (end & (PAGE_SIZE - 1)) {
		pr_err("rodata_test: .rodata end is not 4k aligned\n");
		return -ENODEV;
	}

	return 0;
}
