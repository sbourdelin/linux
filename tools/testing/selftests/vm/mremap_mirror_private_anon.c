/*
 * Test to verify mirror functionality with mremap() system
 * call for private anon mappings. The 'mirrored' buffer is
 * a separate distinct unrelated mapping and different from
 * that of the original one.
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

int main(int argc, char *argv[])
{
	unsigned long alloc_size, i;
	char *ptr, *mirror_ptr;

	alloc_size = sysconf(_SC_PAGESIZE) * NR_PAGES;
	ptr = mmap(NULL, alloc_size, PROT_READ | PROT_WRITE,
			MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
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

	for (i = 0; i < alloc_size; i++) {
		if (ptr[i] == mirror_ptr[i]) {
			printf("Mirror buffer elements matched at %lu\n", i);
			return 1;
		}
	}
	return 0;
}
