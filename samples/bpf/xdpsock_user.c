// SPDX-License-Identifier: GPL-2.0
/* Copyright(c) 2017 - 2018 Intel Corporation. */

#include <assert.h>
#include <errno.h>
#include <getopt.h>
#include <libgen.h>
#include <linux/bpf.h>
#include <linux/if_link.h>
#include <linux/if_xdp.h>
#include <linux/if_ether.h>
#include <net/if.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <net/ethernet.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <locale.h>
#include <sys/types.h>
#include <poll.h>

#include "bpf/libbpf.h"
#include "bpf_util.h"
#include <bpf/bpf.h>

#include "xdpsock.h"

#ifndef SOL_XDP
#define SOL_XDP 283
#endif

#ifndef AF_XDP
#define AF_XDP 44
#endif

#ifndef PF_XDP
#define PF_XDP AF_XDP
#endif

#define NUM_FRAMES (4 * 1024)
#define BATCH_SIZE 64

#define DEBUG_HEXDUMP 0

typedef __u64 u64;
typedef __u32 u32;

static unsigned long prev_time;

enum benchmark_type {
	BENCH_RXDROP = 0,
	BENCH_TXONLY = 1,
	BENCH_L2FWD = 2,
};

static enum benchmark_type opt_bench = BENCH_RXDROP;
static u32 opt_xdp_flags;
static const char *opt_if = "";
static int opt_ifindex;
static int opt_queue;
static int opt_poll;
static int opt_shared_packet_buffer;
static int opt_interval = 1;
static u32 opt_xdp_bind_flags;

struct xdp_umem {
	struct xsk_prod_ring fq;
	struct xsk_cons_ring cq;
	char *umem_area;
	int fd;
};

struct xsk_socket {
	struct xsk_cons_ring rx;
	struct xsk_prod_ring tx;
	struct xdp_umem *umem;
	u32 outstanding_tx;
	unsigned long rx_npkts;
	unsigned long tx_npkts;
	unsigned long prev_rx_npkts;
	unsigned long prev_tx_npkts;
	int fd;
};

static int num_socks;
struct xsk_socket *xsks[MAX_SOCKS];

static void dump_stats(void);

static void __exit_with_error(int error, const char *file, const char *func,
			      int line)
{
	fprintf(stderr, "%s:%s:%i: errno: %d/\"%s\"\n", file, func,
		line, error, strerror(error));
	dump_stats();
	bpf_set_link_xdp_fd(opt_ifindex, -1, opt_xdp_flags);
	exit(EXIT_FAILURE);
}

#define exit_with_error(error) __exit_with_error(error, __FILE__, __func__, \
						 __LINE__)

static unsigned long get_nsecs(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000000000UL + ts.tv_nsec;
}

static const char pkt_data[] =
	"\x3c\xfd\xfe\x9e\x7f\x71\xec\xb1\xd7\x98\x3a\xc0\x08\x00\x45\x00"
	"\x00\x2e\x00\x00\x00\x00\x40\x11\x88\x97\x05\x08\x07\x08\xc8\x14"
"\x1e\x04\x10\x92\x10\x92\x00\x1a\x6d\xa3\x34\x33\x1f\x69\x40\x6b"
	"\x54\x59\xb6\x14\x2d\x11\x44\xbf\xaf\xd9\xbe\xaa";

static void swap_mac_addresses(void *data)
{
	struct ether_header *eth = (struct ether_header *)data;
	struct ether_addr *src_addr = (struct ether_addr *)&eth->ether_shost;
	struct ether_addr *dst_addr = (struct ether_addr *)&eth->ether_dhost;
	struct ether_addr tmp;

	tmp = *src_addr;
	*src_addr = *dst_addr;
	*dst_addr = tmp;
}

static void hex_dump(void *pkt, size_t length, u64 addr)
{
	const unsigned char *address = (unsigned char *)pkt;
	const unsigned char *line = address;
	size_t line_size = 32;
	unsigned char c;
	char buf[32];
	int i = 0;

	if (!DEBUG_HEXDUMP)
		return;

	sprintf(buf, "addr=%llu", addr);
	printf("length = %zu\n", length);
	printf("%s | ", buf);
	while (length-- > 0) {
		printf("%02X ", *address++);
		if (!(++i % line_size) || (length == 0 && i % line_size)) {
			if (length == 0) {
				while (i++ % line_size)
					printf("__ ");
			}
			printf(" | ");	/* right close */
			while (line < address) {
				c = *line++;
				printf("%c", (c < 33 || c == 255) ? 0x2E : c);
			}
			printf("\n");
			if (length > 0)
				printf("%s | ", buf);
		}
	}
	printf("\n");
}

static size_t gen_eth_frame(char *frame)
{
	memcpy(frame, pkt_data, sizeof(pkt_data) - 1);
	return sizeof(pkt_data) - 1;
}

static struct xdp_umem *xsk_configure_umem(void *buffer, u64 size)
{
	struct xdp_umem *umem;

	umem = calloc(1, sizeof(*umem));
	if (!umem)
		exit_with_error(errno);

	umem->fd = xsk__create_umem(buffer, size, &umem->fq, &umem->cq, NULL);
	if (umem->fd < 0)
		exit_with_error(-umem->fd);

	umem->umem_area = buffer;

	return umem;
}

static struct xsk_socket *xsk_configure_socket(struct xdp_umem *umem,
					       bool shared)
{
	struct sockaddr_xdp sxdp = {};
	struct xsk_socket *xsk;
	int ret;

	xsk = calloc(1, sizeof(*xsk));
	if (!xsk)
		exit_with_error(errno);

	xsk->umem = umem;
	xsk->fd = xsk__create_xdp_socket(umem->fd, &xsk->rx, &xsk->tx, NULL);
	if (xsk->fd < 0)
		exit_with_error(-xsk->fd);

	sxdp.sxdp_family = PF_XDP;
	sxdp.sxdp_ifindex = opt_ifindex;
	sxdp.sxdp_queue_id = opt_queue;

	if (shared) {
		sxdp.sxdp_flags = XDP_SHARED_UMEM;
		sxdp.sxdp_shared_umem_fd = umem->fd;
	} else {
		sxdp.sxdp_flags = opt_xdp_bind_flags;
	}

	if (!shared) {
		u32 idx;
		int i;

		ret = xsk__reserve_prod(&xsk->umem->fq, XSK__DEFAULT_NUM_DESCS,
				       &idx);
		if (ret != XSK__DEFAULT_NUM_DESCS)
			exit_with_error(-ret);
		for (i = 0;
		     i < XSK__DEFAULT_NUM_DESCS * XSK__DEFAULT_FRAME_SIZE;
		     i += XSK__DEFAULT_FRAME_SIZE)
			*xsk__get_fill_desc(&xsk->umem->fq, idx++) = i;
		xsk__submit_prod(&xsk->umem->fq);
	}

	ret = bind(xsk->fd, (struct sockaddr *)&sxdp, sizeof(sxdp));
	if (ret)
		exit_with_error(errno);

	return xsk;
}

static void print_benchmark(bool running)
{
	const char *bench_str = "INVALID";

	if (opt_bench == BENCH_RXDROP)
		bench_str = "rxdrop";
	else if (opt_bench == BENCH_TXONLY)
		bench_str = "txonly";
	else if (opt_bench == BENCH_L2FWD)
		bench_str = "l2fwd";

	printf("%s:%d %s ", opt_if, opt_queue, bench_str);
	if (opt_xdp_flags & XDP_FLAGS_SKB_MODE)
		printf("xdp-skb ");
	else if (opt_xdp_flags & XDP_FLAGS_DRV_MODE)
		printf("xdp-drv ");
	else
		printf("	");

	if (opt_poll)
		printf("poll() ");

	if (running) {
		printf("running...");
		fflush(stdout);
	}
}

static void dump_stats(void)
{
	unsigned long now = get_nsecs();
	long dt = now - prev_time;
	int i;

	prev_time = now;

	for (i = 0; i < num_socks && xsks[i]; i++) {
		char *fmt = "%-15s %'-11.0f %'-11lu\n";
		double rx_pps, tx_pps;

		rx_pps = (xsks[i]->rx_npkts - xsks[i]->prev_rx_npkts) *
			 1000000000. / dt;
		tx_pps = (xsks[i]->tx_npkts - xsks[i]->prev_tx_npkts) *
			 1000000000. / dt;

		printf("\n sock%d@", i);
		print_benchmark(false);
		printf("\n");

		printf("%-15s %-11s %-11s %-11.2f\n", "", "pps", "pkts",
		       dt / 1000000000.);
		printf(fmt, "rx", rx_pps, xsks[i]->rx_npkts);
		printf(fmt, "tx", tx_pps, xsks[i]->tx_npkts);

		xsks[i]->prev_rx_npkts = xsks[i]->rx_npkts;
		xsks[i]->prev_tx_npkts = xsks[i]->tx_npkts;
	}
}

static void *poller(void *arg)
{
	(void)arg;
	for (;;) {
		sleep(opt_interval);
		dump_stats();
	}

	return NULL;
}

static void int_exit(int sig)
{
	(void)sig;
	dump_stats();
	bpf_set_link_xdp_fd(opt_ifindex, -1, opt_xdp_flags);
	exit(EXIT_SUCCESS);
}

static struct option long_options[] = {
	{"rxdrop", no_argument, 0, 'r'},
	{"txonly", no_argument, 0, 't'},
	{"l2fwd", no_argument, 0, 'l'},
	{"interface", required_argument, 0, 'i'},
	{"queue", required_argument, 0, 'q'},
	{"poll", no_argument, 0, 'p'},
	{"shared-buffer", no_argument, 0, 's'},
	{"xdp-skb", no_argument, 0, 'S'},
	{"xdp-native", no_argument, 0, 'N'},
	{"interval", required_argument, 0, 'n'},
	{"zero-copy", no_argument, 0, 'z'},
	{"copy", no_argument, 0, 'c'},
	{0, 0, 0, 0}
};

static void usage(const char *prog)
{
	const char *str =
		"  Usage: %s [OPTIONS]\n"
		"  Options:\n"
		"  -r, --rxdrop		Discard all incoming packets (default)\n"
		"  -t, --txonly		Only send packets\n"
		"  -l, --l2fwd		MAC swap L2 forwarding\n"
		"  -i, --interface=n	Run on interface n\n"
		"  -q, --queue=n	Use queue n (default 0)\n"
		"  -p, --poll		Use poll syscall\n"
		"  -s, --shared-buffer	Use shared packet buffer\n"
		"  -S, --xdp-skb=n	Use XDP skb-mod\n"
		"  -N, --xdp-native=n	Enfore XDP native mode\n"
		"  -n, --interval=n	Specify statistics update interval (default 1 sec).\n"
		"  -z, --zero-copy      Force zero-copy mode.\n"
		"  -c, --copy           Force copy mode.\n"
		"\n";
	fprintf(stderr, str, prog);
	exit(EXIT_FAILURE);
}

static void parse_command_line(int argc, char **argv)
{
	int option_index, c;

	opterr = 0;

	for (;;) {
		c = getopt_long(argc, argv, "rtli:q:psSNn:cz", long_options,
				&option_index);
		if (c == -1)
			break;

		switch (c) {
		case 'r':
			opt_bench = BENCH_RXDROP;
			break;
		case 't':
			opt_bench = BENCH_TXONLY;
			break;
		case 'l':
			opt_bench = BENCH_L2FWD;
			break;
		case 'i':
			opt_if = optarg;
			break;
		case 'q':
			opt_queue = atoi(optarg);
			break;
		case 's':
			opt_shared_packet_buffer = 1;
			break;
		case 'p':
			opt_poll = 1;
			break;
		case 'S':
			opt_xdp_flags |= XDP_FLAGS_SKB_MODE;
			opt_xdp_bind_flags |= XDP_COPY;
			break;
		case 'N':
			opt_xdp_flags |= XDP_FLAGS_DRV_MODE;
			break;
		case 'n':
			opt_interval = atoi(optarg);
			break;
		case 'z':
			opt_xdp_bind_flags |= XDP_ZEROCOPY;
			break;
		case 'c':
			opt_xdp_bind_flags |= XDP_COPY;
			break;
		default:
			usage(basename(argv[0]));
		}
	}

	opt_ifindex = if_nametoindex(opt_if);
	if (!opt_ifindex) {
		fprintf(stderr, "ERROR: interface \"%s\" does not exist\n",
			opt_if);
		usage(basename(argv[0]));
	}
}

static void kick_tx(int fd)
{
	int ret;

	ret = sendto(fd, NULL, 0, MSG_DONTWAIT, NULL, 0);
	if (ret >= 0 || errno == ENOBUFS || errno == EAGAIN || errno == EBUSY)
		return;
	exit_with_error(errno);
}

static inline void complete_tx_l2fwd(struct xsk_socket *xsk)
{
	u32 idx_cq, idx_fq;
	unsigned int rcvd;
	size_t ndescs;

	if (!xsk->outstanding_tx)
		return;

	kick_tx(xsk->fd);
	ndescs = (xsk->outstanding_tx > BATCH_SIZE) ? BATCH_SIZE :
		xsk->outstanding_tx;

	/* re-add completed Tx buffers */
	rcvd = xsk__peek_cons(&xsk->umem->cq, ndescs, &idx_cq);
	if (rcvd > 0) {
		unsigned int i;
		int ret;

		ret = xsk__reserve_prod(&xsk->umem->fq, rcvd, &idx_fq);
		while (ret != rcvd) {
			if (ret < 0)
				exit_with_error(-ret);
			ret = xsk__reserve_prod(&xsk->umem->fq, rcvd, &idx_fq);
		}
		for (i = 0; i < rcvd; i++)
			*xsk__get_completion_desc(&xsk->umem->cq, idx_cq++) =
				*xsk__get_fill_desc(&xsk->umem->fq, idx_fq++);

		xsk__submit_prod(&xsk->umem->fq);
		xsk__release_cons(&xsk->umem->cq);
		xsk->outstanding_tx -= rcvd;
		xsk->tx_npkts += rcvd;
	}
}

static inline void complete_tx_only(struct xsk_socket *xsk)
{
	unsigned int rcvd;
	u32 idx;

	if (!xsk->outstanding_tx)
		return;

	kick_tx(xsk->fd);

	rcvd = xsk__peek_cons(&xsk->umem->cq, BATCH_SIZE, &idx);
	if (rcvd > 0) {
		xsk__release_cons(&xsk->umem->cq);
		xsk->outstanding_tx -= rcvd;
		xsk->tx_npkts += rcvd;
	}
}

static void rx_drop(struct xsk_socket *xsk)
{
	unsigned int rcvd, i;
	u32 idx_rx, idx_fq;
	int ret;

	rcvd = xsk__peek_cons(&xsk->rx, BATCH_SIZE, &idx_rx);
	if (!rcvd)
		return;

	ret = xsk__reserve_prod(&xsk->umem->fq, rcvd, &idx_fq);
	while (ret != rcvd) {
		if (ret < 0)
			exit_with_error(-ret);
		ret = xsk__reserve_prod(&xsk->umem->fq, rcvd, &idx_fq);
	}

	for (i = 0; i < rcvd; i++) {
		u64 addr = xsk__get_rx_desc(&xsk->rx, idx_rx)->addr;
		u32 len = xsk__get_rx_desc(&xsk->rx, idx_rx++)->len;
		char *pkt = xsk__get_data(xsk->umem->umem_area, addr);

		hex_dump(pkt, len, addr);
		*xsk__get_fill_desc(&xsk->umem->fq, idx_fq++) = addr;
	}

	xsk__submit_prod(&xsk->umem->fq);
	xsk__release_cons(&xsk->rx);
	xsk->rx_npkts += rcvd;
}

static void rx_drop_all(void)
{
	struct pollfd fds[MAX_SOCKS + 1];
	int i, ret, timeout, nfds = 1;

	memset(fds, 0, sizeof(fds));

	for (i = 0; i < num_socks; i++) {
		fds[i].fd = xsks[i]->fd;
		fds[i].events = POLLIN;
		timeout = 1000; /* 1sn */
	}

	for (;;) {
		if (opt_poll) {
			ret = poll(fds, nfds, timeout);
			if (ret <= 0)
				continue;
		}

		for (i = 0; i < num_socks; i++)
			rx_drop(xsks[i]);
	}
}

static void tx_only(struct xsk_socket *xsk)
{
	int timeout, ret, nfds = 1;
	struct pollfd fds[nfds + 1];
	u32 idx, frame_nb = 0;

	memset(fds, 0, sizeof(fds));
	fds[0].fd = xsk->fd;
	fds[0].events = POLLOUT;
	timeout = 1000; /* 1sn */

	for (;;) {
		if (opt_poll) {
			ret = poll(fds, nfds, timeout);
			if (ret <= 0)
				continue;

			if (fds[0].fd != xsk->fd ||
			    !(fds[0].revents & POLLOUT))
				continue;
		}

		if (xsk__reserve_prod(&xsk->tx, BATCH_SIZE, &idx) ==
		    BATCH_SIZE) {
			unsigned int i;

			for (i = 0; i < BATCH_SIZE; i++) {
				xsk__get_tx_desc(&xsk->tx, idx + i)->addr =
					(frame_nb + i) <<
					XSK__DEFAULT_FRAME_SHIFT;
				xsk__get_tx_desc(&xsk->tx, idx + i)->len =
					sizeof(pkt_data) - 1;
			}

			xsk__submit_prod(&xsk->tx);
			xsk->outstanding_tx += BATCH_SIZE;
			frame_nb += BATCH_SIZE;
			frame_nb %= NUM_FRAMES;
		}

		complete_tx_only(xsk);
	}
}

static void l2fwd(struct xsk_socket *xsk)
{
	for (;;) {
		unsigned int rcvd, i;
		u32 idx_rx, idx_tx;
		int ret;

		for (;;) {
			complete_tx_l2fwd(xsk);

			rcvd = xsk__peek_cons(&xsk->rx, BATCH_SIZE, &idx_rx);
			if (rcvd > 0)
				break;
		}

		ret = xsk__reserve_prod(&xsk->tx, rcvd, &idx_tx);
		while (ret != rcvd) {
			if (ret < 0)
				exit_with_error(-ret);
			ret = xsk__reserve_prod(&xsk->tx, rcvd, &idx_tx);
		}

		for (i = 0; i < rcvd; i++) {
			u64 addr = xsk__get_rx_desc(&xsk->rx, idx_rx)->addr;
			u32 len = xsk__get_rx_desc(&xsk->rx, idx_rx++)->len;
			char *pkt = xsk__get_data(xsk->umem->umem_area, addr);

			swap_mac_addresses(pkt);

			hex_dump(pkt, len, addr);
			xsk__get_tx_desc(&xsk->tx, idx_tx)->addr = addr;
			xsk__get_tx_desc(&xsk->tx, idx_tx++)->len = len;
		}

		xsk__submit_prod(&xsk->tx);
		xsk__release_cons(&xsk->rx);

		xsk->rx_npkts += rcvd;
		xsk->outstanding_tx += rcvd;
	}
}

int main(int argc, char **argv)
{
	struct rlimit r = {RLIM_INFINITY, RLIM_INFINITY};
	struct bpf_prog_load_attr prog_load_attr = {
		.prog_type	= BPF_PROG_TYPE_XDP,
	};
	int prog_fd, qidconf_map, xsks_map;
	struct bpf_object *obj;
	char xdp_filename[256];
	struct xdp_umem *umem;
	struct bpf_map *map;
	int i, ret, key = 0;
	pthread_t pt;
	void *bufs;

	parse_command_line(argc, argv);

	if (setrlimit(RLIMIT_MEMLOCK, &r)) {
		fprintf(stderr, "ERROR: setrlimit(RLIMIT_MEMLOCK) \"%s\"\n",
			strerror(errno));
		exit(EXIT_FAILURE);
	}

	snprintf(xdp_filename, sizeof(xdp_filename), "%s_kern.o", argv[0]);
	prog_load_attr.file = xdp_filename;

	if (bpf_prog_load_xattr(&prog_load_attr, &obj, &prog_fd))
		exit(EXIT_FAILURE);
	if (prog_fd < 0) {
		fprintf(stderr, "ERROR: no program found: %s\n",
			strerror(prog_fd));
		exit(EXIT_FAILURE);
	}

	map = bpf_object__find_map_by_name(obj, "qidconf_map");
	qidconf_map = bpf_map__fd(map);
	if (qidconf_map < 0) {
		fprintf(stderr, "ERROR: no qidconf map found: %s\n",
			strerror(qidconf_map));
		exit(EXIT_FAILURE);
	}

	map = bpf_object__find_map_by_name(obj, "xsks_map");
	xsks_map = bpf_map__fd(map);
	if (xsks_map < 0) {
		fprintf(stderr, "ERROR: no xsks map found: %s\n",
			strerror(xsks_map));
		exit(EXIT_FAILURE);
	}

	if (bpf_set_link_xdp_fd(opt_ifindex, prog_fd, opt_xdp_flags) < 0) {
		fprintf(stderr, "ERROR: link set xdp fd failed\n");
		exit(EXIT_FAILURE);
	}

	ret = bpf_map_update_elem(qidconf_map, &key, &opt_queue, 0);
	if (ret) {
		fprintf(stderr, "ERROR: bpf_map_update_elem qidconf\n");
		exit(EXIT_FAILURE);
	}

	ret = posix_memalign(&bufs, getpagesize(), /* PAGE_SIZE aligned */
			     NUM_FRAMES * XSK__DEFAULT_FRAME_SIZE);
	if (ret)
		exit_with_error(ret);

       /* Create sockets... */
	umem = xsk_configure_umem(bufs, NUM_FRAMES * XSK__DEFAULT_FRAME_SIZE);
	xsks[num_socks++] = xsk_configure_socket(umem, false);

	if (opt_bench == BENCH_TXONLY) {
		int i;

		for (i = 0; i < NUM_FRAMES * XSK__DEFAULT_FRAME_SIZE;
		     i += XSK__DEFAULT_FRAME_SIZE)
			(void)gen_eth_frame(&umem->umem_area[i]);
	}

#if RR_LB
	for (i = 0; i < MAX_SOCKS - 1; i++)
		xsks[num_socks++] = xsk_configure_socket(umem, true);
#endif

	/* ...and insert them into the map. */
	for (i = 0; i < num_socks; i++) {
		key = i;
		ret = bpf_map_update_elem(xsks_map, &key, &xsks[i]->fd, 0);
		if (ret) {
			fprintf(stderr, "ERROR: bpf_map_update_elem %d\n", i);
			exit(EXIT_FAILURE);
		}
	}

	signal(SIGINT, int_exit);
	signal(SIGTERM, int_exit);
	signal(SIGABRT, int_exit);

	setlocale(LC_ALL, "");

	ret = pthread_create(&pt, NULL, poller, NULL);
	if (ret)
		exit_with_error(ret);

	prev_time = get_nsecs();

	if (opt_bench == BENCH_RXDROP)
		rx_drop_all();
	else if (opt_bench == BENCH_TXONLY)
		tx_only(xsks[0]);
	else
		l2fwd(xsks[0]);

	return 0;
}
