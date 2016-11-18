/*
 * Copyright (C) 2016 Cavium, Inc.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of version 2 of the GNU General Public License
 * as published by the Free Software Foundation.
 */

#ifndef __REQUEST_MANGER_H
#define __REQUEST_MANGER_H

#include "cpt_common.h"

#define TIME_IN_RESET_COUNT  5
#define COMPLETION_CODE_SIZE 8
#define COMPLETION_CODE_INIT 0

#if defined(__BIG_ENDIAN_BITFIELD)
#define COMPLETION_CODE_SHIFT     56
#else
#define COMPLETION_CODE_SHIFT      0
#endif

#define PENDING_THOLD  100

#define MAX_SG_IN_OUT_CNT (25u)
#define SG_LIST_HDR_SIZE  (8u)

union data_ptr {
	uint64_t addr64;
	uint8_t *addr;
};

struct cpt_buffer {
	uint8_t type; /**< How to interpret the buffer */
	uint8_t reserved0;
	uint16_t size; /**< Sizeof of the data */
	uint16_t offset;
	uint16_t reserved1;
	union data_ptr ptr; /**< Pointer to data */
};

union ctrl_info {
	uint32_t flags;
	struct {
#if defined(__BIG_ENDIAN_BITFIELD)
		uint32_t reserved0:24;
		uint32_t grp:3; /**< Group bits */
		uint32_t dma_mode:2; /**< DMA mode */
		uint32_t req_mode:2; /**< Requeset mode BLOCKING/NONBLOCKING*/
		uint32_t se_req:1;/**< To SE core */
#else
		uint32_t se_req:1; /**< To SE core */
		uint32_t req_mode:2; /**< Requeset mode BLOCKING/NONBLOCKING*/
		uint32_t dma_mode:2; /**< DMA mode */
		uint32_t grp:3; /* Group bits */
		uint32_t reserved0:24;
#endif
	} s;
};

union opcode_info {
	uint16_t flags;
	struct {
		uint8_t major;
		uint8_t minor;
	} s;
};

struct cptvf_request {
	union opcode_info opcode;
	uint16_t param1;
	uint16_t param2;
	uint16_t dlen;
};

#define MAX_BUF_CNT	16

struct cpt_request_info {
	uint8_t incnt; /**< Number of input buffers */
	uint8_t outcnt; /**< Number of output buffers */
	uint8_t ctxl; /**< Context length, if 0, then INLINE */
	uint16_t rlen; /**< Output length */
	union ctrl_info ctrl; /**< User control information */

	struct cptvf_request req; /**< Request Information (Core specific) */

	uint64_t handle; /**< key/context handle */
	uint64_t request_id; /**< Request ID */

	struct cpt_buffer in[MAX_BUF_CNT];
	struct cpt_buffer out[MAX_BUF_CNT];

	void (*callback)(int, void *); /**< Kernel ASYNC request callabck */
	void *callback_arg; /**< Kernel ASYNC request callabck arg */

	uint32_t status; /**< Request status */
};

enum {
	UNIT_8_BIT,
	UNIT_16_BIT,
	UNIT_32_BIT,
	UNIT_64_BIT
};

struct sglist_component {
	union {
		uint64_t len;
		struct {
			uint16_t len0;
			uint16_t len1;
			uint16_t len2;
			uint16_t len3;
		} s;
	} u;
	uint64_t ptr0;
	uint64_t ptr1;
	uint64_t ptr2;
	uint64_t ptr3;
};

struct buf_ptr {
	uint8_t *vptr;
	dma_addr_t dma_addr;
	uint16_t size;
};

#define MAX_OUTCNT	10
#define MAX_INCNT	10

struct cpt_info_buffer {
	struct cpt_vf *cptvf;
	uint8_t req_type;
	uint8_t dma_mode;

	uint16_t opcode;
	uint8_t queue;
	uint8_t extra_time;
	uint8_t is_ae;

	uint16_t glist_cnt;
	uint16_t slist_cnt;
	uint16_t g_size;
	uint16_t s_size;

	uint32_t outcnt;
	uint32_t status;

	unsigned long time_in;
	uint64_t request_id;

	uint32_t dlen;
	uint32_t rlen;
	uint32_t total_in;
	uint32_t total_out;
	uint64_t dptr_baddr;
	uint64_t rptr_baddr;
	uint64_t comp_baddr;
	uint8_t *in_buffer;
	uint8_t *out_buffer;
	uint8_t *gather_components;
	uint8_t *scatter_components;
	uint32_t outsize[MAX_OUTCNT];
	uint32_t outunit[MAX_OUTCNT];
	uint8_t *outptr[MAX_OUTCNT];

	struct pending_entry *pentry;
	volatile uint64_t *completion_addr;
	volatile uint64_t *alternate_caddr;

	struct buf_ptr glist_ptr[MAX_INCNT];
	struct buf_ptr slist_ptr[MAX_OUTCNT];
};

/*
 * CPT_INST_S software command definitions
 * Words EI (0-3)
 */
union vq_cmd_word0 {
	uint64_t u64;
	struct {
		uint16_t opcode;
		uint16_t param1;
		uint16_t param2;
		uint16_t dlen;
	} s;
};

union vq_cmd_word3 {
	uint64_t u64;
	struct {
#if defined(__BIG_ENDIAN_BITFIELD)
		uint64_t grp	: 3;
		uint64_t cptr	: 61;
#else
		uint64_t cptr	: 61;
		uint64_t grp	: 3;
#endif
	} s;
};

struct cpt_vq_command {
	union vq_cmd_word0 cmd;
	uint64_t dptr;
	uint64_t rptr;
	union vq_cmd_word3 cptr;
};

#if defined(__BIG_ENDIAN_BITFIELD)
#define set_scatter_chunks(value, scatter_component)	{\
	(value) |= (((uint64_t)scatter_component) << 25); }
#else
#define set_scatter_chunks(value, scatter_component)	{\
	(value) |= (((uint64_t)scatter_component) << 32); }
#endif

void vq_post_process(struct cpt_vf *cptvf, uint32_t qno);
int32_t process_request(struct cpt_vf *cptvf,
			struct cpt_request_info *kern_req);
#endif /* __REQUEST_MANGER_H */
