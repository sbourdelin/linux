/*
 *  tpbench
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

#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <netinet/ether.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/shm.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define BATCH_SIZE 64 /* process pace */

#define NUM_BUFFERS 131072
#define FRAME_SIZE 2048

#define BLOCK_SIZE (1 << 22) /* V2/V3 */
#define NUM_DESCS 4096 /* V4 */

static unsigned long rx_npkts;
static unsigned long tx_npkts;
static unsigned long start_time;

/* cli options */
enum tpacket_version {
	PV2 = 0,
	PV3 = 1,
	PV4 = 2,
};

enum benchmark_type {
	BENCH_RXDROP = 0,
	BENCH_TXONLY = 1,
	BENCH_L2FWD = 2,
};

static enum tpacket_version opt_tpver = PV4;
static enum benchmark_type opt_bench = BENCH_RXDROP;
static const char *opt_if = "";
static int opt_zerocopy;

struct tpacket2_queue {
	void *ring;

	unsigned int last_used_idx;
	unsigned int ring_size;
	unsigned int frame_size_log2;
};

struct tp2_queue_pair {
	struct tpacket2_queue rx;
	struct tpacket2_queue tx;
	int sfd;
	const char *interface_name;
};

struct tpacket3_rx_queue {
	void *ring;
	struct tpacket3_hdr *frames[BATCH_SIZE];

	unsigned int last_used_idx;
	unsigned int ring_size; /* NB! blocks, not frames */
	unsigned int block_size_log2;

	struct tpacket3_hdr *last_frame;
	unsigned int npkts; /* >0 in block */
};

struct tp3_queue_pair {
	struct tpacket3_rx_queue rx;
	struct tpacket2_queue tx;
	int sfd;
	const char *interface_name;
};

struct tp4_umem {
	char *buffer;
	size_t size;
	unsigned int frame_size;
	unsigned int frame_size_log2;
	unsigned int nframes;
	int mr_fd;
	unsigned long free_stack[NUM_BUFFERS];
	unsigned int free_stack_idx;
};

struct tp4_queue_pair {
	struct tpacket4_queue rx;
	struct tpacket4_queue tx;
	int sfd;
	const char *interface_name;
	struct tp4_umem *umem;
};

struct benchmark {
	void *		(*configure)(const char *interface_name);
	void		(*rx)(void *queue_pair, unsigned int *start,
			      unsigned int *end);
	void *		(*get_data)(void *queue_pair, unsigned int idx,
				    unsigned int *len);
	unsigned long	(*get_data_desc)(void *queue_pair, unsigned int idx,
					 unsigned int *len,
					 unsigned short *offset);
	void		(*set_data_desc)(void *queue_pair, unsigned int idx,
					 unsigned long didx);
	void		(*process)(void *queue_pair, unsigned int start,
				   unsigned int end);
	void		(*rx_release)(void *queue_pair, unsigned int start,
				      unsigned int end);
	void		(*tx)(void *queue_pair, unsigned int start,
			      unsigned int end);
};

static char tx_frame[1024];
static unsigned int tx_frame_len;
static struct benchmark benchmark;

#define lassert(expr)							\
	do {								\
		if (!(expr)) {						\
			fprintf(stderr, "%s:%s:%i: Assertion failed: "	\
				#expr ": errno: %d/\"%s\"\n",		\
				__FILE__, __func__, __LINE__,		\
				errno, strerror(errno));		\
			exit(EXIT_FAILURE);				\
		}							\
	} while (0)

#define barrier() __asm__ __volatile__("" : : : "memory")
#define u_smp_rmb() barrier()
#define u_smp_wmb() barrier()
#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
#define log2(x)							\
	((unsigned int)(8 * sizeof(unsigned long long) -	\
			__builtin_clzll((x)) - 1))

#if 0
static void hex_dump(void *pkt, size_t length, const char *prefix)
{
	int i = 0;
	const unsigned char *address = (unsigned char *)pkt;
	const unsigned char *line = address;
	size_t line_size = 32;
	unsigned char c;

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

static size_t gen_eth_frame(char *frame, int data)
{
	static const char d[] =
		"\x3c\xfd\xfe\x9e\x7f\x71\xec\xb1\xd7\x98\x3a\xc0\x08\x00\x45\x00"
		"\x00\x2e\x00\x00\x00\x00\x40\x11\x88\x97\x05\x08\x07\x08\xc8\x14"
		"\x1e\x04\x10\x92\x10\x92\x00\x1a\x6d\xa3\x34\x33\x1f\x69\x40\x6b"
		"\x54\x59\xb6\x14\x2d\x11\x44\xbf\xaf\xd9\xbe\xaa";

	(void)data;
	memcpy(frame, d, sizeof(d) - 1);
	return sizeof(d) - 1;

#if 0
	/* XXX This generates "multicast packets" */
	struct ether_header *eh = (struct ether_header *)frame;
	size_t len = sizeof(struct ether_header);
	int i;

	for (i = 0; i < 6; i++) {
		eh->ether_shost[i] = i + 0x01;
		eh->ether_dhost[i] = i + 0x11;
	}
	eh->ether_type = htons(ETH_P_IP);

	for (i = 0; i < 46; i++)
		frame[len++] = data;

	return len;
#endif
}

static void setup_tx_frame(void)
{
	tx_frame_len = gen_eth_frame(tx_frame, 42);
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

static void rx_dummy(void *queue_pair, unsigned int *start, unsigned int *end)
{
	(void)queue_pair;
	*start = 0;
	*end = BATCH_SIZE;
}

static void rx_release_dummy(void *queue_pair, unsigned int start,
			     unsigned int end)
{
	(void)queue_pair;
	(void)start;
	(void)end;
}

static void *get_data_dummy(void *queue_pair, unsigned int idx,
			    unsigned int *len)
{
	(void)queue_pair;
	(void)idx;

	*len = tx_frame_len;

	return tx_frame;
}

#if 0
static void process_hexdump(void *queue_pair, unsigned int start,
			    unsigned int end)
{
	unsigned int len;
	void *data;

	while (start != end) {
		data = benchmark.get_data(queue_pair, start, &len);
		hex_dump(data, len, "Rx:");
		start++;
	}
}
#endif

static void process_swap_mac(void *queue_pair, unsigned int start,
			     unsigned int end)
{
	unsigned int len;
	void *data;

	while (start != end) {
		data = benchmark.get_data(queue_pair, start, &len);
		swap_mac_addresses(data);
		start++;
	}
}

static void run_benchmark(const char *interface_name)
{
	unsigned int start, end;
	struct tp2_queue_pair *qp;

	qp = benchmark.configure(interface_name);

	for (;;) {
		for (;;) {
			benchmark.rx(qp, &start, &end);
			if ((end - start) > 0)
				break;
			// XXX
			//if (poll)
			//	poll();
		}

		if (benchmark.process)
			benchmark.process(qp, start, end);

		benchmark.tx(qp, start, end);
	}
}

static unsigned long get_nsecs(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000000000UL + ts.tv_nsec;
}

static void *tp2_configure(const char *interface_name)
{
	int sfd, noqdisc, ret, ver = TPACKET_V2;
	struct tp2_queue_pair *tqp;
	struct tpacket_req req = {};
	struct sockaddr_ll ll;
	void *rxring;

	/* create PF_PACKET socket */
	sfd = socket(PF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
	lassert(sfd >= 0);
	ret = setsockopt(sfd, SOL_PACKET, PACKET_VERSION, &ver, sizeof(ver));
	lassert(ret == 0);

	tqp = calloc(1, sizeof(*tqp));
	lassert(tqp);

	tqp->sfd = sfd;
	tqp->interface_name = interface_name;

	req.tp_block_size = BLOCK_SIZE;
	req.tp_frame_size = FRAME_SIZE;
	req.tp_block_nr = NUM_BUFFERS * FRAME_SIZE / BLOCK_SIZE;
	req.tp_frame_nr = req.tp_block_nr * BLOCK_SIZE / FRAME_SIZE;

	ret = setsockopt(sfd, SOL_PACKET, PACKET_RX_RING, &req, sizeof(req));
	lassert(ret == 0);
	ret = setsockopt(sfd, SOL_PACKET, PACKET_TX_RING, &req, sizeof(req));
	lassert(ret == 0);

	rxring = mmap(0, 2 * req.tp_block_size * req.tp_block_nr,
		      PROT_READ | PROT_WRITE,
		      MAP_SHARED | MAP_LOCKED | MAP_POPULATE, sfd, 0);
	lassert(rxring != MAP_FAILED);

	tqp->rx.ring = rxring;
	tqp->rx.ring_size = NUM_BUFFERS;
	tqp->rx.frame_size_log2 = log2(req.tp_frame_size);

	tqp->tx.ring = rxring + req.tp_block_size * req.tp_block_nr;
	tqp->tx.ring_size = NUM_BUFFERS;
	tqp->tx.frame_size_log2 = log2(req.tp_frame_size);

	ll.sll_family = PF_PACKET;
	ll.sll_protocol = htons(ETH_P_ALL);
	ll.sll_ifindex = if_nametoindex(interface_name);
	ll.sll_hatype = 0;
	ll.sll_pkttype = 0;
	ll.sll_halen = 0;

	noqdisc = 1;
	ret = setsockopt(sfd, SOL_PACKET, PACKET_QDISC_BYPASS,
			 &noqdisc, sizeof(noqdisc));
	lassert(ret == 0);

	ret = bind(sfd, (struct sockaddr *)&ll, sizeof(ll));
	lassert(ret == 0);

	setup_tx_frame();

	return tqp;
}

static void tp2_rx(void *queue_pair, unsigned int *start, unsigned int *end)
{
	struct tpacket2_queue *rxq = &((struct tp2_queue_pair *)queue_pair)->rx;
	unsigned int batch = 0;

	*start = rxq->last_used_idx;
	*end = rxq->last_used_idx;

	for (;;) {
		unsigned int idx = *end & (rxq->ring_size - 1);
		struct tpacket2_hdr *hdr;

		hdr = (struct tpacket2_hdr *)(rxq->ring +
					      (idx << rxq->frame_size_log2));
		if ((hdr->tp_status & TP_STATUS_USER) != TP_STATUS_USER)
			break;

		(*end)++;
		if (++batch == BATCH_SIZE)
			break;
	}

	rxq->last_used_idx = *end;
	rx_npkts += (*end - *start);

	/* status before data */
	u_smp_rmb();
}

static void tp2_rx_release(void *queue_pair, unsigned int start,
			   unsigned int end)
{
	struct tpacket2_queue *rxq = &((struct tp2_queue_pair *)queue_pair)->rx;
	struct tpacket2_hdr *hdr;

	while (start != end) {
		hdr = (struct tpacket2_hdr *)(rxq->ring +
					      ((start & (rxq->ring_size - 1))
					       << rxq->frame_size_log2));

		hdr->tp_status = TP_STATUS_KERNEL;
		start++;
	}
}

static void *tp2_get_data(void *queue_pair, unsigned int idx, unsigned int *len)
{
	struct tpacket2_queue *rxq = &((struct tp2_queue_pair *)queue_pair)->rx;
	struct tpacket2_hdr *hdr;

	hdr = (struct tpacket2_hdr *)(rxq->ring + ((idx & (rxq->ring_size - 1))
						   << rxq->frame_size_log2));
	*len = hdr->tp_snaplen;

	return (char *)hdr + hdr->tp_mac;
}

static void tp2_tx(void *queue_pair, unsigned int start, unsigned int end)
{
	struct tp2_queue_pair *qp = queue_pair;
	struct tpacket2_queue *txq = &qp->tx;
	unsigned int len, curr = start;
	void *data;
	int ret;

	while (curr != end) {
		unsigned int idx = txq->last_used_idx & (txq->ring_size - 1);
		struct tpacket2_hdr *hdr;

		hdr = (struct tpacket2_hdr *)(txq->ring +
					      (idx << txq->frame_size_log2));
		if (hdr->tp_status &
		    (TP_STATUS_SEND_REQUEST | TP_STATUS_SENDING)) {
			break;
		}

		data = benchmark.get_data(queue_pair, curr, &len);

		hdr->tp_snaplen = len;
		hdr->tp_len = len;
		memcpy((char *)hdr + TPACKET2_HDRLEN -
		       sizeof(struct sockaddr_ll), data, len);

		u_smp_wmb();

		hdr->tp_status = TP_STATUS_SEND_REQUEST;

		txq->last_used_idx++;
		curr++;
	}

	ret = sendto(qp->sfd, NULL, 0, MSG_DONTWAIT, NULL, 0);
	if (!(ret >= 0 || errno == EAGAIN || errno == ENOBUFS))
		lassert(0);

	benchmark.rx_release(queue_pair, start, end);

	tx_npkts += (curr - start);
}

static void *tp3_configure(const char *interface_name)
{
	int sfd, noqdisc, ret, ver = TPACKET_V3;
	struct tp3_queue_pair *tqp;
	struct tpacket_req3 req = {};
	struct sockaddr_ll ll;
	void *rxring;

	unsigned int blocksiz = 1 << 22, framesiz = 1 << 11;
	unsigned int blocknum = 64;

	/* create PF_PACKET socket */
	sfd = socket(PF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
	lassert(sfd >= 0);
	ret = setsockopt(sfd, SOL_PACKET, PACKET_VERSION, &ver, sizeof(ver));
	lassert(ret == 0);

	tqp = calloc(1, sizeof(*tqp));
	lassert(tqp);

	tqp->sfd = sfd;
	tqp->interface_name = interface_name;

	/* XXX is is unfair to have 2 frames per block in V3? */
	req.tp_block_size = BLOCK_SIZE;
	req.tp_frame_size = FRAME_SIZE;
	req.tp_block_nr = NUM_BUFFERS * FRAME_SIZE / BLOCK_SIZE;
	req.tp_frame_nr = req.tp_block_nr * BLOCK_SIZE / FRAME_SIZE;
	req.tp_retire_blk_tov = 0;
	req.tp_sizeof_priv = 0;
	req.tp_feature_req_word = 0;

	ret = setsockopt(sfd, SOL_PACKET, PACKET_RX_RING, &req, sizeof(req));
	lassert(ret == 0);
	ret = setsockopt(sfd, SOL_PACKET, PACKET_TX_RING, &req, sizeof(req));
	lassert(ret == 0);

	rxring = mmap(0, 2 * req.tp_block_size * req.tp_block_nr,
		      PROT_READ | PROT_WRITE,
		      MAP_SHARED | MAP_LOCKED | MAP_POPULATE, sfd, 0);
	lassert(rxring != MAP_FAILED);

	tqp->rx.ring = rxring;
	tqp->rx.ring_size = blocknum;
	tqp->rx.block_size_log2 = log2(blocksiz);

	tqp->tx.ring = rxring + req.tp_block_size * req.tp_block_nr;
	tqp->tx.ring_size = (blocksiz * blocknum) / framesiz;
	tqp->tx.frame_size_log2 = log2(req.tp_frame_size);

	ll.sll_family = PF_PACKET;
	ll.sll_protocol = htons(ETH_P_ALL);
	ll.sll_ifindex = if_nametoindex(interface_name);
	ll.sll_hatype = 0;
	ll.sll_pkttype = 0;
	ll.sll_halen = 0;

	noqdisc = 1;
	ret = setsockopt(sfd, SOL_PACKET, PACKET_QDISC_BYPASS,
			 &noqdisc, sizeof(noqdisc));
	lassert(ret == 0);

	ret = bind(sfd, (struct sockaddr *)&ll, sizeof(ll));
	lassert(ret == 0);

	setup_tx_frame();

	return tqp;
}

static void tp3_rx(void *queue_pair, unsigned int *start, unsigned int *end)
{
	struct tpacket3_rx_queue *rxq =
		&((struct tp3_queue_pair *)queue_pair)->rx;
	unsigned int i, npkts = BATCH_SIZE;
	struct tpacket_block_desc *bd;
	bool no_more_frames = false;

	*start = 0;
	*end = 0;

	if (rxq->last_frame) {
		if (rxq->npkts <= BATCH_SIZE) {
			no_more_frames = true;
			npkts = rxq->npkts;
		}

		for (i = 0; i < npkts; i++) {
			rxq->last_frame = (struct tpacket3_hdr *)
					  ((char *)rxq->last_frame +
					   rxq->last_frame->tp_next_offset);
			rxq->frames[i] = rxq->last_frame;
		}

		if (no_more_frames)
			rxq->last_frame = NULL;

		rxq->npkts -= npkts;
		*end = npkts;
		rx_npkts += npkts;

		return;
	}

	bd = (struct tpacket_block_desc *)
	     (rxq->ring + ((rxq->last_used_idx & (rxq->ring_size - 1))
			   << rxq->block_size_log2));
	if ((bd->hdr.bh1.block_status & TP_STATUS_USER) != TP_STATUS_USER)
		return;

	u_smp_rmb();

	rxq->npkts = bd->hdr.bh1.num_pkts;
	if (rxq->npkts <= BATCH_SIZE) {
		no_more_frames = true;
		npkts = rxq->npkts;
	}

	rxq->last_frame = (struct tpacket3_hdr *)
			  ((char *)bd + bd->hdr.bh1.offset_to_first_pkt);
	rxq->frames[0] = rxq->last_frame;
	for (i = 1; i < npkts; i++) {
		rxq->last_frame = (struct tpacket3_hdr *)
				  ((char *)rxq->last_frame +
				   rxq->last_frame->tp_next_offset);
		rxq->frames[i] = rxq->last_frame;
	}

	if (no_more_frames)
		rxq->last_frame = NULL;

	*end = npkts;
	rx_npkts += npkts;
}

static void tp3_rx_release(void *queue_pair, unsigned int start,
			   unsigned int end)
{
	struct tpacket3_rx_queue *rxq =
		&((struct tp3_queue_pair *)queue_pair)->rx;
	struct tpacket_block_desc *bd;

	(void)start;
	(void)end;

	if (rxq->last_frame)
		return;

	bd = (struct tpacket_block_desc *)
	     (rxq->ring + ((rxq->last_used_idx & (rxq->ring_size - 1))
			   << rxq->block_size_log2));

	bd->hdr.bh1.block_status = TP_STATUS_KERNEL;
	rxq->last_used_idx++;
}

static void *tp3_get_data(void *queue_pair, unsigned int idx, unsigned int *len)
{
	struct tpacket3_rx_queue *rxq =
		&((struct tp3_queue_pair *)queue_pair)->rx;
	struct tpacket3_hdr *hdr = rxq->frames[idx];

	*len = hdr->tp_snaplen;

	return (char *)hdr + hdr->tp_mac;
}

static void tp3_tx(void *queue_pair, unsigned int start, unsigned int end)
{
	struct tp3_queue_pair *qp = queue_pair;
	struct tpacket2_queue *txq = &qp->tx;
	unsigned int len, curr = start;
	void *data;
	int ret;

	while (curr != end) {
		unsigned int idx = txq->last_used_idx & (txq->ring_size - 1);
		struct tpacket3_hdr *hdr;

		hdr = (struct tpacket3_hdr *)(txq->ring +
					      (idx << txq->frame_size_log2));
		if (hdr->tp_status &
		    (TP_STATUS_SEND_REQUEST | TP_STATUS_SENDING)) {
			break;
		}

		data = benchmark.get_data(queue_pair, curr, &len);

		hdr->tp_snaplen = len;
		hdr->tp_len = len;
		memcpy((char *)hdr + TPACKET3_HDRLEN -
		       sizeof(struct sockaddr_ll), data, len);

		u_smp_wmb();

		hdr->tp_status = TP_STATUS_SEND_REQUEST;

		txq->last_used_idx++;
		curr++;
	}

	ret = sendto(qp->sfd, NULL, 0, MSG_DONTWAIT, NULL, 0);
	if (!(ret >= 0 || errno == EAGAIN || errno == ENOBUFS))
		lassert(0);

	benchmark.rx_release(queue_pair, start, end);

	tx_npkts += (curr - start);
}

static inline void push_free_stack(struct tp4_umem *umem, unsigned long idx)
{
	umem->free_stack[--umem->free_stack_idx] = idx;
}

static inline unsigned long pop_free_stack(struct tp4_umem *umem)
{
	return	umem->free_stack[umem->free_stack_idx++];
}

static struct tp4_umem *alloc_and_register_buffers(size_t nbuffers)
{
	struct tpacket_memreg_req req = { .frame_size = FRAME_SIZE };
	struct tp4_umem *umem;
	size_t i;
	int fd, ret;
	void *bufs;

	ret = posix_memalign((void **)&bufs, getpagesize(),
			     nbuffers * req.frame_size);
	lassert(ret == 0);

	umem = calloc(1, sizeof(*umem));
	lassert(umem);
	fd = socket(PF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
	lassert(fd > 0);
	req.addr = (unsigned long)bufs;
	req.len = nbuffers * req.frame_size;
	ret = setsockopt(fd, SOL_PACKET, PACKET_MEMREG, &req, sizeof(req));
	lassert(ret == 0);

	umem->frame_size = FRAME_SIZE;
	umem->frame_size_log2 = log2(FRAME_SIZE);
	umem->buffer = bufs;
	umem->size = nbuffers * req.frame_size;
	umem->nframes = nbuffers;
	umem->mr_fd = fd;

	for (i = 0; i < nbuffers; i++)
		umem->free_stack[i] = i;

	for (i = 0; i < nbuffers; i++) {
		tx_frame_len = gen_eth_frame(bufs, 42);
		bufs += FRAME_SIZE;
	}

	return umem;
}

static inline int tp4q_enqueue(struct tpacket4_queue *q,
			       const struct tpacket4_desc *d,
			       unsigned int dcnt)
{
	unsigned int avail_idx = q->avail_idx;
	unsigned int i;
	int j;

	if (q->num_free < dcnt)
		return -ENOSPC;

	q->num_free -= dcnt;

	for (i = 0; i < dcnt; i++) {
		unsigned int idx = (avail_idx++) & q->ring_mask;

		q->ring[idx].idx = d[i].idx;
		q->ring[idx].len = d[i].len;
		q->ring[idx].offset = d[i].offset;
		q->ring[idx].error = 0;
	}
	u_smp_wmb();

	for (j = dcnt - 1; j >= 0; j--) {
		unsigned int idx = (q->avail_idx + j) & q->ring_mask;

		q->ring[idx].flags = d[j].flags | TP4_DESC_KERNEL;
	}
	q->avail_idx += dcnt;

	return 0;
}

static void *tp4_configure(const char *interface_name)
{
	int sfd, noqdisc, ret, ver = TPACKET_V4;
	struct tpacket_req4 req = {};
	struct tp4_queue_pair *tqp;
	struct sockaddr_ll ll;
	unsigned int i;
	void *rxring;

	/* create PF_PACKET socket */
	sfd = socket(PF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
	lassert(sfd >= 0);
	ret = setsockopt(sfd, SOL_PACKET, PACKET_VERSION, &ver, sizeof(ver));
	lassert(ret == 0);

	tqp = calloc(1, sizeof(*tqp));
	lassert(tqp);

	tqp->sfd = sfd;
	tqp->interface_name = interface_name;

	tqp->umem = alloc_and_register_buffers(NUM_BUFFERS);
	lassert(tqp->umem);

	req.mr_fd = tqp->umem->mr_fd;
	req.desc_nr = NUM_DESCS;
	ret = setsockopt(sfd, SOL_PACKET, PACKET_RX_RING, &req, sizeof(req));
	lassert(ret == 0);
	ret = setsockopt(sfd, SOL_PACKET, PACKET_TX_RING, &req, sizeof(req));
	lassert(ret == 0);

	rxring = mmap(0, 2 * req.desc_nr * sizeof(struct tpacket4_desc),
		      PROT_READ | PROT_WRITE,
		      MAP_SHARED | MAP_LOCKED | MAP_POPULATE, sfd, 0);
	lassert(rxring != MAP_FAILED);

	tqp->rx.ring = rxring;
	tqp->rx.num_free = req.desc_nr;
	tqp->rx.ring_mask = req.desc_nr - 1;

	tqp->tx.ring = &tqp->rx.ring[req.desc_nr];
	tqp->tx.num_free = req.desc_nr;
	tqp->tx.ring_mask = req.desc_nr - 1;

	ll.sll_family = PF_PACKET;
	ll.sll_protocol = htons(ETH_P_ALL);
	ll.sll_ifindex = if_nametoindex(interface_name);
	ll.sll_hatype = 0;
	ll.sll_pkttype = 0;
	ll.sll_halen = 0;

	noqdisc = 1;
	ret = setsockopt(sfd, SOL_PACKET, PACKET_QDISC_BYPASS,
			 &noqdisc, sizeof(noqdisc));
	lassert(ret == 0);

	ret = bind(sfd, (struct sockaddr *)&ll, sizeof(ll));
	lassert(ret == 0);

	if (opt_zerocopy > 0) {
		ret = setsockopt(sfd, SOL_PACKET, PACKET_ZEROCOPY,
				 &opt_zerocopy, sizeof(opt_zerocopy));
		lassert(ret == 0);
	}

	for (i = 0; i < (tqp->rx.ring_mask + 1)/4; i++) {
		struct tpacket4_desc desc = {};

		desc.idx = i;
		ret = tp4q_enqueue(&tqp->rx, &desc, 1);
		lassert(ret == 0);
	}

	return tqp;
}

static void tp4_rx(void *queue_pair, unsigned int *start, unsigned int *end)
{
	struct tpacket4_queue *q = &((struct tp4_queue_pair *)queue_pair)->rx;
	unsigned int idx, recv_size, last_used = q->last_used_idx;
	unsigned int uncleared = (q->avail_idx - last_used);

	*start = last_used;
	*end = last_used;
	recv_size = (uncleared < BATCH_SIZE) ? uncleared : BATCH_SIZE;

	idx = (last_used + recv_size - 1) & q->ring_mask;
	if (q->ring[idx].flags & TP4_DESC_KERNEL)
		return;

	*end += recv_size;
	rx_npkts += recv_size;
	q->num_free = recv_size;

	u_smp_rmb();
}

static inline void tp4_rx_release(void *queue_pair, unsigned int start,
				  unsigned int end)
{
	struct tp4_queue_pair *qp = queue_pair;
	struct tpacket4_queue *q = &qp->rx;
	struct tpacket4_desc *src, *dst;
	unsigned int nitems = end - start;

	while (nitems--) {
		dst = &q->ring[(q->avail_idx++) & q->ring_mask];
		src = &q->ring[start++ & q->ring_mask];
		*dst = *src;

		u_smp_wmb();

		dst->flags = TP4_DESC_KERNEL;
	}

	q->last_used_idx += q->num_free;
	q->num_free = 0;
}

static inline void *tp4_get_data(void *queue_pair, unsigned int idx,
				 unsigned int *len)
{
	struct tp4_queue_pair *qp = (struct tp4_queue_pair *)queue_pair;
	struct tp4_umem *umem = qp->umem;
	struct tpacket4_desc *d;

	d = &qp->rx.ring[idx & qp->rx.ring_mask];
	*len = d->len;

	return (char *)umem->buffer + (d->idx << umem->frame_size_log2)
		+ d->offset;
}


static inline unsigned long tp4_get_data_desc(void *queue_pair,
					      unsigned int idx,
					      unsigned int *len,
					      unsigned short *offset)
{
	struct tp4_queue_pair *qp = queue_pair;
	struct tpacket4_queue *q = &qp->rx;
	struct tpacket4_desc *d;

	d = &q->ring[idx & q->ring_mask];
	*len = d->len;
	*offset = d->offset;

	return d->idx;
}

static inline unsigned long tp4_get_data_desc_dummy(void *queue_pair,
						    unsigned int idx,
						    unsigned int *len,
						    unsigned short *offset)
{
	struct tp4_queue_pair *qp = queue_pair;

	(void)idx;

	*len = tx_frame_len;
	*offset = 0;

	return pop_free_stack(qp->umem);
}

static inline void tp4_set_data_desc(void *queue_pair, unsigned int idx,
				     unsigned long didx)
{
	struct tp4_queue_pair *qp = queue_pair;
	struct tpacket4_queue *q = &qp->rx;
	struct tpacket4_desc *d;

	d = &q->ring[idx & q->ring_mask];
	d->idx = didx;
}

static inline void tp4_set_data_desc_dummy(void *queue_pair, unsigned int idx,
					   unsigned long didx)
{
	struct tp4_queue_pair *qp = queue_pair;

	(void)idx;

	push_free_stack(qp->umem, didx);
}

static void tp4_tx(void *queue_pair, unsigned int start, unsigned int end)
{
	struct tp4_queue_pair *qp = (struct tp4_queue_pair *)queue_pair;
	struct tpacket4_queue *q = &qp->tx;
	unsigned int i, aidx, uidx, send_size, s, entries, ncleared = 0;
	unsigned long cleared[BATCH_SIZE];
	int ret;

	entries = end - start;

	if (q->num_free != NUM_DESCS) {
		for (i = 0; i < entries; i++) {
			uidx = q->last_used_idx & q->ring_mask;
			if (q->ring[uidx].flags & TP4_DESC_KERNEL)
				break;

			q->last_used_idx++;
			cleared[i] = q->ring[uidx].idx;
			q->num_free++;
			ncleared++;
		}
	}

	tx_npkts += ncleared;

	send_size = (q->num_free < entries) ? q->num_free : entries;
	i = 0;
	s = start;
	q->num_free -= send_size;

	while (send_size--) {
		aidx = q->avail_idx++ & q->ring_mask;

		q->ring[aidx].idx = benchmark.get_data_desc(
			qp, s, &q->ring[aidx].len,
			&q->ring[aidx].offset);
		if (i < ncleared)
			benchmark.set_data_desc(qp, s++, cleared[i++]);

		u_smp_wmb();

		q->ring[aidx].flags = TP4_DESC_KERNEL;
	}

	benchmark.rx_release(queue_pair, start, start + ncleared);

	ret = sendto(qp->sfd, NULL, 0, MSG_DONTWAIT, NULL, 0);
	if (!(ret >= 0 || errno == EAGAIN || errno == ENOBUFS))
		lassert(0);
}

static struct benchmark benchmarks[3][3] = {
	{ /* V2 */
		{ .configure = tp2_configure,
		  .rx = tp2_rx,
		  .get_data = NULL,
		  .get_data_desc = NULL,
		  .set_data_desc = NULL,
		  .process = NULL,
		  .rx_release = NULL,
		  .tx = tp2_rx_release,
		},
		{ .configure = tp2_configure,
		  .rx = rx_dummy,
		  .get_data = get_data_dummy,
		  .get_data_desc = NULL,
		  .set_data_desc = NULL,
		  .process = NULL,
		  .rx_release = rx_release_dummy,
		  .tx = tp2_tx,
		},
		{ .configure = tp2_configure,
		  .rx = tp2_rx,
		  .get_data = tp2_get_data,
		  .get_data_desc = NULL,
		  .set_data_desc = NULL,
		  .process = process_swap_mac,
		  .rx_release = tp2_rx_release,
		  .tx = tp2_tx,
		}
	},
	{ /* V3 */
		{ .configure = tp3_configure,
		  .rx = tp3_rx,
		  .get_data = NULL,
		  .get_data_desc = NULL,
		  .set_data_desc = NULL,
		  .process = NULL,
		  .rx_release = NULL,
		  .tx = tp3_rx_release,
		},
		{ .configure = tp3_configure,
		  .rx = rx_dummy,
		  .get_data = get_data_dummy,
		  .get_data_desc = NULL,
		  .set_data_desc = NULL,
		  .process = NULL,
		  .rx_release = rx_release_dummy,
		  .tx = tp3_tx,
		},
		{ .configure = tp3_configure,
		  .rx = tp3_rx,
		  .get_data = tp3_get_data,
		  .set_data_desc = NULL,
		  .get_data_desc = NULL,
		  .process = process_swap_mac,
		  .rx_release = tp3_rx_release,
		  .tx = tp3_tx,
		}
	},
	{ /* V4 */
		{ .configure = tp4_configure,
		  .rx = tp4_rx,
		  .get_data = NULL,
		  .get_data_desc = NULL,
		  .set_data_desc = NULL,
		  .process = NULL,
		  .rx_release = NULL,
		  .tx = tp4_rx_release,
		},
		{ .configure = tp4_configure,
		  .rx = rx_dummy,
		  .get_data = NULL,
		  .get_data_desc = tp4_get_data_desc_dummy,
		  .set_data_desc = tp4_set_data_desc_dummy,
		  .process = NULL,
		  .rx_release = rx_release_dummy,
		  .tx = tp4_tx,
		},
		{ .configure = tp4_configure,
		  .rx = tp4_rx,
		  .get_data = tp4_get_data,
		  .get_data_desc = tp4_get_data_desc,
		  .set_data_desc = tp4_set_data_desc,
		  .process = process_swap_mac,
		  .rx_release = tp4_rx_release,
		  .tx = tp4_tx,
		}
	}
};

static struct benchmark *get_benchmark(enum tpacket_version ver,
				       enum benchmark_type type)
{
	return &benchmarks[ver][type];
}




static struct option long_options[] = {
	{"version", required_argument, 0, 'v'},
	{"rxdrop", no_argument, 0, 'r'},
	{"txonly", no_argument, 0, 't'},
	{"l2fwd", no_argument, 0, 'l'},
	{"zerocopy", required_argument, 0, 'z'},
	{"interface", required_argument, 0, 'i'},
	{0, 0, 0, 0}
};

static void usage(void)
{
	const char *str =
		"  Usage: tpbench [OPTIONS]\n"
		"  Options:\n"
		"  -v, --version=n	Use tpacket version n (default 4)\n"
		"  -r, --rxdrop		Discard all incoming packets (default)\n"
		"  -t, --txonly		Only send packets\n"
		"  -l, --l2fwd		MAC swap L2 forwarding\n"
		"  -z, --zerocopy=n	Enable zero-copy on queue n\n"
		"  -i, --interface=n	Run on interface n\n"
		"\n";
	fprintf(stderr, "%s", str);
	exit(EXIT_FAILURE);
}

static void parse_command_line(int argc, char **argv)
{
	int option_index, c, version, ret;

	opterr = 0;

	for (;;) {
		c = getopt_long(argc, argv, "v:rtlz:i:", long_options,
				&option_index);
		if (c == -1)
			break;

		switch (c) {
		case 'v':
			version = atoi(optarg);
			if (version < 2 || version > 4) {
				fprintf(stderr,
					"ERROR: version has to be [2,4]\n");
				usage();
			}
			opt_tpver = version - 2;
			break;
		case 'r':
			opt_bench = BENCH_RXDROP;
			break;
		case 't':
			opt_bench = BENCH_TXONLY;
			break;
		case 'l':
			opt_bench = BENCH_L2FWD;
			break;
		case 'z':
			opt_zerocopy = atoi(optarg);
			break;
		case 'i':
			opt_if = optarg;
			break;
		default:
			usage();
		}
	}

	if (opt_zerocopy > 0 && opt_tpver != PV4) {
		fprintf(stderr, "ERROR: version 4 required for zero-copy\n");
		usage();
	}

	ret = if_nametoindex(opt_if);
	if (!ret) {
		fprintf(stderr, "ERROR: interface \"%s\" does not exist\n",
			opt_if);
		usage();
	}
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

	printf("%s v%d %s ", opt_if, opt_tpver + 2, bench_str);
	if (opt_zerocopy > 0)
		printf("zc ");
	else
		printf("   ");

	if (running) {
		printf("running...");
		fflush(stdout);
	}
}

static void sigdie(int sig)
{
	unsigned long stop_time = get_nsecs();
	long dt = stop_time - start_time;
	(void)sig;

	double rx_pps = rx_npkts * 1000000000. / dt;
	double tx_pps = tx_npkts * 1000000000. / dt;

	printf("\r");
	print_benchmark(false);
	printf("duration %4.2fs rx: %16lupkts @ %16.2fpps tx: %16lupkts @ %16.2fpps.\n",
	       dt / 1000000000., rx_npkts, rx_pps, tx_npkts, tx_pps);

	exit(EXIT_SUCCESS);
}

int main(int argc, char **argv)
{
	signal(SIGINT, sigdie);
	parse_command_line(argc, argv);
	print_benchmark(true);
	benchmark = *get_benchmark(opt_tpver, opt_bench);
	start_time = get_nsecs();
	run_benchmark(opt_if);

	return 0;
}
