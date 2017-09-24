#ifndef FDMAP_H
#define FDMAP_H

#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>

long fdmap(pid_t pid, int *fds, size_t count, int start_fd, int flags);
int fdmap_full(pid_t pid, int **fds, size_t *n);
int fdmap_proc(pid_t pid, int **fds, size_t *n);

#endif
