#include <errno.h>
#include <syscall.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <limits.h>
#include "../kselftest_harness.h"
#include "fdmap.h"

TEST(efault) {
	int ret;

	ret = syscall(333, 0, NULL, 20 * sizeof(int), 0, 0);
	ASSERT_EQ(-1, ret);
	ASSERT_EQ(EFAULT, errno);
}

TEST(big_start_fd) {
	int fds[1];
	int ret;

	ret = syscall(333, 0, fds, sizeof(int), INT_MAX, 0);
	ASSERT_EQ(0, ret);
}

TEST(einval) {
	int ret;

	ret = syscall(333, 0, NULL, 0, -1, 0);
	ASSERT_EQ(-1, ret);
	ASSERT_EQ(EINVAL, errno);

	ret = syscall(333, 0, NULL, 0, 0, 1);
	ASSERT_EQ(-1, ret);
	ASSERT_EQ(EINVAL, errno);
}

TEST(esrch) {
	int fds[1], ret;
	pid_t pid;

	pid = fork();
	ASSERT_NE(-1, pid);
	if (!pid)
		exit(0);
	waitpid(pid, NULL, 0);

	ret = syscall(333, pid, fds, sizeof(int), 0, 0);
	ASSERT_EQ(-1, ret);
	ASSERT_EQ(ESRCH, errno);
}

TEST(simple) {
	int *fds1, *fds2;
	size_t size1, size2, i;
	int ret1, ret2;

	ret1 = fdmap_full(0, &fds1, &size1);
	ret2 = fdmap_proc(0, &fds2, &size2);
	ASSERT_EQ(ret2, ret1);
	ASSERT_EQ(size2, size1);
	for (i = 0; i < size1; i++)
		ASSERT_EQ(fds2[i], fds1[i]);
	free(fds1);
	free(fds2);
}

TEST(init) {
	int *fds1, *fds2;
	size_t size1, size2, i;
	int ret1, ret2;

	ret1 = fdmap_full(1, &fds1, &size1);
	ret2 = fdmap_proc(1, &fds2, &size2);
	ASSERT_EQ(ret2, ret1);
	ASSERT_EQ(size2, size1);
	for (i = 0; i < size1; i++)
		ASSERT_EQ(fds2[i], fds1[i]);
	free(fds1);
	free(fds2);
}

TEST(zero) {
	int *fds, i;
	size_t size;
	int ret;

	ret = fdmap_proc(0, &fds, &size);
	ASSERT_EQ(0, ret);
	for (i = 0; i < size; i++)
		close(fds[i]);
	free(fds);
	fds = NULL;

	ret = fdmap_full(0, &fds, &size);
	ASSERT_EQ(0, ret);
	ASSERT_EQ(0, size);
}

TEST(more_fds) {
	int *fds1, *fds2, ret1, ret2;
	size_t size1, size2, i;

	struct rlimit rlim = {
		.rlim_cur = 600000,
		.rlim_max = 600000
	};
	ASSERT_EQ(0, setrlimit(RLIMIT_NOFILE, &rlim));
	for (int i = 0; i < 500000; i++)
		dup(0);

	ret1 = fdmap_full(0, &fds1, &size1);
	ret2 = fdmap_proc(0, &fds2, &size2);
	ASSERT_EQ(ret2, ret1);
	ASSERT_EQ(size2, size1);
	for (i = 0; i < size1; i++)
		ASSERT_EQ(fds2[i], fds1[i]);
	free(fds1);
	free(fds2);
}

TEST(child) {
	int pipefd[2];
	int *fds1, *fds2, ret1, ret2, i;
	size_t size1, size2;
	char byte = 0;
	pid_t pid;

	ASSERT_NE(-1, pipe(pipefd));
	pid = fork();
	ASSERT_NE(-1, pid);
	if (!pid) {
		read(pipefd[0], &byte, 1);
		close(pipefd[0]);
		close(pipefd[1]);
		exit(0);
	}

	ret1 = fdmap_full(0, &fds1, &size1);
	ret2 = fdmap_proc(0, &fds2, &size2);
	ASSERT_EQ(ret2, ret1);
	ASSERT_EQ(size2, size1);
	for (i = 0; i < size1; i++)
		ASSERT_EQ(fds2[i], fds1[i]);
	free(fds1);
	free(fds2);

	write(pipefd[1], &byte, 1);
	close(pipefd[0]);
	close(pipefd[1]);
	waitpid(pid, NULL, 0);
}

TEST_HARNESS_MAIN
