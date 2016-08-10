#include <stdio.h>
#include <assert.h>
#include <linux/bpf.h>
#include <unistd.h>
#include "libbpf.h"
#include "bpf_load.h"
#include <sys/socket.h>
#include <string.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <errno.h>
#include <linux/bpf.h>

static void usage(char **argv)
{
	printf("Usage:	%s [...]\n", argv[0]);
	printf("Prints the file opening activity of all processes under a given cgroupv2 hierarchy.\n");
	printf("	-v <value>	Full path of the cgroup2\n");
	printf("	-h		Display this help\n");
}

int main(int argc, char **argv)
{
	char filename[256];
	const char *cg2 = NULL;
	int ret, opt, cg2_fd;
	int array_index = 0;

	while ((opt = getopt(argc, argv, "v:")) != -1) {
		switch (opt) {
		case 'v':
			cg2 = optarg;
			break;
		default:
			usage(argv);
			return 1;
		}
	}

	if (!cg2) {
		usage(argv);
		return 1;
	}

	cg2_fd = open(cg2, O_RDONLY);
	if (cg2_fd < 0) {
		fprintf(stderr, "open(%s,...): %s(%d)\n",
			cg2, strerror(errno), errno);
		return 1;
	}

	snprintf(filename, sizeof(filename), "%s_kern.o", argv[0]);
	if (load_bpf_file(filename)) {
		printf("%s", bpf_log_buf);
		return 1;
	}

	ret = bpf_update_elem(map_fd[0],
			      &array_index,
			      &cg2_fd, BPF_ANY);
	if (ret) {
		perror("bpf_update_elem");
		return 1;
	}

	read_trace_pipe();
	return 0;
}
