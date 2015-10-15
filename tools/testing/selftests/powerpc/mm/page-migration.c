/*
 * Copyright (C) 2015, Anshuman Khandual, IBM Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation.
 */
#include "migration.h"

static int page_migration(void)
{
	int ret = 0;

	if ((unsigned long)getpagesize() == 0x1000)
		printf("Running on base page size 4K\n");

	if ((unsigned long)getpagesize() == 0x10000)
		printf("Running on base page size 64K\n");

	ret = test_migration(4 * MEM_MB);
	ret = test_migration(64 * MEM_MB);
	ret = test_migration(256 * MEM_MB);
	ret = test_migration(512 * MEM_MB);
	ret = test_migration(1 * MEM_GB);
	ret = test_migration(2 * MEM_GB);

	return ret;
}

int main(void)
{
	return test_harness(page_migration, "page_migration");
}
