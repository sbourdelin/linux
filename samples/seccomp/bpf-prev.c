/*
 * Seccomp BPF example that uses information about the previous syscall.
 *
 * Copyright (C) 2015 TOSHIBA corp.
 * Author: Daniel Sangorrin <daniel.sangorrin@gmail.com>
 *
 * The code may be used by anyone for any purpose,
 * and can serve as a starting point for developing
 * applications using prctl or seccomp.
 */
#if defined(__x86_64__)
#define SUPPORTED_ARCH 1
#endif

#if defined(SUPPORTED_ARCH)
#define __USE_GNU 1
#define _GNU_SOURCE 1

#include <linux/filter.h>
/* NOTE: make sure seccomp_data in /usr/include/linux/seccomp.h has prev_nr */
#include <linux/seccomp.h>
#include <linux/unistd.h>
#include <stdio.h>
#include <stddef.h>
#include <sys/prctl.h>
#include <unistd.h>
#include <sys/msg.h>
#include <assert.h>

#define MSGPERM		0600
#define MTEXTSIZE	128
#define MTYPE		1

struct msg_buf {
	long mtype;
	char mtext[MTEXTSIZE];
};

#define syscall_nr (offsetof(struct seccomp_data, nr))
#define prev_nr (offsetof(struct seccomp_data, prev_nr))

#define EXAMINE_SYSCALL \
	BPF_STMT(BPF_LD+BPF_W+BPF_ABS, syscall_nr)

#define EXAMINE_PREV_SYSCALL \
	BPF_STMT(BPF_LD+BPF_W+BPF_ABS, prev_nr)

#define KILL_PROCESS \
	BPF_STMT(BPF_RET+BPF_K, SECCOMP_RET_KILL)

#ifndef PR_SET_NO_NEW_PRIVS
#define PR_SET_NO_NEW_PRIVS 38
#endif

#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))

static int install_syscall_filter(void)
{
	/* allow __NR_msgrcv only if prev_nr is __NR_prctl or __NR_msgsnd */
	struct sock_filter filter[] = {
		EXAMINE_SYSCALL,
		BPF_JUMP(BPF_JMP+BPF_JEQ+BPF_K, __NR_msgrcv, 1, 0),
		BPF_STMT(BPF_RET+BPF_K, SECCOMP_RET_ALLOW),
		EXAMINE_PREV_SYSCALL,
		BPF_JUMP(BPF_JMP+BPF_JEQ+BPF_K, __NR_prctl, 0, 1),
		BPF_STMT(BPF_RET+BPF_K, SECCOMP_RET_ALLOW),
		BPF_JUMP(BPF_JMP+BPF_JEQ+BPF_K, __NR_msgsnd, 0, 1),
		BPF_STMT(BPF_RET+BPF_K, SECCOMP_RET_ALLOW),
		BPF_JUMP(BPF_JMP+BPF_JEQ+BPF_K, __NR_clone, 0, 1),
		BPF_STMT(BPF_RET+BPF_K, SECCOMP_RET_ALLOW),
		KILL_PROCESS,
	};
	struct sock_fprog prog = {
		.len = ARRAY_SIZE(filter),
		.filter = filter,
	};

	if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0)) {
		perror("prctl(NO_NEW_PRIVS)");
		return 1;
	}

	if (prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog)) {
		perror("prctl(SECCOMP)");
		return 1;
	}
	return 0;
}

int main(int argc, char *argv[])
{
	long ret;
	int id;
	struct msg_buf send, recv;

	id = syscall(__NR_msgget, IPC_PRIVATE, MSGPERM | IPC_CREAT | IPC_EXCL);
	assert(id >= 0);

	send.mtype = MTYPE;
	snprintf(send.mtext, MTEXTSIZE, "hello");
	printf("parent msgsnd: %s\n", send.mtext);
	ret = syscall(__NR_msgsnd, id, &send, MTEXTSIZE, 0);
	assert(ret == 0);

	install_syscall_filter();

	/* TEST 1: msgrcv can be executed after prctl */
	ret = syscall(__NR_msgrcv, id, &recv, MTEXTSIZE, MTYPE, 0);
	assert(ret == MTEXTSIZE);
	printf("parent msgrcv after prctl: %s (%d bytes)\n", recv.mtext, ret);

	snprintf(send.mtext, MTEXTSIZE, "world");
	printf("parent msgsnd: %s\n", send.mtext);
	ret = syscall(__NR_msgsnd, id, &send, MTEXTSIZE, 0);
	assert(ret == 0);

	/* TEST 2: msgrcv can be executed after msgsnd */
	ret = syscall(__NR_msgrcv, id, &recv, MTEXTSIZE, MTYPE, 0);
	assert(ret == MTEXTSIZE);
	printf("parent msgrcv after msgsnd: %s (%d bytes)\n", recv.mtext, ret);

	snprintf(send.mtext, MTEXTSIZE, "this is mars");
	printf("parent msgsnd: %s\n", send.mtext);
	ret = syscall(__NR_msgsnd, id, &send, MTEXTSIZE, 0);
	assert(ret == 0);

	pid_t pid = fork();

	if (pid == 0) {
		/* TEST 3a: msgrcv can be executed after clone */
		ret = syscall(__NR_msgrcv, id, &recv, MTEXTSIZE, MTYPE, 0);
		assert(ret == MTEXTSIZE);
		printf("child msgrcv after clone: %s (%d bytes)\n",
		       recv.mtext, ret);
		_exit(0);
	} else if (pid > 0) {
		int status;

		pid = wait(&status);
		printf("parent: child %d exited with status %d\n", pid, status);
		/* TEST 3b: msgrcv can NOT be executed after write (dmseg) */
		syscall(__NR_write, STDOUT_FILENO, "Should fail: ", 14);
		syscall(__NR_msgrcv, id, &recv, MTEXTSIZE, MTYPE, 0);
		return 0;
	}

	assert(0); /* should never arrive here */

	return 0;
}
#else	/* SUPPORTED_ARCH */
/*
 * This sample has been tested on x86_64. Other architectures will result in
 * using only the main() below.
 */
int main(void)
{
	return 1;
}
#endif	/* SUPPORTED_ARCH */
