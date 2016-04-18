/*
 * 32-bit test to check vdso mremap.
 *
 * Copyright (c) 2016 Dmitry Safonov
 * Suggested-by: Andrew Lutomirski
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */
/*
 * Can be built statically:
 * gcc -Os -Wall -static -m32 test_mremap_vdso.c
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>

#include <sys/mman.h>
#include <sys/auxv.h>
#include <sys/syscall.h>

#if !defined(__i386__)
int main(int argc, char **argv, char **envp)
{
	printf("[SKIP]\tNot a 32-bit x86 userspace\n");
	return 0;
}
#else

#define PAGE_SIZE	4096
#define VDSO_SIZE	PAGE_SIZE

int main(int argc, char **argv, char **envp)
{
	unsigned long vdso_addr, dest_addr;
	void *new_addr;
	const char *ok_string = "[OK]\n";

	vdso_addr = getauxval(AT_SYSINFO_EHDR);
	printf("\tAT_SYSINFO_EHDR is 0x%lx\n", vdso_addr);
	if (!vdso_addr || vdso_addr == -ENOENT) {
		printf("[FAIL]\tgetauxval failed\n");
		return 1;
	}

	/* to low for stack, to high for lib/data/code mappings */
	dest_addr = 0x0a000000;
	printf("[NOTE]\tMoving vDSO: [%lx, %lx] -> [%lx, %lx]\n",
		vdso_addr, vdso_addr + VDSO_SIZE,
		dest_addr, dest_addr + VDSO_SIZE);
	new_addr = mremap((void *)vdso_addr, VDSO_SIZE, VDSO_SIZE,
			MREMAP_FIXED|MREMAP_MAYMOVE, dest_addr);
	if ((unsigned long)new_addr == (unsigned long)-1) {
		printf("[FAIL]\tmremap failed (%d): %m\n", errno);
		return 1;
	}

	asm volatile ("int $0x80" : : "a" (__NR_write), "b" (STDOUT_FILENO),
			"c" (ok_string), "d" (strlen(ok_string)));
	asm volatile ("int $0x80" : : "a" (__NR_exit), "b" (0));

	return 0;
}
#endif
