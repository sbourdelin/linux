/* eBPF example program:
 *
 * - Loads eBPF program
 *
 *   The eBPF program sets the sk_bound_dev_if index in new AF_INET{6}
 *   sockets opened by processes in the cgroup.
 *
 * - Attaches the new program to a cgroup using BPF_PROG_ATTACH
 */

#define _GNU_SOURCE

#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <linux/bpf.h>

#include "libbpf.h"

char bpf_log_buf[BPF_LOG_BUF_SIZE];

static int prog_load(int idx, __u64 dev, __u64 ino)
{
	struct bpf_insn prog[] = {
		BPF_MOV64_REG(BPF_REG_6, BPF_REG_1), /* save sk ctx to r6 */

		/* compare network namespace context for socket; r1 = ctx */
		BPF_LD_IMM64(BPF_REG_2, dev),
		BPF_LD_IMM64(BPF_REG_3, ino),
		BPF_RAW_INSN(BPF_JMP | BPF_CALL, 0, 0, 0, BPF_FUNC_sk_netns_cmp),
		/* if no match skip setting sk_bound_dev_if */
		BPF_JMP_IMM(BPF_JEQ, BPF_REG_0, 0, 3),

		/* set sk_bound_dev_if for socket */
		BPF_MOV64_IMM(BPF_REG_2, idx),
		BPF_MOV64_REG(BPF_REG_1, BPF_REG_6),
		BPF_STX_MEM(BPF_W, BPF_REG_1, BPF_REG_2,
			    offsetof(struct bpf_sock, bound_dev_if)),

		BPF_MOV64_IMM(BPF_REG_0, 1), /* r0 = verdict */
		BPF_EXIT_INSN(),
	};
	size_t insns_cnt = sizeof(prog) / sizeof(struct bpf_insn);

	return bpf_load_program(BPF_PROG_TYPE_CGROUP_SOCK, prog, insns_cnt,
				"GPL", 0, bpf_log_buf, BPF_LOG_BUF_SIZE);
}

/* return namespace dev and inode */
static int get_netns(pid_t pid, __u64 *ns_dev, __u64 *ns_ino)
{
	char path[64];
	struct stat st;

	snprintf(path, sizeof(path), "/proc/%d/ns/net", pid);

	if (stat(path, &st) != 0)
		return -1;

	*ns_dev = st.st_dev;
	*ns_ino = st.st_ino;

	return 0;
}

static int bind_prog(const char *cpath, const char *dev)
{
	int cg_fd, prog_fd, ret;
	unsigned int idx;
	__u64 ns_dev, ns_ino;

	if (!dev)
		return 1;

	idx = if_nametoindex(dev);
	if (!idx) {
		printf("Invalid device name\n");
		return EXIT_FAILURE;
	}

	if (get_netns(getpid(), &ns_dev, &ns_ino)) {
		fprintf(stderr,
			"Failed to read network namespace data\n");
		return EXIT_FAILURE;
	}

	cg_fd = open(cpath, O_DIRECTORY | O_RDONLY);
	if (cg_fd < 0) {
		printf("Failed to open cgroup path: '%s'\n", strerror(errno));
		return EXIT_FAILURE;
	}

	prog_fd = prog_load(idx, ns_dev, ns_ino);
	printf("Output from kernel verifier:\n%s\n-------\n", bpf_log_buf);

	if (prog_fd < 0) {
		printf("Failed to load prog: '%s'\n", strerror(errno));
		return EXIT_FAILURE;
	}

	ret = bpf_prog_attach(prog_fd, cg_fd, BPF_CGROUP_INET_SOCK_CREATE, 0);
	if (ret < 0) {
		printf("Failed to attach prog to cgroup: '%s'\n",
		       strerror(errno));
		return EXIT_FAILURE;
	}

	return EXIT_SUCCESS;
}

static int socket_test(int family, const char *dev, int is_negative)
{
	unsigned int idx;
	socklen_t optlen;
	char name[16];
	int sd, rc;

	if (!dev)
		return 1;

	if (!is_negative) {
		idx = if_nametoindex(dev);
		if (!idx) {
			printf("Invalid device name\n");
			return EXIT_FAILURE;
		}
	}

	sd = socket(family, SOCK_DGRAM, 0);
	if (sd < 0)
		return 1;

	name[0] = '\0';
	optlen = sizeof(name);
	rc = getsockopt(sd, SOL_SOCKET, SO_BINDTODEVICE, name, &optlen);

	close(sd);
	if (rc) {
		printf("getsockopt(SO_BINDTODEVICE) failed\n");
		return 1;
	}

	printf("%s socket bound to \"%s\", checking against \"%s\", neg test %d\n",
		family == PF_INET ? "ipv4" : "ipv6",
		name, dev, is_negative);

	if (strcmp(name, dev) && !is_negative) {
		printf("socket not bound to device as expected\n");
		return 1;
	}

	if (!strcmp(name, dev) && is_negative) {
		printf("socket is bound to device when not expected\n");
		return 1;
	}

	return 0;
}

static int usage(const char *argv0)
{
	printf("Usage: %s -c cg-path -d device-index -4 -6 -n\n", argv0);
	return EXIT_FAILURE;
}

int main(int argc, char **argv)
{
	const char *dev = NULL, *cpath = NULL;
	int do_ipv4 = 0, do_ipv6 = 0, is_negative = 0;
	int rc;

	extern char *optarg;

	while ((rc = getopt(argc, argv, "d:c:46in")) > 0) {
		switch (rc) {
		case 'd':
			dev = optarg;
			break;
		case 'c':
			cpath = optarg;
			break;
		case '4':
			do_ipv4 = 1;
			break;
		case '6':
			do_ipv6 = 1;
			break;
		case 'n':
			is_negative = 1;
			break;
		default:
			usage(argv[0]);
			return 1;
		}
	}

	if (cpath && bind_prog(cpath, dev))
		return 1;

	if (do_ipv4 && socket_test(PF_INET, dev, is_negative))
		return 1;

	if (do_ipv6 && socket_test(PF_INET6, dev, is_negative))
		return 1;

	return EXIT_SUCCESS;
}
