#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <syscall.h>
#include <linux/unistd.h>

#ifndef UMASK_GET_MASK
#define UMASK_GET_MASK 1
#endif

static int umask2_(int mask, int flags)
{
#ifdef __NR_umask2
	return syscall(__NR_umask2, mask, flags);
#else
	errno = ENOSYS;
	return -1;
#endif
}

int main(int argc, char **argv)
{
	int r;

	/* umask2 available in current kernel? */
	r = umask2_(0, UMASK_GET_MASK);
	if (r == -1 && errno == ENOSYS) {
		fprintf(stderr,
			"umask2 not available in current kernel or headers, skipping test\n");
		exit(0);
	}

	/* Check that old umask still works. */
	r = umask(022);
	if (r == -1) {
		perror("umask");
		exit(1);
	}

	/* Call umask2 to emulate old umask. */
	r = umask2_(023, 0);
	if (r == -1) {
		perror("umask2");
		exit(1);
	}
	if (r != 022) {
		fprintf(stderr, "umask2: expected %o, got %o\n", 022, r);
		exit(1);
	}

	/* Call umask2 to read current umask without modifying it. */
	r = umask2_(0777, UMASK_GET_MASK);
	if (r == -1) {
		perror("umask2");
		exit(1);
	}
	if (r != 023) {
		fprintf(stderr, "umask2: expected %o, got %o\n", 023, r);
		exit(1);
	}

	/* Call it again to make sure we didn't modify umask. */
	r = umask2_(0777, UMASK_GET_MASK);
	if (r == -1) {
		perror("umask2");
		exit(1);
	}
	if (r != 023) {
		fprintf(stderr, "umask2: expected %o, got %o\n", 023, r);
		exit(1);
	}

	exit(0);
}
