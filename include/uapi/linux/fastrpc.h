/* SPDX-License-Identifier: GPL-2.0 */

#ifndef __QCOM_FASTRPC_H__
#define __QCOM_FASTRPC_H__

#include <linux/types.h>

#define FASTRPC_IOCTL_INVOKE	_IOWR('R', 3, struct fastrpc_ioctl_invoke)
#define FASTRPC_IOCTL_INIT	_IOWR('R', 4, struct fastrpc_ioctl_init)

/* INIT a new process or attach to guestos */
#define FASTRPC_INIT_ATTACH      0
#define FASTRPC_INIT_CREATE      1
#define FASTRPC_INIT_CREATE_STATIC  2

#define remote_arg64_t    union remote_arg64

struct remote_buf64 {
	uint64_t pv;
	uint64_t len;
};

struct remote_dma_handle64 {
	int fd;
	uint32_t offset;
	uint32_t len;
};

union remote_arg64 {
	struct remote_buf64	buf;
	struct remote_dma_handle64 dma;
	uint32_t h;
};

#define remote_arg_t    union remote_arg

struct remote_buf {
	void *pv;		/* buffer pointer */
	size_t len;		/* length of buffer */
};

struct remote_dma_handle {
	int fd;
	uint32_t offset;
};

union remote_arg {
	struct remote_buf buf;	/* buffer info */
	struct remote_dma_handle dma;
	uint32_t h;		/* remote handle */
};

struct fastrpc_ioctl_invoke {
	uint32_t handle;	/* remote handle */
	uint32_t sc;		/* scalars describing the data */
	remote_arg_t *pra;	/* remote arguments list */
	int *fds;		/* fd list */
	unsigned int *attrs;	/* attribute list */
	unsigned int *crc;
};

struct fastrpc_ioctl_init {
	uint32_t flags;		/* one of FASTRPC_INIT_* macros */
	uintptr_t file;		/* pointer to elf file */
	uint32_t filelen;	/* elf file length */
	int32_t filefd;		/* ION fd for the file */
	uintptr_t mem;		/* mem for the PD */
	uint32_t memlen;	/* mem length */
	int32_t memfd;		/* fd for the mem */
	int attrs;
	unsigned int siglen;
};

#endif /* __QCOM_FASTRPC_H__ */
