#include <linux/unistd.h>
#include <linux/bpf.h>

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <assert.h>
#include <errno.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/xattr.h>

#include <net/ethernet.h>
#include <arpa/inet.h>

#include "libbpf.h"

static char *fname;

static void test_bpf_pin_fd(int fd)
{
	char buff[64];
	int ret;

	memset(buff, 0, sizeof(buff));
	ret = bpf_pin_fd(fd, fname);
	getxattr(fname, "bpf.type", buff, sizeof(buff));

	printf("fd:%d type:%s pinned (%s)\n", fd, buff, strerror(errno));
	assert(ret == 0);
}

static int test_bpf_new_fd(void)
{
	char buff[64];
	int fd;

	memset(buff, 0, sizeof(buff));
	getxattr(fname, "bpf.type", buff, sizeof(buff));
	fd = bpf_new_fd(fname);

	printf("fd:%d type:%s fetched (%s)\n", fd, buff, strerror(errno));
	assert(fd > 0);

	return fd;
}

static int test_bpf_map_create(void)
{
	int fd;

	fd = bpf_create_map(BPF_MAP_TYPE_ARRAY, sizeof(uint32_t),
			    sizeof(uint32_t), 1024);

	printf("fd:%d created (%s)\n", fd, strerror(errno));
	assert(fd > 0);

	return fd;
}

static int test_bpf_map_insert(int fd, uint32_t val)
{
	uint32_t key = 123;
	int ret;

	ret = bpf_update_elem(fd, &key, &val, 0);

	printf("fd:%d wrote (%u, %u)\n", fd, key, val);
	assert(ret == 0);

	return ret;
}

static int test_bpf_map_lookup(int fd)
{
	uint32_t key = 123, val;
	int ret;

	ret = bpf_lookup_elem(fd, &key, &val);

	printf("fd:%d read (%u, %u)\n", fd, key, val);
	assert(ret == 0);

	return ret;
}

static int bpf_map_test_case_1(void)
{
	int fd;

	fd = test_bpf_map_create();
	test_bpf_pin_fd(fd);
	test_bpf_map_insert(fd, 456);
	test_bpf_map_lookup(fd);
	close(fd);

	return 0;
}

static int bpf_map_test_case_2(void)
{
	int fd;

	fd = test_bpf_new_fd();
	test_bpf_map_lookup(fd);
	close(fd);

	return 0;
}

static int bpf_map_test_case_3(void)
{
	int fd1, fd2;

	unlink(fname);
	fd1 = test_bpf_map_create();
	test_bpf_pin_fd(fd1);
	fd2 = test_bpf_new_fd();
	test_bpf_map_lookup(fd1);
	test_bpf_map_insert(fd2, 456);
	test_bpf_map_lookup(fd1);
	test_bpf_map_lookup(fd2);
	test_bpf_map_insert(fd1, 789);
	test_bpf_map_lookup(fd2);
	close(fd1);
	close(fd2);

	return 0;
}

static int test_bpf_prog_create(void)
{
	struct bpf_insn insns[] = {
		BPF_MOV64_IMM(BPF_REG_0, 1),
		BPF_EXIT_INSN(),
	};
	int ret;

	ret = bpf_prog_load(BPF_PROG_TYPE_SOCKET_FILTER, insns,
			    sizeof(insns), "GPL", 0);

	assert(ret > 0);
	printf("fd:%d created\n", ret);

	return ret;
}

static int test_bpf_prog_attach(int fd)
{
	int sock, ret;

	sock = open_raw_sock("lo");
	ret = setsockopt(sock, SOL_SOCKET, SO_ATTACH_BPF, &fd, sizeof(fd));

	printf("sock:%d got fd:%d attached\n", sock, fd);
	assert(ret == 0);

	return ret;
}

static int bpf_prog_test_case_1(void)
{
	int fd;

	fd = test_bpf_prog_create();
	test_bpf_pin_fd(fd);
	close(fd);

	return 0;
}

static int bpf_prog_test_case_2(void)
{
	int fd;

	fd = test_bpf_new_fd();
	test_bpf_prog_attach(fd);
	close(fd);

	return 0;
}

static int bpf_prog_test_case_3(void)
{
	int fd1, fd2;

	unlink(fname);
	fd1 = test_bpf_prog_create();
	test_bpf_pin_fd(fd1);
	fd2 = test_bpf_new_fd();
	test_bpf_prog_attach(fd1);
	test_bpf_prog_attach(fd2);
	close(fd1);
	close(fd2);

	return 0;
}

int main(int argc, char **argv)
{
	if (argc != 3)
		return -1;

	fname = argv[2];

	if (!strcmp("map-pin", argv[1]))
		return bpf_map_test_case_1();
	if (!strcmp("map-new", argv[1]))
		return bpf_map_test_case_2();
	if (!strcmp("map-all", argv[1]))
		return bpf_map_test_case_3();

	if (!strcmp("prog-pin", argv[1]))
		return bpf_prog_test_case_1();
	if (!strcmp("prog-new", argv[1]))
		return bpf_prog_test_case_2();
	if (!strcmp("prog-all", argv[1]))
		return bpf_prog_test_case_3();

	return 0;
}
