/*
 * Copyright (C) 2015, Anshuman Khandual, IBM Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation.
 */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <fcntl.h>

#include "utils.h"

#define HPAGE_OFF	0
#define HPAGE_ON	1

#define PAGE_SHIFT_4K	12
#define PAGE_SHIFT_64K	16
#define PAGE_SIZE_4K	0x1000
#define PAGE_SIZE_64K	0x10000
#define PAGE_SIZE_HUGE	16UL * 1024 * 1024

#define MEM_GB		1024UL * 1024 * 1024
#define MEM_MB		1024UL * 1024
#define MME_KB		1024UL

#define PMAP_FILE	"/proc/self/pagemap"
#define PMAP_PFN	0x007FFFFFFFFFFFFFUL
#define PMAP_SIZE	8

#define SOFT_OFFLINE	"/sys/devices/system/memory/soft_offline_page"
#define HARD_OFFLINE	"/sys/devices/system/memory/hard_offline_page"

#define MMAP_LENGTH	(256 * MEM_MB)
#define MMAP_ADDR	(void *)(0x0UL)
#define MMAP_PROT	(PROT_READ | PROT_WRITE)
#define MMAP_FLAGS	(MAP_PRIVATE | MAP_ANONYMOUS)
#define MMAP_FLAGS_HUGE	(MAP_SHARED)

#define FILE_NAME	"huge/hugepagefile"

static void write_buffer(char *addr, unsigned long length)
{
	unsigned long i;

	for (i = 0; i < length; i++)
		*(addr + i) = (char)i;
}

static int read_buffer(char *addr, unsigned long length)
{
	unsigned long i;

	for (i = 0; i < length; i++) {
		if (*(addr + i) != (char)i) {
			printf("Data miscompare at addr[%lu]\n", i);
			return 1;
		}
	}
	return 0;
}

static unsigned long get_npages(unsigned long length, unsigned long size)
{
	unsigned int tmp1 = length, tmp2 = size;

	return tmp1/tmp2;
}

static void soft_offline_pages(int hugepage, void *addr,
	unsigned long npages, unsigned long *skipped, unsigned long *failed)
{
	unsigned long psize, offset, pfn, paddr, fail, skip, i;
	void *tmp;
	int fd1, fd2;
	char buf[20];

	fd1 = open(PMAP_FILE, O_RDONLY);
	if (fd1 == -1) {
		perror("open() failed");
		exit(-1);
	}

	fd2 = open(SOFT_OFFLINE, O_WRONLY);
	if (fd2 == -1) {
		perror("open() failed");
		exit(-1);
	}

	fail = skip = 0;
	psize = getpagesize();
	for (i = 0; i < npages; i++) {
		if (hugepage)
			tmp = addr + i * PAGE_SIZE_HUGE;
		else
			tmp = addr + i * psize;

		offset = ((unsigned long) tmp / psize) * PMAP_SIZE;

		if (lseek(fd1, offset, SEEK_SET) == -1) {
			perror("lseek() failed");
			exit(-1);
		}

		if (read(fd1, &pfn, sizeof(pfn)) == -1) {
			perror("read() failed");
			exit(-1);
		}

		/* Skip if no valid PFN */
		pfn = pfn & PMAP_PFN;
		if (!pfn) {
			skip++;
			continue;
		}

		if (psize == PAGE_SIZE_4K)
			paddr = pfn << PAGE_SHIFT_4K;

		if (psize == PAGE_SIZE_64K)
			paddr = pfn << PAGE_SHIFT_64K;

		sprintf(buf, "0x%lx\n", paddr);

		if (write(fd2, buf, strlen(buf)) == -1) {
			perror("write() failed");
			printf("[%ld] PFN: %lx BUF: %s\n",i, pfn, buf);
			fail++;
		}

	}

	if (failed)
		*failed = fail;

	if (skipped)
		*skipped = skip;

	close(fd1);
	close(fd2);
}

int test_migration(unsigned long length)
{
	unsigned long skipped, failed;
	void *addr;
	int ret;

	addr = mmap(MMAP_ADDR, length, MMAP_PROT, MMAP_FLAGS, -1, 0);
	if (addr == MAP_FAILED) {
		perror("mmap() failed");
		exit(-1);
	}

	write_buffer(addr, length);
	soft_offline_pages(HPAGE_OFF, addr, length/getpagesize(), &skipped, &failed);
	ret = read_buffer(addr, length);

	printf("%ld moved %ld skipped %ld failed\n", (length/getpagesize() - skipped - failed), skipped, failed);

	munmap(addr, length);
	return ret;
}

int test_huge_migration(unsigned long length)
{
	unsigned long skipped, failed, npages;
	void *addr;
	int fd, ret;

	fd = open(FILE_NAME, O_CREAT | O_RDWR, 0755);
	if (fd < 0) {
		perror("open() failed");
		exit(-1);
	}

	addr = mmap(MMAP_ADDR, length, MMAP_PROT, MMAP_FLAGS_HUGE, fd, 0);
	if (addr == MAP_FAILED) {
		perror("mmap() failed");
		unlink(FILE_NAME);
		exit(-1);
	}

        if (mlock(addr, length) == -1) {
                perror("mlock() failed");
		munmap(addr, length);
                unlink(FILE_NAME);
                exit(-1);
        }

	write_buffer(addr, length);
	npages = get_npages(length, PAGE_SIZE_HUGE);
	soft_offline_pages(HPAGE_ON, addr, npages, &skipped, &failed);
	ret = read_buffer(addr, length);

	printf("%ld moved %ld skipped %ld failed\n", (npages - skipped - failed), skipped, failed);

	munmap(addr, length);
	unlink(FILE_NAME);
	return ret;
}
