/*
 * Copyright (C) 2015, Anshuman Khandual, IBM Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation.
 */
#include "migration.h"

static int hugepage_migration(void)
{
	int ret = 0;

	if ((unsigned long)getpagesize() == 0x1000)
		printf("Running on base page size 4K\n");

	if ((unsigned long)getpagesize() == 0x10000)
		printf("Running on base page size 64K\n");

	ret = test_huge_migration(16 * MEM_MB);
	ret = test_huge_migration(256 * MEM_MB);
	ret = test_huge_migration(512 * MEM_MB);

	return ret;
}

int main(void)
{
	return test_harness(hugepage_migration, "hugepage_migration");
}
