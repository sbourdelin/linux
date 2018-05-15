// SPDX-License-Identifier: GPL-2.0
// test ir decoder
//
// Copyright (C) 2018 Sean Young <sean@mess.org>

#include <linux/bpf.h>
#include <linux/lirc.h>
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <libgen.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "bpf_util.h"
#include <bpf/bpf.h>
#include <bpf/libbpf.h>

int main(int argc, char **argv)
{
	struct bpf_object *obj;
	int ret, lircfd, progfd, mode;
	int testir = 0x1dead;

	if (argc != 2) {
		printf("Usage: %s /dev/lircN\n", argv[0]);
		return 2;
	}

	ret = bpf_prog_load("test_rawir_event_kern.o",
			    BPF_PROG_TYPE_RAWIR_EVENT, &obj, &progfd);
	if (ret) {
		printf("Failed to load bpf program\n");
		return 1;
	}

	lircfd = open(argv[1], O_RDWR | O_NONBLOCK);
	if (lircfd == -1) {
		printf("failed to open lirc device %s: %m\n", argv[1]);
		return 1;
	}

	mode = LIRC_MODE_SCANCODE;
	if (ioctl(lircfd, LIRC_SET_REC_MODE, &mode)) {
		printf("failed to set rec mode: %m\n");
		return 1;
	}

	ret = bpf_prog_attach(progfd, lircfd, BPF_RAWIR_EVENT, 0);
	if (ret) {
		printf("Failed to attach bpf to lirc device: %m\n");
		return 1;
	}

	/* Write raw IR */
	ret = write(lircfd, &testir, sizeof(testir));
	if (ret != sizeof(testir)) {
		printf("Failed to send test IR message: %m\n");
		return 1;
	}

	struct pollfd pfd = { .fd = lircfd, .events = POLLIN };

	poll(&pfd, 1, 100);

	struct lirc_scancode lsc;

	/* Read decoded IR */
	ret = read(lircfd, &lsc, sizeof(lsc));
	if (ret != sizeof(lsc)) {
		printf("Failed to read decoded IR: %m\n");
		return 1;
	}

	if (lsc.scancode != 0xdead || lsc.rc_proto != 64) {
		printf("Incorrect scancode decoded\n");
		return 1;
	}

	return 0;
}
