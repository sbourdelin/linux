/*
 * Test to verify mirror functionality with mremap() system
 * call for shared anon mappings.
 *
 * Copyright (C) 2017 Anshuman Khandual, IBM Corporation
 *
 * Licensed under GPL V2
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/time.h>

#define PATTERN		0xbe
#define ALLOC_SIZE	0x10000UL /* Works for 64K and 4K pages */

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
	char *ptr, *mirror_ptr;

	ptr = mmap(NULL, ALLOC_SIZE, PROT_READ | PROT_WRITE,
			MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (ptr == MAP_FAILED) {
		perror("map() failed");
		return -1;
	}
	memset(ptr, PATTERN, ALLOC_SIZE);

	mirror_ptr =  (char *) mremap(ptr, 0, ALLOC_SIZE, 1);
	if (mirror_ptr == MAP_FAILED) {
		perror("mremap() failed");
		return -1;
	}

	if (test_mirror(ptr, mirror_ptr, ALLOC_SIZE))
		return 1;
	return 0;
}
