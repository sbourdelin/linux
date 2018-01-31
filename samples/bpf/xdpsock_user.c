/*
 *  Copyright(c) 2017 Intel Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 */

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

#include "bpf_load.h"
#include "bpf_util.h"
#include "libbpf.h"

#ifndef SOL_XDP
#define SOL_XDP 283
#endif

#ifndef AF_XDP
#define AF_XDP 44
#endif

#ifndef PF_XDP
#define PF_XDP AF_XDP
#endif

#define NUM_BUFFERS 131072
#define DATA_HEADROOM 0
#define FRAME_SIZE 2048
#define NUM_DESCS 1024
#define BATCH_SIZE 16

#define DEBUG_HEXDUMP 0

static unsigned long rx_npkts;
static unsigned long tx_npkts;
static unsigned long start_time;

enum benchmark_type {
	BENCH_RXDROP = 0,
	BENCH_TXONLY = 1,
	BENCH_L2FWD = 2,
};

static enum benchmark_type opt_bench = BENCH_RXDROP;
static __u32 opt_xdp_flags;
static const char *opt_if = "";
static int opt_ifindex;
static int opt_queue;

struct xdp_umem {
	char *buffer;
	size_t size;
	unsigned int frame_size;
	unsigned int frame_size_log2;
	unsigned int nframes;
	int mr_fd;
};

struct xdp_queue_pair {
	struct xdp_queue rx;
	struct xdp_queue tx;
	int sfd;
	struct xdp_umem *umem;
	__u32 outstanding_tx;
};

static unsigned long get_nsecs(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000000000UL + ts.tv_nsec;
}
#define lassert(expr)							\
	do {								\
		if (!(expr)) {						\
			fprintf(stderr, "%s:%s:%i: Assertion failed: " #expr ": errno: %d/\"%s\"\n", __FILE__, __func__, __LINE__, errno, strerror(errno)); \
			exit(EXIT_FAILURE);				\
		}							\
	} while (0)

#define barrier() __asm__ __volatile__("": : :"memory")
#define smp_rmb() barrier()
#define smp_wmb() barrier()
#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
#define log2(x) ((unsigned int)(8 * sizeof(unsigned long long) - __builtin_clzll((x)) - 1))

static const char pkt_data[] =
	"\x3c\xfd\xfe\x9e\x7f\x71\xec\xb1\xd7\x98\x3a\xc0\x08\x00\x45\x00"
	"\x00\x2e\x00\x00\x00\x00\x40\x11\x88\x97\x05\x08\x07\x08\xc8\x14"
	"\x1e\x04\x10\x92\x10\x92\x00\x1a\x6d\xa3\x34\x33\x1f\x69\x40\x6b"
	"\x54\x59\xb6\x14\x2d\x11\x44\xbf\xaf\xd9\xbe\xaa";

#include "xdpsock_queue.h"

static inline void *xq_get_data(struct xdp_queue_pair *q, __u32 idx, __u32 off)
{
	if (idx >= q->umem->nframes) {
		fprintf(stderr, "ERROR idx=%u off=%u\n", (unsigned int)idx, (unsigned int)off);
		lassert(0);
	}

	return (__u8 *)(q->umem->buffer + (idx << q->umem->frame_size_log2)
			+ off);
}

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

#if DEBUG_HEXDUMP
static void hex_dump(void *pkt, size_t length, const char *prefix)
{
	int i = 0;
	const unsigned char *address = (unsigned char *)pkt;
	const unsigned char *line = address;
	size_t line_size = 32;
	unsigned char c;

	printf("length = %zu\n", length);
	printf("%s | ", prefix);
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
				printf("%s | ", prefix);
		}
	}
	printf("\n");
}
#endif

static size_t gen_eth_frame(char *frame)
{
	memcpy(frame, pkt_data, sizeof(pkt_data) - 1);
	return sizeof(pkt_data) - 1;
}

static struct xdp_umem *xsk_alloc_and_mem_reg_buffers(int sfd, size_t nbuffers)
{
	struct xdp_mr_req req = { .frame_size = FRAME_SIZE,
				  .data_headroom = DATA_HEADROOM };
	struct xdp_umem *umem;
	void *bufs;
	int ret;

	ret = posix_memalign((void **)&bufs, getpagesize(),
			     nbuffers * req.frame_size);
	lassert(ret == 0);

	umem = calloc(1, sizeof(*umem));
	lassert(umem);
	req.addr = (unsigned long)bufs;
	req.len = nbuffers * req.frame_size;
	ret = setsockopt(sfd, SOL_XDP, XDP_MEM_REG, &req, sizeof(req));
	lassert(ret == 0);

	umem->frame_size = FRAME_SIZE;
	umem->frame_size_log2 = log2(FRAME_SIZE);
	umem->buffer = bufs;
	umem->size = nbuffers * req.frame_size;
	umem->nframes = nbuffers;
	umem->mr_fd = sfd;

	if (opt_bench == BENCH_TXONLY) {
		char *pkt = bufs;
		int i = 0;

		while (i++ < nbuffers) {
			(void)gen_eth_frame(pkt);
			pkt += req.frame_size;
		}
	}

	return umem;
}

static struct xdp_queue_pair *xsk_configure(void)
{
	struct xdp_queue_pair *xqp;
	struct sockaddr_xdp sxdp;
	struct xdp_ring_req req;
	int sfd, ret, i;

	sfd = socket(PF_XDP, SOCK_RAW, 0);
	lassert(sfd >= 0);

	xqp = calloc(1, sizeof(*xqp));
	lassert(xqp);

	xqp->sfd = sfd;
	xqp->outstanding_tx = 0;

	xqp->umem = xsk_alloc_and_mem_reg_buffers(sfd, NUM_BUFFERS);
	lassert(xqp->umem);

	req.mr_fd = xqp->umem->mr_fd;
	req.desc_nr = NUM_DESCS;

	ret = setsockopt(sfd, SOL_XDP, XDP_RX_RING, &req, sizeof(req));
	lassert(ret == 0);

	ret = setsockopt(sfd, SOL_XDP, XDP_TX_RING, &req, sizeof(req));
	lassert(ret == 0);

	/* Rx */
	xqp->rx.ring = mmap(0, req.desc_nr * sizeof(struct xdp_desc),
			    PROT_READ | PROT_WRITE,
			    MAP_SHARED | MAP_LOCKED | MAP_POPULATE, sfd,
			    XDP_PGOFF_RX_RING);
	lassert(xqp->rx.ring != MAP_FAILED);

	xqp->rx.num_free = req.desc_nr;
	xqp->rx.ring_mask = req.desc_nr - 1;

	for (i = 0; i < (xqp->rx.ring_mask + 1); i++) {
		struct xdp_desc desc = {.idx = i};

		ret = xq_enq(&xqp->rx, &desc, 1);
		lassert(ret == 0);
	}

	/* Tx */
	xqp->tx.ring = mmap(0, req.desc_nr * sizeof(struct xdp_desc),
			    PROT_READ | PROT_WRITE,
			    MAP_SHARED | MAP_LOCKED | MAP_POPULATE, sfd,
			    XDP_PGOFF_TX_RING);
	lassert(xqp->tx.ring != MAP_FAILED);

	xqp->tx.num_free = req.desc_nr;
	xqp->tx.ring_mask = req.desc_nr - 1;

	sxdp.sxdp_family = PF_XDP;
	sxdp.sxdp_ifindex = opt_ifindex;
	sxdp.sxdp_queue_id = opt_queue;

	ret = bind(sfd, (struct sockaddr *)&sxdp, sizeof(sxdp));
	lassert(ret == 0);

	return xqp;
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

	if (running) {
		printf("running...");
		fflush(stdout);
	}
}

static void dump_stats(void)
{
	unsigned long stop_time = get_nsecs();
	long dt = stop_time - start_time;
	double rx_pps = rx_npkts * 1000000000. / dt;
	double tx_pps = tx_npkts * 1000000000. / dt;
	char *fmt = "%-15s %'-11.0f %'-11lu\n";

	printf("\n");
	print_benchmark(false);
	printf("\n");

	printf("%-15s %-11s %-11s %-11.2f\n", "", "pps", "pkts", dt / 1000000000.);
	printf(fmt, "rx", rx_pps, rx_npkts);
	printf(fmt, "tx", tx_pps, tx_npkts);
}

static void *poller(void *arg)
{
	(void)arg;
	for (;;) {
		dump_stats();
		sleep(1);
	}

	return NULL;
}

static void int_exit(int sig)
{
	(void)sig;
	dump_stats();
	set_link_xdp_fd(opt_ifindex, -1, opt_xdp_flags);
	exit(EXIT_SUCCESS);
}

static struct option long_options[] = {
	{"rxdrop", no_argument, 0, 'r'},
	{"txonly", no_argument, 0, 't'},
	{"l2fwd", no_argument, 0, 'l'},
	{"interface", required_argument, 0, 'i'},
	{"queue", required_argument, 0, 'q'},
	{"xdp-skb", no_argument, 0, 'S'},
	{"xdp-native", no_argument, 0, 'N'},
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
		"  -S, --xdp-skb=n	Use XDP skb-mod\n"
		"  -N, --xdp-native=n	Enfore XDP native mode\n"
		"\n";
	fprintf(stderr, str, prog);
	exit(EXIT_FAILURE);
}

static void parse_command_line(int argc, char **argv)
{
	int option_index, c;

	opterr = 0;

	for (;;) {
		c = getopt_long(argc, argv, "rtli:q:SN", long_options,
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
		case 'S':
			opt_xdp_flags |= XDP_FLAGS_SKB_MODE;
			break;
		case 'N':
			opt_xdp_flags |= XDP_FLAGS_DRV_MODE;
			break;
		default:
			usage(basename(argv[0]));
		}
	}

	opt_ifindex = if_nametoindex(opt_if);
	if (!opt_ifindex) {
		fprintf(stderr, "ERROR: interface \"%s\" does not exist\n", opt_if);
		usage(basename(argv[0]));
	}
}

static void kick_tx(int fd)
{
	int ret;

	for (;;) {
		ret = sendto(fd, NULL, 0, MSG_DONTWAIT, NULL, 0);
		if (ret >= 0 || errno == ENOBUFS)
			return;
		if (errno == EAGAIN)
			continue;
		lassert(0);
	}
}

static inline void complete_tx_l2fwd(struct xdp_queue_pair *q,
				     struct xdp_desc *descs)
{
	unsigned int rcvd;
	size_t ndescs;
	int ret;

	if (!q->outstanding_tx)
		return;

	ndescs = (q->outstanding_tx > BATCH_SIZE) ? BATCH_SIZE :
		q->outstanding_tx;

	/* re-add completed Tx buffers */
	rcvd = xq_deq(&q->tx, descs, ndescs);
	if (rcvd > 0) {
		/* No error checking on TX completion */
		ret = xq_enq(&q->rx, descs, rcvd);
		lassert(ret == 0);
		q->outstanding_tx -= rcvd;
		tx_npkts += rcvd;
	}
}

static inline void complete_tx_only(struct xdp_queue_pair *q,
				    struct xdp_desc *descs)
{
	unsigned int rcvd;
	size_t ndescs;

	if (!q->outstanding_tx)
		return;

	ndescs = (q->outstanding_tx > BATCH_SIZE) ? BATCH_SIZE :
		q->outstanding_tx;

	rcvd = xq_deq(&q->tx, descs, ndescs);
	if (rcvd > 0) {
		q->outstanding_tx -= rcvd;
		tx_npkts += rcvd;
	}
}

static void rx_drop(struct xdp_queue_pair *xqp)
{
	for (;;) {
		struct xdp_desc descs[BATCH_SIZE];
		unsigned int rcvd, i;
		int ret;

		for (;;) {
			rcvd = xq_deq(&xqp->rx, descs, BATCH_SIZE);
			if (rcvd > 0)
				break;
		}

		for (i = 0; i < rcvd; i++) {
			__u32 idx = descs[i].idx;

			lassert(idx < NUM_BUFFERS);
#if DEBUG_HEXDUMP
			char *pkt;
			char buf[32];

			pkt = xq_get_data(xqp, idx, descs[i].offset);
			sprintf(buf, "idx=%d", idx);
			hex_dump(pkt, descs[i].len, buf);
#endif
		}

		rx_npkts += rcvd;

		ret = xq_enq(&xqp->rx, descs, rcvd);
		lassert(ret == 0);
	}
}

static void gen_tx_descs(struct xdp_desc *descs, unsigned int idx,
			 unsigned int ndescs)
{
	int i;

	for (i = 0; i < ndescs; i++) {
		descs[i].idx = idx + i;
		descs[i].len = sizeof(pkt_data) - 1;
		descs[i].offset = 0;
		descs[i].flags = 0;
	}
}

static void tx_only(struct xdp_queue_pair *xqp)
{
	unsigned int idx = 0;

	for (;;) {
		struct xdp_desc descs[BATCH_SIZE];
		int ret;

		if (xqp->tx.num_free >= BATCH_SIZE) {
			gen_tx_descs(descs, idx, BATCH_SIZE);
			ret = xq_enq(&xqp->tx, descs, BATCH_SIZE);
			lassert(ret == 0);
			kick_tx(xqp->sfd);

			xqp->outstanding_tx += BATCH_SIZE;
			idx += BATCH_SIZE;
			idx %= NUM_BUFFERS;
		}

		complete_tx_only(xqp, descs);
	}
}

static void l2fwd(struct xdp_queue_pair *xqp)
{
	for (;;) {
		struct xdp_desc descs[BATCH_SIZE];
		unsigned int rcvd, i;
		int ret;

		for (;;) {
			complete_tx_l2fwd(xqp, descs);

			rcvd = xq_deq(&xqp->rx, descs, BATCH_SIZE);
			if (rcvd > 0)
				break;
		}

		for (i = 0; i < rcvd; i++) {
			char *pkt = xq_get_data(xqp, descs[i].idx,
						descs[i].offset);

			swap_mac_addresses(pkt);
#if DEBUG_HEXDUMP
			char buf[32];
			__u32 idx = descs[i].idx;

			sprintf(buf, "idx=%d", idx);
			hex_dump(pkt, descs[i].len, buf);
#endif
		}

		rx_npkts += rcvd;

		ret = xq_enq(&xqp->tx, descs, rcvd);
		lassert(ret == 0);
		xqp->outstanding_tx += rcvd;
		kick_tx(xqp->sfd);
	}
}

int main(int argc, char **argv)
{
	struct rlimit r = {RLIM_INFINITY, RLIM_INFINITY};
	struct xdp_queue_pair *xqp;
	char xdp_filename[256];
	pthread_t pt;
	int ret;

	parse_command_line(argc, argv);

	if (setrlimit(RLIMIT_MEMLOCK, &r)) {
		fprintf(stderr, "ERROR: setrlimit(RLIMIT_MEMLOCK) \"%s\"\n", strerror(errno));
		exit(EXIT_FAILURE);
	}

	snprintf(xdp_filename, sizeof(xdp_filename), "%s_kern.o", argv[0]);

	if (load_bpf_file(xdp_filename)) {
		fprintf(stderr, "ERROR: load_bpf_file %s\n", bpf_log_buf);
		exit(EXIT_FAILURE);
	}

	if (!prog_fd[0]) {
		fprintf(stderr, "ERROR: load_bpf_file: \"%s\"\n", strerror(errno));
		exit(EXIT_FAILURE);
	}

	if (set_link_xdp_fd(opt_ifindex, prog_fd[0], opt_xdp_flags) < 0) {
		fprintf(stderr, "ERROR: link set xdp fd failed\n");
		exit(EXIT_FAILURE);
	}

	xqp = xsk_configure();

	signal(SIGINT, int_exit);
	signal(SIGTERM, int_exit);
	signal(SIGABRT, int_exit);

	start_time = get_nsecs();

	setlocale(LC_ALL, "");

        ret = pthread_create(&pt, NULL, poller, NULL);
	lassert(ret == 0);

	if (opt_bench == BENCH_RXDROP)
		rx_drop(xqp);
	else if (opt_bench == BENCH_TXONLY)
		tx_only(xqp);
	else
		l2fwd(xqp);

	return 0;
}
