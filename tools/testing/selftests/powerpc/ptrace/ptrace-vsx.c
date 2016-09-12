/*
 * Ptrace test for VMX/VSX registers
 *
 * Copyright (C) 2015 Anshuman Khandual, IBM Corporation.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */
#include "ptrace.h"
#include "ptrace-vsx.h"

/* Tracer and Tracee Shared Data */
int shm_id;
volatile int *cptr, *pptr;

unsigned long fp_load[VEC_MAX];
unsigned long fp_load_new[VEC_MAX];
unsigned long fp_store[VEC_MAX];

void vsx(void)
{
	int ret;

	cptr = (int *)shmat(shm_id, NULL, 0);
	loadvsx(fp_load, 0);
	cptr[1] = 1;

	while (!cptr[0]);
	shmdt((void *) cptr);

	storevsx(fp_store, 0);
	ret = compare_vsx_vmx(fp_store, fp_load_new);
	if (ret)
		exit(1);
	exit(0);
}

int trace_vsx(pid_t child)
{
	unsigned long vsx[VSX_MAX];
	unsigned long vmx[VMX_MAX + 2][2];
	int ret;

	ret = start_trace(child);
	if (ret)
		return TEST_FAIL;

	ret = show_vsx(child, vsx);
	if (ret)
		return TEST_FAIL;

	ret = validate_vsx(vsx, fp_load);
	if (ret)
		return TEST_FAIL;

	ret = show_vmx(child, vmx);
	if (ret)
		return TEST_FAIL;

	ret = validate_vmx(vmx, fp_load);
	if (ret)
		return TEST_FAIL;

	memset(vsx, 0, sizeof(vsx));
	memset(vmx, 0, sizeof(vmx));
	load_vsx_vmx(fp_load_new, vsx, vmx);

	ret = write_vsx(child, vsx);
	if (ret)
		return TEST_FAIL;

	ret = write_vmx(child, vmx);
	if (ret)
		return TEST_FAIL;

	ret = stop_trace(child);
	if (ret)
		return TEST_FAIL;

	return TEST_PASS;
}

int ptrace_vsx(void)
{
	pid_t pid;
	int ret, status, i;

	shm_id = shmget(IPC_PRIVATE, sizeof(int) * 2, 0777|IPC_CREAT);

	for (i = 0; i < VEC_MAX; i++)
		fp_load[i] = i + rand();

	for (i = 0; i < VEC_MAX; i++)
		fp_load_new[i] = i + 2 * rand();

	pid = fork();
	if (pid < 0) {
		perror("fork() failed");
		return TEST_FAIL;
	}

	if (pid == 0)
		vsx();

	if (pid) {
		pptr = (int *)shmat(shm_id, NULL, 0);
		while (!pptr[1]);

		ret = trace_vsx(pid);
		if (ret) {
			kill(pid, SIGTERM);
			shmdt((void *)pptr);
			shmctl(shm_id, IPC_RMID, NULL);
			return TEST_FAIL;
		}

		pptr[0] = 1;
		shmdt((void *)pptr);

		ret = wait(&status);
		shmctl(shm_id, IPC_RMID, NULL);
		if (ret != pid) {
			printf("Child's exit status not captured\n");
			return TEST_FAIL;
		}

		if (WIFEXITED(status)) {
			if (WEXITSTATUS(status))
				return TEST_FAIL;
		}
	}
	return TEST_PASS;
}

int main(int argc, char *argv[])
{
	return test_harness(ptrace_vsx, "ptrace_vsx");
}
