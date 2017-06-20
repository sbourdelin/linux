/* Copyright (c) 2017 Facebook
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of version 2 of the GNU General Public
 * License as published by the Free Software Foundation.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <linux/bpf.h>
#include "libbpf.h"
#include "bpf_load.h"
#include <unistd.h>
#include <errno.h>
#include <linux/unistd.h>

static void usage(char *pname)
{
	printf("USAGE:\n  %s [-r] [-l] <pname>\n", pname);
	printf("WHERE:\n");
	printf("  -r      remove current loaded socketops BPF program\n");
	printf("          not needed if loading a new program\n");
	printf("  -l      print out BPF log buffer\n");
	printf("  <pname> name of BPF sockeops program to load\n");
	printf("          if <pname> does not end in \".o\", then \"_kern.o\" "
	       "is appended\n");
	printf("          example: using tcp1 will load tcp1_kern.o\n");
	printf("\n");
}

int main(int argc, char **argv)
{
	//union bpf_attr attr;
	int k, logFlag = 0;
	int error = 0;
	char fn[500];

	if (argc <= 1)
		usage(argv[0]);
	for (k = 1; k < argc; k++) {
		if (!strcmp(argv[k], "-r")) {
			error = bpf_prog_detach(0, BPF_GLOBAL_SOCK_OPS);
			if (error) {
				printf("ERROR: bpf_prog_detach: %d (%s)\n",
				       error, strerror(errno));
				error =  1;
			}
		} else if (!strcmp(argv[k], "-l")) {
			logFlag = 1;
		} else if (!strcmp(argv[k], "-h")) {
			usage(argv[0]);
		} else if (argv[k][0] == '-') {
			printf("Error, unknown flag: %s\n", argv[k]);
			error = 2;
		} else if (strlen(argv[k]) > 450) {
			printf("Error, program name too long %d\n",
			       (int) strlen(argv[k]));
			error = 3;
		} else {
			if (!strcmp(argv[k]+strlen(argv[k])-2, ".o"))
				strcpy(fn, argv[k]);
			else
				sprintf(fn, "%s_kern.o", argv[k]);
			if (logFlag)
				printf("loading bpf file:%s\n", fn);
			if (load_bpf_file(fn)) {
				printf("%s", bpf_log_buf);
				return 1;
			}
			if (logFlag) {
				printf("TCP BPF Loaded %s\n", fn);
				printf("%s\n", bpf_log_buf);
			}
			error = bpf_prog_attach(prog_fd[0], 0,
						BPF_GLOBAL_SOCK_OPS, 0);
			if (error) {
				printf("ERROR: bpf_prog_attach: %d (%s)\n",
				       error, strerror(errno));
				error = 4;
			} else if (logFlag) {
				read_trace_pipe();
			}
		}
	}
	return error;
}
