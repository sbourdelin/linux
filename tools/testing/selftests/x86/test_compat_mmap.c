/*
 * Check that compat 32-bit mmap() returns address < 4Gb on 64-bit.
 *
 * Copyright (c) 2017 Dmitry Safonov (Virtuozzo)
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
#include <sys/mman.h>
#include <sys/types.h>

#include <stdio.h>
#include <unistd.h>
#include <stdint.h>
#include <signal.h>
#include <stdlib.h>

#define PAGE_SIZE 4096
#define MMAP_SIZE (PAGE_SIZE*1024)
#define MAX_VMAS 50
#define BUF_SIZE 1024

#ifndef __NR32_mmap2
#define __NR32_mmap2 192
#endif

struct syscall_args32 {
	uint32_t nr, arg0, arg1, arg2, arg3, arg4, arg5;
};

static void do_full_int80(struct syscall_args32 *args)
{
	asm volatile ("int $0x80"
		      : "+a" (args->nr),
			"+b" (args->arg0), "+c" (args->arg1), "+d" (args->arg2),
			"+S" (args->arg3), "+D" (args->arg4),
			"+rbp" (args->arg5)
			: : "r8", "r9", "r10", "r11");
}

void *mmap2(void *addr, size_t len, int prot, int flags,
	int fildes, off_t off)
{
	struct syscall_args32 s;

	s.nr	= __NR32_mmap2;
	s.arg0	= (uint32_t)(uintptr_t)addr;
	s.arg1	= (uint32_t)len;
	s.arg2	= prot;
	s.arg3	= flags;
	s.arg4	= fildes;
	s.arg5	= (uint32_t)off;

	do_full_int80(&s);

	return (void *)(uintptr_t)s.nr;
}

struct vm_area {
	unsigned long start;
	unsigned long end;
};

static struct vm_area vmas_before_mmap[MAX_VMAS];
static struct vm_area vmas_after_mmap[MAX_VMAS];

static char buf[BUF_SIZE];

int parse_maps(struct vm_area *vmas)
{
	FILE *maps;
	int i;

	maps = fopen("/proc/self/maps", "r");
	if (maps == NULL) {
		printf("[ERROR]\tFailed to open maps file: %m\n");
		return -1;
	}

	for (i = 0; i < MAX_VMAS; i++) {
		struct vm_area *v = &vmas[i];
		char *end;

		if (fgets(buf, BUF_SIZE, maps) == NULL)
			break;

		v->start = strtoul(buf, &end, 16);
		v->end = strtoul(end + 1, NULL, 16);
		//printf("[NOTE]\tVMA: [%#lx, %#lx]\n", v->start, v->end);
	}

	if (i == MAX_VMAS) {
		printf("[ERROR]\tNumber of VMAs is bigger than reserved array's size\n");
		return -1;
	}

	if (fclose(maps)) {
		printf("[ERROR]\tFailed to close maps file: %m\n");
		return -1;
	}
	return 0;
}

int compare_vmas(struct vm_area *vmax, struct vm_area *vmay)
{
	if (vmax->start > vmay->start)
		return 1;
	if (vmax->start < vmay->start)
		return -1;
	if (vmax->end > vmay->end)
		return 1;
	if (vmax->end < vmay->end)
		return -1;
	return 0;
}

unsigned long vma_size(struct vm_area *v)
{
	return v->end - v->start;
}

int find_new_vma_like(struct vm_area *vma)
{
	int i, j = 0, found_alike = -1;

	for (i = 0; i < MAX_VMAS && j < MAX_VMAS; i++, j++) {
		int cmp = compare_vmas(&vmas_before_mmap[i],
				&vmas_after_mmap[j]);

		if (cmp == 0)
			continue;
		if (cmp < 0) {/* Lost mapping */
			printf("[NOTE]\tLost mapping: %#lx\n",
				vmas_before_mmap[i].start);
			j--;
			continue;
		}

		printf("[NOTE]\tNew mapping appeared: %#lx\n",
				vmas_after_mmap[j].start);
		i--;
		if (!compare_vmas(&vmas_after_mmap[j], vma))
			return 0;

		if (((vmas_after_mmap[j].start & 0xffffffff) == vma->start) &&
				(vma_size(&vmas_after_mmap[j]) == vma_size(vma)))
			found_alike = j;
	}

	/* Left new vmas in tail */
	for (; i < MAX_VMAS; i++)
		if (!compare_vmas(&vmas_after_mmap[j], vma))
			return 0;

	if (found_alike != -1) {
		printf("[FAIL]\tFound VMA [%#lx, %#lx] in maps file, that was allocated with compat syscall\n",
			vmas_after_mmap[found_alike].start,
			vmas_after_mmap[found_alike].end);
		return -1;
	}

	printf("[ERROR]\tCan't find [%#lx, %#lx] in maps file\n",
		vma->start, vma->end);
	return -1;
}

int main(int argc, char **argv)
{
	void *map;
	struct vm_area vma;

	if (parse_maps(vmas_before_mmap)) {
		printf("[ERROR]\tFailed to parse maps file\n");
		return 1;
	}

	map = mmap2(0, MMAP_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC,
			MAP_PRIVATE | MAP_ANON, -1, 0);
	if (((uintptr_t)map) % PAGE_SIZE) {
		printf("[ERROR]\tmmap2 failed: %d\n",
				(~(uint32_t)(uintptr_t)map) + 1);
		return 1;
	} else {
		printf("[NOTE]\tAllocated mmap %p, sized %#x\n", map, MMAP_SIZE);
	}

	if (parse_maps(vmas_after_mmap)) {
		printf("[ERROR]\tFailed to parse maps file\n");
		return 1;
	}

	munmap(map, MMAP_SIZE);

	vma.start = (unsigned long)(uintptr_t)map;
	vma.end = vma.start + MMAP_SIZE;
	if (find_new_vma_like(&vma))
		return 1;

	printf("[OK]\n");

	return 0;
}
