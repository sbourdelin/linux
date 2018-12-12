// SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)

/*
 * AF_XDP user-space access library.
 *
 * Copyright(c) 2018 Intel Corporation.
 */

#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <asm/barrier.h>
#include <linux/compiler.h>
#include <linux/if_xdp.h>
#include <linux/list.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/types.h>

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

/* This has to be a power of 2 for performance reasons. */
#define HASH_TABLE_ENTRIES 128

struct xsk_umem_info {
	struct xsk_prod_ring *fq;
	struct xsk_cons_ring *cq;
	char *umem_area;
	struct list_head list;
	struct xsk_umem_config config;
	int fd;
	int refcount;
};

struct xsk_xdp_socket_info {
	struct xsk_cons_ring *rx;
	struct xsk_prod_ring *tx;
	__u64 outstanding_tx;
	struct list_head list;
	struct xsk_umem_info *umem;
	struct xsk_xdp_socket_config config;
	int fd;
};

static struct xsk_xdp_socket_info *xsk_hash_table[HASH_TABLE_ENTRIES];
static struct xsk_umem_info *umem_hash_table[HASH_TABLE_ENTRIES];

/* For 32-bit systems, we need to use mmap2 as the offsets are 64-bit.
 * Unfortunately, it is not part of glibc.
 */
static inline void *xsk_mmap(void *addr, size_t length, int prot, int flags,
			     int fd, __u64 offset)
{
#ifdef __NR_mmap2
	unsigned int page_shift = __builtin_ffs(getpagesize()) - 1;
	long ret = syscall(__NR_mmap2, addr, length, prot, flags, fd,
			   (off_t)(offset >> page_shift));

	return (void *)ret;
#else
	return mmap(addr, length, prot, flags, fd, offset);
#endif
}

static __u32 xsk_prod_nb_free(struct xsk_prod_ring *r, __u32 nb)
{
	__u32 free_entries = r->cached_cons - r->cached_prod;

	if (free_entries >= nb)
		return free_entries;

	/* Refresh the local tail pointer.
	 * cached_cons is r->size bigger than the real consumer pointer so
	 * that this addition can be avoided in the more frequently
	 * executed code that computs free_entries in the beginning of
	 * this function. Without this optimization it whould have been
	 * free_entries = r->cached_prod - r->cached_cons + r->size.
	 */
	r->cached_cons = *r->consumer + r->size;

	return r->cached_cons - r->cached_prod;
}

static __u32 xsk_cons_nb_avail(struct xsk_cons_ring *r, __u32 nb)
{
	__u32 entries = r->cached_prod - r->cached_cons;

	if (entries == 0) {
		r->cached_prod = *r->producer;
		entries = r->cached_prod - r->cached_cons;
	}

	return (entries > nb) ? nb : entries;
}

size_t xsk__reserve_prod(struct xsk_prod_ring *prod, size_t nb, __u32 *idx)
{
	if (unlikely(xsk_prod_nb_free(prod, nb) < nb))
		return 0;

	*idx = prod->cached_prod;
	prod->cached_prod += nb;

	return nb;
}

void xsk__submit_prod(struct xsk_prod_ring *prod)
{
	/* Make sure everything has been written to the ring before signalling
	 * this to the kernel.
	 */
	smp_wmb();

	*prod->producer = prod->cached_prod;
}

size_t xsk__peek_cons(struct xsk_cons_ring *cons, size_t nb, __u32 *idx)
{
	size_t entries = xsk_cons_nb_avail(cons, nb);

	if (likely(entries > 0)) {
		/* Make sure we do not speculatively read the data before
		 * we have received the packet buffers from the ring.
		 */
		smp_rmb();

		*idx = cons->cached_cons;
		cons->cached_cons += entries;
	}

	return entries;
}

void xsk__release_cons(struct xsk_cons_ring *cons)
{
	*cons->consumer = cons->cached_cons;
}

void *xsk__get_data(void *umem_area, __u64 addr)
{
	return &((char *)umem_area)[addr];
}

static bool xsk_page_aligned(void *buffer)
{
	unsigned long addr = (unsigned long)buffer;

	return !(addr & (getpagesize() - 1));
}

/* Since the file descriptors are generally allocated sequentially, and also
 * for performance reasons, we pick the simplest possible hash function:
 * just a single "and" operation (from the modulo operator).
 */
static void xsk_hash_insert_umem(int fd, struct xsk_umem_info *umem)
{
	struct xsk_umem_info *umem_in_hash =
		umem_hash_table[fd % HASH_TABLE_ENTRIES];

	if (umem_in_hash) {
		list_add_tail(&umem->list, &umem_in_hash->list);
		return;
	}

	INIT_LIST_HEAD(&umem->list);
	umem_hash_table[fd % HASH_TABLE_ENTRIES] = umem;
}

static struct xsk_umem_info *xsk_hash_find_umem(int fd)
{
	struct xsk_umem_info *umem = umem_hash_table[fd % HASH_TABLE_ENTRIES];

	while (umem && umem->fd != fd)
		umem = list_next_entry(umem, list);

	return umem;
}

static void xsk_hash_remove_umem(int fd)
{
	struct xsk_umem_info *umem = umem_hash_table[fd % HASH_TABLE_ENTRIES];

	while (umem && umem->fd != fd)
		umem = list_next_entry(umem, list);

	if (umem) {
		if (list_empty(&umem->list)) {
			umem_hash_table[fd % HASH_TABLE_ENTRIES] = NULL;
			return;
		}

		if (umem == umem_hash_table[fd % HASH_TABLE_ENTRIES])
			umem_hash_table[fd % HASH_TABLE_ENTRIES] =
				list_next_entry(umem, list);
		list_del(&umem->list);
	}
}

static void xsk_hash_insert_xdp_socket(int fd, struct xsk_xdp_socket_info *xsk)
{
	struct xsk_xdp_socket_info *xsk_in_hash =
		xsk_hash_table[fd % HASH_TABLE_ENTRIES];

	if (xsk_in_hash) {
		list_add_tail(&xsk->list, &xsk_in_hash->list);
		return;
	}

	INIT_LIST_HEAD(&xsk->list);
	xsk_hash_table[fd % HASH_TABLE_ENTRIES] = xsk;
}

static struct xsk_xdp_socket_info *xsk_hash_find_xdp_socket(int fd)
{
	struct xsk_xdp_socket_info *xsk =
		xsk_hash_table[fd % HASH_TABLE_ENTRIES];

	while (xsk && xsk->fd != fd)
		xsk = list_next_entry(xsk, list);

	return xsk;
}

static void xsk_hash_remove_xdp_socket(int fd)
{
	struct xsk_xdp_socket_info *xsk =
		xsk_hash_table[fd % HASH_TABLE_ENTRIES];

	while (xsk && xsk->fd != fd)
		xsk = list_next_entry(xsk, list);

	if (xsk) {
		if (list_empty(&xsk->list)) {
			xsk_hash_table[fd % HASH_TABLE_ENTRIES] = NULL;
			return;
		}

		if (xsk == xsk_hash_table[fd % HASH_TABLE_ENTRIES])
			xsk_hash_table[fd % HASH_TABLE_ENTRIES] =
				list_next_entry(xsk, list);
		list_del(&xsk->list);
	}
}

static void xsk_set_umem_config(struct xsk_umem_config *config,
				struct xsk_umem_config *usr_config)
{
	if (!usr_config) {
		config->fq_size = XSK__DEFAULT_NUM_DESCS;
		config->cq_size = XSK__DEFAULT_NUM_DESCS;
		config->frame_size = XSK__DEFAULT_FRAME_SIZE;
		config->frame_headroom = XSK__DEFAULT_FRAME_HEADROOM;
		return;
	}

	config->fq_size = usr_config->fq_size;
	config->cq_size = usr_config->cq_size;
	config->frame_size = usr_config->frame_size;
	config->frame_headroom = usr_config->frame_headroom;
}

static void xsk_set_xdp_socket_config(struct xsk_xdp_socket_config *config,
				      struct xsk_xdp_socket_config *usr_config)
{
	if (!usr_config) {
		config->rx_size = XSK__DEFAULT_NUM_DESCS;
		config->tx_size = XSK__DEFAULT_NUM_DESCS;
		return;
	}

	config->rx_size = usr_config->rx_size;
	config->tx_size = usr_config->tx_size;
}

int xsk__create_umem(void *umem_area, __u64 size, struct xsk_prod_ring *fq,
		     struct xsk_cons_ring *cq,
		     struct xsk_umem_config *usr_config)
{
	struct xdp_mmap_offsets off;
	struct xsk_umem_info *umem;
	struct xdp_umem_reg mr;
	socklen_t optlen;
	int err, fd;
	void *map;

	if (!umem_area)
		return -EFAULT;
	if (!size && !xsk_page_aligned(umem_area))
		return -EINVAL;

	fd = socket(AF_XDP, SOCK_RAW, 0);
	if (fd < 0)
		return -errno;

	umem = calloc(1, sizeof(*umem));
	if (!umem) {
		err = -ENOMEM;
		goto out_socket;
	}

	umem->umem_area = umem_area;
	umem->fd = fd;
	xsk_set_umem_config(&umem->config, usr_config);

	mr.addr = (uintptr_t)umem_area;
	mr.len = size;
	mr.chunk_size = umem->config.frame_size;
	mr.headroom = umem->config.frame_headroom;

	err = setsockopt(fd, SOL_XDP, XDP_UMEM_REG, &mr, sizeof(mr));
	if (err) {
		err = -errno;
		goto out_umem_alloc;
	}
	err = setsockopt(fd, SOL_XDP, XDP_UMEM_FILL_RING,
			 &umem->config.fq_size, sizeof(umem->config.fq_size));
	if (err) {
		err = -errno;
		goto out_umem_alloc;
	}
	err = setsockopt(fd, SOL_XDP, XDP_UMEM_COMPLETION_RING,
			 &umem->config.cq_size, sizeof(umem->config.cq_size));
	if (err) {
		err = -errno;
		goto out_umem_alloc;
	}

	optlen = sizeof(off);
	err = getsockopt(fd, SOL_XDP, XDP_MMAP_OFFSETS, &off, &optlen);
	if (err) {
		err = -errno;
		goto out_umem_alloc;
	}

	map = xsk_mmap(NULL, off.fr.desc + umem->config.fq_size * sizeof(__u64),
		       PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE,
		       fd, XDP_UMEM_PGOFF_FILL_RING);
	if (map == MAP_FAILED) {
		err = -errno;
		goto out_umem_alloc;
	}

	umem->fq = fq;
	fq->mask = umem->config.fq_size - 1;
	fq->size = umem->config.fq_size;
	fq->producer = map + off.fr.producer;
	fq->consumer = map + off.fr.consumer;
	fq->ring = map + off.fr.desc;
	fq->cached_cons = umem->config.fq_size;

	map = xsk_mmap(NULL, off.cr.desc + umem->config.cq_size * sizeof(__u64),
		    PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE,
		    fd, XDP_UMEM_PGOFF_COMPLETION_RING);
	if (map == MAP_FAILED) {
		err = -errno;
		goto out_mmap;
	}

	umem->cq = cq;
	cq->mask = umem->config.cq_size - 1;
	cq->size = umem->config.cq_size;
	cq->producer = map + off.cr.producer;
	cq->consumer = map + off.cr.consumer;
	cq->ring = map + off.cr.desc;

	xsk_hash_insert_umem(fd, umem);
	return fd;

out_mmap:
	munmap(umem->fq, off.fr.desc + umem->config.fq_size * sizeof(__u64));
out_umem_alloc:
	free(umem);
out_socket:
	close(fd);
	return err;
}

int xsk__create_xdp_socket(int umem_fd, struct xsk_cons_ring *rx,
			   struct xsk_prod_ring *tx,
			   struct xsk_xdp_socket_config *usr_config)
{
	struct xsk_xdp_socket_info *xsk;
	struct xdp_mmap_offsets off;
	struct xsk_umem_info *umem;
	socklen_t optlen;
	int err, fd;
	void *map;

	umem = xsk_hash_find_umem(umem_fd);
	if (!umem)
		return -EBADF;

	if (umem->refcount++ == 0) {
		fd = umem_fd;
	} else {
		fd = socket(AF_XDP, SOCK_RAW, 0);
		if (fd < 0)
			return -errno;
	}

	xsk = calloc(1, sizeof(*xsk));
	if (!xsk) {
		err = -ENOMEM;
		goto out_socket;
	}

	xsk->fd = fd;
	xsk->outstanding_tx = 0;
	xsk_set_xdp_socket_config(&xsk->config, usr_config);

	if (rx) {
		err = setsockopt(fd, SOL_XDP, XDP_RX_RING,
				 &xsk->config.rx_size,
				 sizeof(xsk->config.rx_size));
		if (err) {
			err = -errno;
			goto out_xsk_alloc;
		}
	}
	if (tx) {
		err = setsockopt(fd, SOL_XDP, XDP_TX_RING,
				 &xsk->config.tx_size,
				 sizeof(xsk->config.tx_size));
		if (err) {
			err = -errno;
			goto out_xsk_alloc;
		}
	}

	optlen = sizeof(off);
	err = getsockopt(fd, SOL_XDP, XDP_MMAP_OFFSETS, &off, &optlen);
	if (err) {
		err = -errno;
		goto out_xsk_alloc;
	}

	if (rx) {
		map = xsk_mmap(NULL, off.rx.desc +
			       xsk->config.rx_size * sizeof(struct xdp_desc),
			       PROT_READ | PROT_WRITE,
			       MAP_SHARED | MAP_POPULATE,
			       fd, XDP_PGOFF_RX_RING);
		if (map == MAP_FAILED) {
			err = -errno;
			goto out_xsk_alloc;
		}

		rx->mask = xsk->config.rx_size - 1;
		rx->size = xsk->config.rx_size;
		rx->producer = map + off.rx.producer;
		rx->consumer = map + off.rx.consumer;
		rx->ring = map + off.rx.desc;
	}
	xsk->rx = rx;

	if (tx) {
		map = xsk_mmap(NULL, off.tx.desc +
			       xsk->config.tx_size * sizeof(struct xdp_desc),
			       PROT_READ | PROT_WRITE,
			       MAP_SHARED | MAP_POPULATE,
			       fd, XDP_PGOFF_TX_RING);
		if (map == MAP_FAILED) {
			err = -errno;
			goto out_mmap;
		}

		tx->mask = xsk->config.tx_size - 1;
		tx->size = xsk->config.tx_size;
		tx->producer = map + off.tx.producer;
		tx->consumer = map + off.tx.consumer;
		tx->ring = map + off.tx.desc;
		tx->cached_cons = xsk->config.tx_size;
	}
	xsk->tx = tx;

	xsk_hash_insert_xdp_socket(fd, xsk);
	return fd;

out_mmap:
	if (rx)
		munmap(xsk->rx,
		       off.rx.desc +
		       xsk->config.rx_size * sizeof(struct xdp_desc));
out_xsk_alloc:
	free(xsk);
out_socket:
	if (--umem->refcount)
		close(fd);
	return err;
}

int xsk__delete_umem(int fd)
{
	struct xdp_mmap_offsets off;
	struct xsk_umem_info *umem;
	socklen_t optlen;
	int err;

	umem = xsk_hash_find_umem(fd);
	if (!umem)
		return -EBADF;

	if (umem->refcount)
		return -EBUSY;

	optlen = sizeof(off);
	err = getsockopt(fd, SOL_XDP, XDP_MMAP_OFFSETS, &off, &optlen);
	if (!err) {
		munmap(umem->fq->ring,
		       off.fr.desc + umem->config.fq_size * sizeof(__u64));
		munmap(umem->cq->ring,
		       off.cr.desc + umem->config.cq_size * sizeof(__u64));
	}

	xsk_hash_remove_umem(fd);
	close(fd);
	free(umem);

	return 0;
}

int xsk__delete_xdp_socket(int fd)
{
	struct xsk_xdp_socket_info *xsk;
	struct xdp_mmap_offsets off;
	socklen_t optlen;
	int err;

	xsk = xsk_hash_find_xdp_socket(fd);
	if (!xsk)
		return -EBADF;

	optlen = sizeof(off);
	err = getsockopt(fd, SOL_XDP, XDP_MMAP_OFFSETS, &off, &optlen);
	if (!err) {
		if (xsk->rx)
			munmap(xsk->rx->ring,
			       off.rx.desc +
			       xsk->config.rx_size * sizeof(struct xdp_desc));
		if (xsk->tx)
			munmap(xsk->tx->ring,
			       off.tx.desc +
			       xsk->config.tx_size * sizeof(struct xdp_desc));
	}

	xsk->umem->refcount--;
	xsk_hash_remove_xdp_socket(fd);
	/* Do not close the fd that also has an associated umem connected
	 * to it.
	 */
	if (xsk->fd != xsk->umem->fd)
		close(fd);
	free(xsk);

	return 0;
}
