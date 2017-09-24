#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include "fdmap.h"

#define	BUF_SIZE	1024

long fdmap(pid_t pid, int *fds, size_t count, int start_fd, int flags)
{
	register int64_t r10 asm("r10") = start_fd;
	register int64_t r8 asm("r8") = flags;
	long ret;

	asm volatile (
		"syscall"
		: "=a"(ret)
		: "0" (333),
		  "D" (pid), "S" (fds), "d" (count), "r" (r10), "r" (r8)
		: "rcx", "r11", "cc", "memory"
	);
	return ret;
}

int fdmap_full(pid_t pid, int **fds, size_t *n)
{
	int buf[BUF_SIZE], start_fd = 0;
	long ret;

	*n = 0;
	*fds = NULL;
	for (;;) {
		int *new_buff;

		ret = fdmap(pid, buf, BUF_SIZE, start_fd, 0);
		if (ret < 0)
			break;
		if (!ret)
			return 0;

		new_buff = realloc(*fds, (*n + ret) * sizeof(int));
		if (!new_buff) {
			ret = -errno;
			break;
		}
		*fds = new_buff;
		memcpy(*fds + *n, buf, ret * sizeof(int));
		*n += ret;
		start_fd = (*fds)[*n - 1] + 1;
	}
	free(*fds);
	*fds = NULL;
	return -ret;
}

int fdmap_proc(pid_t pid, int **fds, size_t *n)
{
	char fds_path[20];
	int dir_fd = 0;
	struct dirent *fd_link;
	DIR *fds_dir;

	*fds = NULL;
	*n = 0;
	if (!pid)
		strcpy(fds_path, "/proc/self/fd");
	else
		sprintf(fds_path, "/proc/%d/fd", pid);

	fds_dir = opendir(fds_path);
	if (!fds_dir)
		return errno == ENOENT ? ESRCH : errno;
	if (!pid)
		dir_fd = dirfd(fds_dir);

	while ((fd_link = readdir(fds_dir))) {
		if (fd_link->d_name[0] < '0'
		    || fd_link->d_name[0] > '9')
			continue;
		if (*n % BUF_SIZE == 0) {
			int *new_buff;

			new_buff = realloc(*fds, (*n + BUF_SIZE) * sizeof(int));
			if (!new_buff) {
				int ret = errno;

				free(*fds);
				*fds = NULL;
				return ret;
			}
			*fds = new_buff;
		}
		(*fds)[*n] = atoi(fd_link->d_name);
		*n += 1;
	}
	closedir(fds_dir);

	if (!pid) {
		size_t i;

		for (i = 0; i < *n; i++)
			if ((*fds)[i] == dir_fd)
				break;
		i++;
		memmove(*fds + i - 1, *fds + i, (*n - i) * sizeof(int));
		(*n)--;
	}
	return 0;
}
