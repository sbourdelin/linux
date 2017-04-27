/* Copyright (c) 2017 Facebook
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of version 2 of the GNU General Public
 * License as published by the Free Software Foundation.
 */
#include <linux/types.h>
#include <linux/bpf.h>
#include <linux/err.h>
#include <bpf/bpf.h>
#include <sys/resource.h>
#include <unistd.h>
#include <bpf/libbpf.h>
#include <assert.h>

#include "bpf_util.h"

static int bpf_prog_load(const char *file, enum bpf_prog_type type,
			 struct bpf_object **pobj, int *prog_fd)
{
	struct bpf_program *prog;
	struct bpf_object *obj;
	int err;

	obj = bpf_object__open(file);
	if (IS_ERR(obj))
		return -ENOENT;

	prog = bpf_program__next(NULL, obj);
	if (!prog) {
		bpf_object__close(obj);
		return -ENOENT;
	}

	bpf_program__set_type(prog, type);
	err = bpf_object__load(obj);
	if (err) {
		bpf_object__close(obj);
		return -EINVAL;
	}

	*pobj = obj;
	*prog_fd = bpf_program__fd(prog);
	return 0;
}

int main(void)
{
	struct rlimit rinf = { RLIM_INFINITY, RLIM_INFINITY };
	const char *file = "./test_pkt_access.o";
	const int nr_iters = 16;
	int bpf_prog_fds[nr_iters];
	int i, err = 0;
	uint32_t next_id = 0;

	if (setrlimit(RLIMIT_MEMLOCK, &rinf)) {
		perror("setrlimit");
		return -1;
	}

	memset(bpf_prog_fds, -1, sizeof(bpf_prog_fds));

	for (i = 0; i < nr_iters; i++) {
		struct bpf_object *obj;
		int prog_fd;

		err = bpf_prog_load(file, BPF_PROG_TYPE_SCHED_CLS, &obj,
				    &prog_fd);
		if (err) {
			perror("bpf_prog_load");
			goto done;
		}

		bpf_prog_fds[i] = prog_fd;
	}

	i = 0;
	while (!bpf_prog_get_next_id(next_id, &next_id)) {
		printf("prog_uid:%08u\n", next_id);
		i++;
		assert(i <= nr_iters);
	}
	assert(i == nr_iters);

done:
	for (i = 0; i < nr_iters; i++) {
		if (bpf_prog_fds[i] != -1) {
			close(bpf_prog_fds[i]);
		}
	}

	return err ? -1 : 0;
}
