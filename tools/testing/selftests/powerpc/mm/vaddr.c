/*
 * Copyright 2017, Anshuman Khandual, IBM Corp.
 * Licensed under GPLv2.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <numaif.h>
#include <sys/mman.h>
#include <sys/time.h>
#include "utils.h"

#define PAGE_SIZE        65536UL  /* 64KB */
#define MAP_SIZE_16GB    262144UL /* 16GB  */
#define NR_SLICES_128TB  8192UL   /* 128TB */
#define NR_SLICES_384TB  24576UL  /* 384TB */
#define ADDR_MARK_128TB  0x800000000000UL /* Beyond 128TB */

static char *hind_addr(void)
{
	int bits = 48 + rand() % 15;

	return (char *) (1UL << bits);
}

static int validate_addr(char *ptr, int high_addr)
{
	unsigned long addr = (unsigned long) ptr;

	if (high_addr) {
		if (addr < ADDR_MARK_128TB) {
			printf("Bad address %lx\n", addr);
			return 1;
		}
		return 0;
	}

	if (addr > ADDR_MARK_128TB) {
		printf("Bad address %lx\n", addr);
		return 1;
	}
	return 0;
}

int vaddr(void)
{
	char *ptr[NR_SLICES_128TB];
	char *hptr[NR_SLICES_384TB];
	char *hint;
	int  i;

	for (i = 0; i < NR_SLICES_128TB; i++) {
		ptr[i] = mmap(NULL, MAP_SIZE_16GB, PROT_READ | PROT_WRITE,
					MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

		if (ptr[i] == MAP_FAILED)
			break;

		if (validate_addr(ptr[i], 0))
			return 1;
	}

	for (i = 0; i < NR_SLICES_384TB; i++) {
		hint = hind_addr();
		hptr[i] = mmap(hint, MAP_SIZE_16GB, PROT_READ | PROT_WRITE,
					MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

		if (hptr[i] == MAP_FAILED)
			break;

		if (validate_addr(hptr[i], 1))
			return 1;
	}

	for (i = 0; i < NR_SLICES_128TB; i++)
		munmap(ptr[i], MAP_SIZE_16GB);

	for (i = 0; i < NR_SLICES_384TB; i++)
		munmap(hptr[i], MAP_SIZE_16GB);
	return 0;
}

int main(int argc, char *argv[])
{
	return test_harness(vaddr, "vaddr-range");
}
