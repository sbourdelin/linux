/*
 * Ptrace interface test helper functions
 *
 * Copyright (C) 2015 Anshuman Khandual, IBM Corporation.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */
#include <inttypes.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <errno.h>
#include <time.h>
#include <sys/ptrace.h>
#include <sys/ioctl.h>
#include <sys/uio.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/signal.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/user.h>
#include <linux/elf.h>
#include <linux/types.h>
#include <linux/auxvec.h>
#include "reg.h"
#include "utils.h"

#define TEST_PASS 0
#define TEST_FAIL 1

struct ebb_regs {
	unsigned long	ebbrr;
	unsigned long	ebbhr;
	unsigned long	bescr;
};

struct pmu_regs {
	unsigned long	siar;
	unsigned long	sdar;
	unsigned long	sier;
	unsigned long	mmcr2;
	unsigned long	mmcr0;
};

struct fpr_regs {
	unsigned long fpr[32];
	unsigned long fpscr;
};


/* Basic ptrace operations */
int start_trace(pid_t child)
{
	int ret;

	ret = ptrace(PTRACE_ATTACH, child, NULL, NULL);
	if (ret) {
		perror("ptrace(PTRACE_ATTACH) failed");
		return TEST_FAIL;
	}
	ret = waitpid(child, NULL, 0);
	if (ret != child) {
		perror("waitpid() failed");
		return TEST_FAIL;
	}
	return TEST_PASS;
}

int stop_trace(pid_t child)
{
	int ret;

	ret = ptrace(PTRACE_DETACH, child, NULL, NULL);
	if (ret) {
		perror("ptrace(PTRACE_DETACH) failed");
		return TEST_FAIL;
	}
	return TEST_PASS;
}

int cont_trace(pid_t child)
{
	int ret;

	ret = ptrace(PTRACE_CONT, child, NULL, NULL);
	if (ret) {
		perror("ptrace(PTRACE_CONT) failed");
		return TEST_FAIL;
	}
	return TEST_PASS;
}

/* PMU */
int show_pmu_registers(pid_t child, struct pmu_regs *regs)
{
	struct pmu_regs *pmu;
	struct iovec iov;
	int ret;

	pmu = malloc(sizeof(struct pmu_regs));
	if (!pmu) {
		perror("malloc() failed");
		return TEST_FAIL;
	}

	iov.iov_base = (struct pmu_regs *) pmu;
	iov.iov_len = sizeof(struct pmu_regs);
	ret = ptrace(PTRACE_GETREGSET, child, NT_PPC_PMU, &iov);
	if (ret) {
		perror("ptrace(PTRACE_GETREGSET) failed");
		goto fail;
	}

	if (regs)
		memcpy(regs, pmu, sizeof(struct pmu_regs));

	free(pmu);
	return TEST_PASS;
fail:
	free(pmu);
	return TEST_FAIL;
}

/* EBB */
int show_ebb_registers(pid_t child, struct ebb_regs *regs)
{
	struct ebb_regs *ebb;
	struct iovec iov;
	int ret;

	ebb = malloc(sizeof(struct ebb_regs));
	if (!ebb) {
		perror("malloc() failed");
		return TEST_FAIL;
	}

	iov.iov_base = (struct ebb_regs *) ebb;
	iov.iov_len = sizeof(struct ebb_regs);
	ret = ptrace(PTRACE_GETREGSET, child, NT_PPC_EBB, &iov);
	if (ret) {
		perror("ptrace(PTRACE_GETREGSET) failed");
		goto fail;
	}

	if (regs)
		memcpy(regs, ebb, sizeof(struct ebb_regs));

	free(ebb);
	return TEST_PASS;
fail:
	free(ebb);
	return TEST_FAIL;
}

/* Analyse TEXASR after TM failure */
inline unsigned long get_tfiar(void)
{
	unsigned long ret;

	asm volatile("mfspr %0,%1" : "=r" (ret) : "i" (SPRN_TFIAR));
	return ret;
}

void analyse_texasr(unsigned long texasr)
{
	printf("TEXASR: %16lx\t", texasr);

	if (texasr & TEXASR_FP)
		printf("TEXASR_FP  ");

	if (texasr & TEXASR_DA)
		printf("TEXASR_DA  ");

	if (texasr & TEXASR_NO)
		printf("TEXASR_NO  ");

	if (texasr & TEXASR_FO)
		printf("TEXASR_FO  ");

	if (texasr & TEXASR_SIC)
		printf("TEXASR_SIC  ");

	if (texasr & TEXASR_NTC)
		printf("TEXASR_NTC  ");

	if (texasr & TEXASR_TC)
		printf("TEXASR_TC  ");

	if (texasr & TEXASR_TIC)
		printf("TEXASR_TIC  ");

	if (texasr & TEXASR_IC)
		printf("TEXASR_IC  ");

	if (texasr & TEXASR_IFC)
		printf("TEXASR_IFC  ");

	if (texasr & TEXASR_ABT)
		printf("TEXASR_ABT  ");

	if (texasr & TEXASR_SPD)
		printf("TEXASR_SPD  ");

	if (texasr & TEXASR_HV)
		printf("TEXASR_HV  ");

	if (texasr & TEXASR_PR)
		printf("TEXASR_PR  ");

	if (texasr & TEXASR_FS)
		printf("TEXASR_FS  ");

	if (texasr & TEXASR_TE)
		printf("TEXASR_TE  ");

	if (texasr & TEXASR_ROT)
		printf("TEXASR_ROT  ");

	printf("TFIAR :%lx\n", get_tfiar());
}
