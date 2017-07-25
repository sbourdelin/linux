/*
 * Test to verify mirror functionality with mremap() system
 * call for shared anon mappings. The 'mirrored' buffer will
 * match element to element with that of the original one.
 *
 * Copyright (C) 2017 Anshuman Khandual, IBM Corporation
 *
 * Licensed under GPL V2
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/time.h>

#define PATTERN		0xbe
#define NR_PAGES	10

int test_mirror(char *old, char *new, unsigned long size)
{
	unsigned long i;

	for (i = 0; i < size; i++) {
		if (new[i] != old[i]) {
			printf("Mismatch at new[%lu] expected "
				"%d received %d\n", i, old[i], new[i]);
			return 1;
		}
	}
	return 0;
}

int main(int argc, char *argv[])
{
	unsigned long alloc_size;
	char *ptr, *mirror_ptr;

	alloc_size = sysconf(_SC_PAGESIZE) * NR_PAGES;
	ptr = mmap(NULL, alloc_size, PROT_READ | PROT_WRITE,
			MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (ptr == MAP_FAILED) {
		perror("map() failed");
		return -1;
	}
	memset(ptr, PATTERN, alloc_size);

	mirror_ptr =  (char *) mremap(ptr, 0, alloc_size, MREMAP_MAYMOVE);
	if (mirror_ptr == MAP_FAILED) {
		perror("mremap() failed");
		return -1;
	}

	if (test_mirror(ptr, mirror_ptr, alloc_size))
		return 1;
	return 0;
}
