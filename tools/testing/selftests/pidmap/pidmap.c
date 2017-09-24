#define _GNU_SOURCE
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <stdlib.h>
#include <sched.h>
#include <dirent.h>
#include <string.h>
#include <sys/mount.h>
#include <signal.h>
#include <assert.h>
#include "pidmap.h"
#include "../kselftest_harness.h"

#define SIZE 512

static inline long pidmap(pid_t pid, int *pids, unsigned int count,
			  unsigned int start_pid, int flags)
{
	long ret;

	register long r10 asm("r10") = start_pid;
	register long r8 asm("r8") = flags;

	asm volatile ("syscall" : "=a"(ret) :
		"0"(334), "D"(pid), "S"(pids), "d"(count), "r"(r10), "r"(r8) :
		"rcx", "r11", "cc", "memory");
	return ret;
}

static int compare(const void *a, const void *b)
{
	return *((int *)a) > *((int *)b);
}

int pidmap_full(int **pid, unsigned int *res_count)
{
	int n;
	int start_pid = 1;
	*pid = (int *)malloc(SIZE * sizeof(int));
	*res_count = 0;

	while ((n = pidmap(0, *pid + *res_count, SIZE, start_pid,
			   PIDMAP_TASKS)) > 0) {
		*res_count += n;
		*pid = (int *)realloc(*pid, (*res_count + SIZE) * sizeof(int));
		start_pid = (*pid)[*res_count - 1] + 1;
	}
	return n;
}

int pidmap_proc(int **pid, unsigned int *n)
{
	DIR *dir = opendir("/proc");
	struct dirent *dirs;

	*n = 0;
	*pid = NULL;

	while ((dirs = readdir(dir))) {
		char dname[32] = "";
		DIR *task_dir;

		if (dirs->d_name[0] < '0' || dirs->d_name[0] > '9')
			continue;

		strcpy(dname, "/proc/");
		strcat(dname, dirs->d_name);
		strcat(dname, "/task");
		task_dir = opendir(dname);

		if (task_dir) {
			struct dirent *task_dirs;

			while ((task_dirs = readdir(task_dir))) {
				if (task_dirs->d_name[0] < '0' ||
						task_dirs->d_name[0] > '9')
					continue;

				*pid = (int *)realloc(*pid, (*n + 1) *
								sizeof(int));
				if (*pid == NULL)
					return -1;
				*(*pid + *n) = atoi(task_dirs->d_name);
				*n += 1;
			}
		} else {
			*pid = (int *)realloc(*pid, (*n + 1) * sizeof(int));
			if (*pid == NULL)
				return -1;
			*(*pid + *n) = atoi(dirs->d_name);
			*n += 1;
		}
		closedir(task_dir);
	}
	closedir(dir);
	return 0;
}

TEST(bufsize)
{
	int pid[SIZE];

	EXPECT_EQ(0, pidmap(0, pid, 0, 1, PIDMAP_TASKS));
}

TEST(get_pid)
{
	int pid;
	int ret;

	ret = pidmap(0, &pid, 1, getpid(), PIDMAP_TASKS);
	ASSERT_LE(0, ret);
	EXPECT_EQ(getpid(), pid);
}

TEST(bad_start)
{
	int pid[SIZE];

	ASSERT_LE(0, pidmap(0, pid, SIZE, -1, PIDMAP_TASKS));
	ASSERT_LE(0, pidmap(0, pid, SIZE, ~0U, PIDMAP_TASKS));
	ASSERT_LE(0, pidmap(0, pid, SIZE, 0, PIDMAP_TASKS));
	EXPECT_EQ(1, pid[0]);
}

TEST(child_pid)
{
	pid_t pid = fork();

	if (pid == 0)
		pause();
	else {
		int ret;
		int result = 0;

		ret = pidmap(0, &result, 1, pid, PIDMAP_TASKS);
		EXPECT_LE(0, ret);
		EXPECT_EQ(pid, result);
		kill(pid, SIGTERM);
	}
}

TEST(pidmap_children_flag)
{
	int real_pids[SIZE], pids[SIZE];
	int i;

	for (i = 0; i < SIZE; i++) {
		pid_t pid = fork();
		if (!pid) {
			pause();
			exit(0);
		} else if (pid < 0) {
			perror("fork");
			exit(1);
		}
		real_pids[i] = pid;
	}

	ASSERT_EQ(SIZE, pidmap(0, pids, SIZE, 0, PIDMAP_CHILDREN));
	for (i = 0; i < SIZE; i++) {
		ASSERT_EQ(real_pids[i], pids[i]);
		kill(real_pids[i], SIGKILL);
	}
}

int write_pidmax(int new_pidmax)
{
	char old_pidmax[32];
	char new[32];
	int fd = open("/proc/sys/kernel/pid_max", O_RDWR);

	if (read(fd, old_pidmax, 32) <= 0)
		printf("Read failed\n");
	lseek(fd, 0, 0);
	snprintf(new, sizeof(new), "%d", new_pidmax);
	if (write(fd, new, strlen(new)) <= 0)
		printf("Write failed\n");
	close(fd);
	return atoi(old_pidmax);
}

void do_forks(unsigned int n)
{
	while (n--) {
		pid_t pid = fork();

		if (pid == 0)
			exit(0);
		waitpid(pid, NULL, 0);
	}
}

TEST(pid_max)
{
	int *pid;
	unsigned int n;
	int ret, p;
	int old_pidmax;

	old_pidmax = write_pidmax(50000);

	do_forks(40000);

	p = fork();

	if (p == 0)
		pause();

	ret = pidmap_full(&pid, &n);
	kill(p, SIGKILL);

	EXPECT_LE(0, ret);
	EXPECT_LE(1, n);
	if (ret < 0 || n <= 0)
		goto exit;
	EXPECT_EQ(p, pid[n - 1]);
exit:
	write_pidmax(old_pidmax);
}

void sigquit_h(int sig)
{
	assert(sig == SIGQUIT);
	if (getgid() != getpid())
		exit(0);
}

TEST(compare_proc)
{
	pid_t pid;

	if (unshare(CLONE_NEWNS | CLONE_NEWPID) == -1)
		return;

	pid = fork();

	if (pid == 0) {
		pid_t p;
		int i = 0;

		signal(SIGQUIT, sigquit_h);

		mount("none", "/", NULL, MS_REC | MS_PRIVATE, NULL);
		mount("none", "/proc", NULL, MS_REC | MS_PRIVATE, NULL);
		mount("proc", "/proc", "proc",
			MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL);

		while (i < 150) {
			i++;

			p = fork();

			if (p == -1) {
				umount("/proc");
				return;
			}
			if (p == 0) {
				pause();
				return;
			}
		}

		int *pids, *pids_proc;
		unsigned int n = 0;
		unsigned int n_proc = 0;
		int ret, ret_proc;

		ret = pidmap_full(&pids, &n);

		ret_proc = pidmap_proc(&pids_proc, &n_proc);
		qsort(pids_proc, n_proc, sizeof(int), compare);

		EXPECT_LE(0, ret);
		if (ret < 0 || ret_proc < 0)
			goto exit;

		EXPECT_EQ(n_proc, n);
		if (n != n_proc)
			goto exit;

		for (int i = 0; i < n; i++) {
			EXPECT_EQ(pids_proc[i], pids[i]);
			if (pids_proc[i] != pids[i])
				break;
		}
exit:
		free(pids_proc);
		free(pids);
		umount("/proc");
		kill(-getpid(), SIGQUIT);
	}
	wait(NULL);
}

TEST_HARNESS_MAIN
