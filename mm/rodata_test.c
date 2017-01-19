/*
 * rodata_test.c: functional test for mark_rodata_ro function
 *
 * (C) Copyright 2017 Jinbum Park <jinb.park7@gmail.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; version 2
 * of the License.
 */
#include <asm/uaccess.h>
#include <asm/sections.h>

const int rodata_test_data = 0xC3;
EXPORT_SYMBOL_GPL(rodata_test_data);

void rodata_test(void)
{
	unsigned long start, end, rodata_addr;
	int zero = 0;

	/* prepare test */
	rodata_addr = ((unsigned long)&rodata_test_data);

	/* test 1: read the value */
	/* If this test fails, some previous testrun has clobbered the state */
	if (!rodata_test_data) {
		pr_err("rodata_test: test 1 fails (start data)\n");
		return;
	}

	/* test 2: write to the variable; this should fault */
	/*
	 * This must be written in assembly to be able to catch the
	 * exception that is supposed to happen in the correct case.
	 *
	 * So that put_user macro is used to write arch-independent assembly.
	 */
	if (!put_user(zero, (int *)rodata_addr)) {
		pr_err("rodata_test: test data was not read only\n");
		return;
	}

	/* test 3: check the value hasn't changed */
	if (rodata_test_data == zero) {
		pr_err("rodata_test: test data was changed\n");
		return;
	}

	/* test 4: check if the rodata section is PAGE_SIZE aligned */
	start = (unsigned long)__start_rodata;
	end = (unsigned long)__end_rodata;
	if (start & (PAGE_SIZE - 1)) {
		pr_err("rodata_test: start of .rodata is not page size aligned\n");
		return;
	}
	if (end & (PAGE_SIZE - 1)) {
		pr_err("rodata_test: end of .rodata is not page size aligned\n");
		return;
	}

	pr_info("rodata_test: all tests were successful\n");
}
