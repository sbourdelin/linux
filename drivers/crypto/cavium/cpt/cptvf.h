/*
 * Copyright (C) 2016 Cavium, Inc.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of version 2 of the GNU General Public License
 * as published by the Free Software Foundation.
 */

#ifndef __CPTVF_H
#define __CPTVF_H

#include <linux/list.h>
#include "cpt_common.h"

struct command_chunk {
	uint8_t *head; /* 128-byte aligned real_vaddr */
	uint8_t *real_vaddr; /* Virtual address after dma_alloc_consistent */
	dma_addr_t dma_addr; /* 128-byte aligned real_dma_addr */
	dma_addr_t real_dma_addr; /* DMA address after dma_alloc_consistent */
	uint32_t size; /* Chunk size, max CPT_INST_CHUNK_MAX_SIZE */
	struct hlist_node nextchunk;
};

struct iq_stats {
	atomic64_t instr_posted;
	atomic64_t instr_dropped;
};

/**
 * comamnd queue structure
 */
struct command_queue {
	spinlock_t lock; /* command queue lock */
	uint32_t idx; /* Command queue host write idx */
	uint32_t dbell_count; /* outstanding commands */
	uint32_t nchunks; /* Number of command chunks */
	struct command_chunk *qhead;	/* Command queue head, instructions
					 * are inserted here
					 */
	struct hlist_head chead;
	struct iq_stats stats; /* Queue statistics */
};

struct command_qinfo {
	uint32_t dbell_thold; /* Command queue doorbell threshold */
	uint32_t cmd_size; /* Command size (32/64-Byte) */
	uint32_t qchunksize; /* Command queue chunk size configured by user */
	struct command_queue queue[DEFAULT_DEVICE_QUEUES];
};

/**
 * pending entry structure
 */
struct pending_entry {
	uint8_t busy; /* Entry status (free/busy) */
	uint8_t done;
	uint8_t is_ae;

	volatile uint64_t *completion_addr; /* Completion address */
	void *post_arg;
	void (*callback)(int, void *); /* Kernel ASYNC request callabck */
	void *callback_arg; /* Kernel ASYNC request callabck arg */
};

/**
 * pending queue structure
 */
struct pending_queue {
	struct pending_entry *head;	/* head of the queue */
	uint32_t front; /* Process work from here */
	uint32_t rear; /* Append new work here */
	atomic64_t pending_count;
	spinlock_t lock; /* Queue lock */
};

struct pending_qinfo {
	uint32_t nr_queues;	/* Number of queues supported */
	uint32_t qlen; /* Queue length */
	struct pending_queue queue[DEFAULT_DEVICE_QUEUES];
};

#define for_each_pending_queue(qinfo, q, i)	\
	for (i = 0, q = &qinfo->queue[i]; i < qinfo->nr_queues; i++, \
	     q = &qinfo->queue[i])

/**
 * CPT VF device structure
 */
struct cpt_vf {
	uint32_t chip_id; /* CPT Device ID */
	uint16_t flags; /* Flags to hold device status bits */
	uint8_t vfid; /* Device Index 0...CPT_MAX_VF_NUM */
	uint8_t vftype; /* VF type of SE_TYPE(1) or AE_TYPE(1) */
	uint8_t vfgrp; /* VF group (0 - 8) */
	uint8_t node; /* Operating node: Bits (46:44) in BAR0 address */
	uint8_t  priority; /* VF priority ring: 1-High proirity round
			    * robin ring;0-Low priority round robin ring;
			    */
	uint8_t  reqmode; /* Request processing mode POLL/ASYNC */
	struct pci_dev *pdev; /* pci device handle */
	void *sysdev; /* sysfs device */
	void *proc; /* proc dir */
	void __iomem *reg_base; /* Register start address */
	void *wqe_info;	/* BH worker threads */
	void *context;	/* Context Specific Information*/
	void *nqueue_info; /* Queue Specific Information*/
	/* MSI-X */
	bool msix_enabled;
	uint8_t	num_vec;
	struct msix_entry msix_entries[CPT_VF_MSIX_VECTORS];
	bool irq_allocated[CPT_VF_MSIX_VECTORS];
	cpumask_var_t affinity_mask[CPT_VF_MSIX_VECTORS];
	uint64_t intcnt;
	/* Command and Pending queues */
	uint32_t qlen;
	uint32_t qsize; /* Calculated queue size */
	uint32_t nr_queues;
	uint32_t max_queues;
	struct command_qinfo cqinfo; /* Command queue information */
	struct pending_qinfo pqinfo; /* Pending queue information */
	/* VF-PF mailbox communication */
	bool pf_acked;
	bool pf_nacked;
} ____cacheline_aligned_in_smp;

#define CPT_NODE_ID_SHIFT (44u)
#define CPT_NODE_ID_MASK (3u)

#define MAX_CPT_AE_CORES 6
#define MAX_CPT_SE_CORES 10

enum req_mode {
	BLOCKING,
	NON_BLOCKING,
	SPEED,
	KERN_POLL,
};

enum dma_mode {
	DMA_DIRECT_DIRECT, /* Input DIRECT, Output DIRECT */
	DMA_GATHER_SCATTER
};

enum inputype {
	FROM_CTX = 0,
	FROM_DPTR = 1
};

enum CspErrorCodes {
	/*Microcode errors*/
	NO_ERR = 0x00,
	ERR_OPCODE_UNSUPPORTED = 0x01,

	/*SCATTER GATHER*/
	ERR_SCATTER_GATHER_WRITE_LENGTH = 0x02,
	ERR_SCATTER_GATHER_LIST = 0x03,
	ERR_SCATTER_GATHER_NOT_SUPPORTED = 0x04,

	/*AE*/
	ERR_LENGTH_INVALID = 0x05,
	ERR_MOD_LEN_INVALID = 0x06,
	ERR_EXP_LEN_INVALID = 0x07,
	ERR_DATA_LEN_INVALID = 0x08,
	ERR_MOD_LEN_ODD = 0x09,
	ERR_PKCS_DECRYPT_INCORRECT = 0x0a,
	ERR_ECC_PAI = 0xb,
	ERR_ECC_CURVE_UNSUPPORTED = 0xc,
	ERR_ECC_SIGN_R_INVALID = 0xd,
	ERR_ECC_SIGN_S_INVALID = 0xe,
	ERR_ECC_SIGNATURE_MISMATCH = 0xf,

	/*SE GC*/
	ERR_GC_LENGTH_INVALID = 0x41,
	ERR_GC_RANDOM_LEN_INVALID = 0x42,
	ERR_GC_DATA_LEN_INVALID = 0x43,
	ERR_GC_DRBG_TYPE_INVALID = 0x44,
	ERR_GC_CTX_LEN_INVALID = 0x45,
	ERR_GC_CIPHER_UNSUPPORTED = 0x46,
	ERR_GC_AUTH_UNSUPPORTED = 0x47,
	ERR_GC_OFFSET_INVALID = 0x48,
	ERR_GC_HASH_MODE_UNSUPPORTED = 0x49,
	ERR_GC_DRBG_ENTROPY_LEN_INVALID = 0x4a,
	ERR_GC_DRBG_ADDNL_LEN_INVALID = 0x4b,
	ERR_GC_ICV_MISCOMPARE = 0x4c,
	ERR_GC_DATA_UNALIGNED = 0x4d,

	/*SE IPSEC*/
	ERR_IPSEC_AUTH_UNSUPPORTED = 0xB0,
	ERR_IPSEC_ENCRYPT_UNSUPPORTED = 0xB1,
	ERR_IPSEC_IP_VERSION = 0xB2,
	ERR_IPSEC_PROTOCOL = 0xB3,
	ERR_IPSEC_CONTEXT_INVALID = 0xB4,
	ERR_IPSEC_CONTEXT_DIRECTION_MISMATCH = 0xB5,
	ERR_IPSEC_IP_PAYLOAD_TYPE = 0xB6,
	ERR_IPSEC_CONTEXT_FLAG_MISMATCH = 0xB7,
	ERR_IPSEC_GRE_HEADER_MISMATCH = 0xB8,
	ERR_IPSEC_GRE_PROTOCOL = 0xB9,
	ERR_IPSEC_CUSTOM_HDR_LEN = 0xBA,
	ERR_IPSEC_ESP_NEXT_HEADER = 0xBB,
	ERR_IPSEC_IPCOMP_CONFIGURATION = 0xBC,
	ERR_IPSEC_FRAG_SIZE_CONFIGURATION = 0xBD,
	ERR_IPSEC_SPI_MISMATCH = 0xBE,
	ERR_IPSEC_CHECKSUM = 0xBF,
	ERR_IPSEC_IPCOMP_PACKET_DETECTED = 0xC0,
	ERR_IPSEC_TFC_PADDING_WITH_PREFRAG = 0xC1,
	ERR_IPSEC_DSIV_INCORRECT_PARAM = 0xC2,
	ERR_IPSEC_AUTHENTICATION_MISMATCH = 0xC3,
	ERR_IPSEC_PADDING = 0xC4,
	ERR_IPSEC_DUMMY_PAYLOAD = 0xC5,
	ERR_IPSEC_IPV6_EXTENSION_HEADERS_TOO_BIG = 0xC6,
	ERR_IPSEC_IPV6_HOP_BY_HOP = 0xC7,
	ERR_IPSEC_IPV6_RH_LENGTH = 0xC8,
	ERR_IPSEC_IPV6_OUTBOUND_RH_COPY_ADDR = 0xC9,
	ERR_IPSEC_IPV6_DECRYPT_RH_SEGS_LEFT = 0xCA,
	ERR_IPSEC_IPV6_HEADER_INVALID = 0xCB,
	ERR_IPSEC_SELECTOR_MATCH = 0xCC,

	/*SE SSL*/
	ERR_SSL_POM_LEN_INVALID = 0x81,
	ERR_SSL_RECORD_LEN_INVALID = 0x82,
	ERR_SSL_CTX_LEN_INVALID = 0x83,
	ERR_SSL_CIPHER_UNSUPPORTED = 0x84,
	ERR_SSL_MAC_UNSUPPORTED = 0x85,
	ERR_SSL_VERSION_UNSUPPORTED = 0x86,
	ERR_SSL_VERIFY_AUTH_UNSUPPORTED = 0x87,
	ERR_SSL_MS_LEN_INVALID = 0x88,
	ERR_SSL_MAC_MISMATCH = 0x89,

	/* API Layer */
	ERR_REQ_TIMEOUT      = (0x40000000 | 0x103),    /* 0x40000103 */
	ERR_REQ_PENDING      = (0x40000000 | 0x110),    /* 0x40000110 */
	ERR_BAD_INPUT_LENGTH = (0x40000000 | 384),    /* 0x40000180 */
	ERR_BAD_KEY_LENGTH,
	ERR_BAD_KEY_HANDLE,
	ERR_BAD_CONTEXT_HANDLE,
	ERR_BAD_SCALAR_LENGTH,
	ERR_BAD_DIGEST_LENGTH,
	ERR_BAD_INPUT_ARG,
	ERR_BAD_SSL_MSG_TYPE,
	ERR_BAD_RECORD_PADDING,
	ERR_NB_REQUEST_PENDING,
};

int cptvf_send_vf_up(struct cpt_vf *cptvf);
int cptvf_send_vf_down(struct cpt_vf *cptvf);
int cptvf_send_vf_to_grp_msg(struct cpt_vf *cptvf);
int cptvf_send_vf_priority_msg(struct cpt_vf *cptvf);
int cptvf_send_vq_size_msg(struct cpt_vf *cptvf);
int cptvf_check_pf_ready(struct cpt_vf *cptvf);
void cptvf_handle_mbox_intr(struct cpt_vf *cptvf);
void cvm_crypto_exit(void);
int cvm_crypto_init(struct cpt_vf *cptvf);
void vq_post_process(struct cpt_vf *cptvf, uint32_t qno);
void cptvf_write_vq_doorbell(struct cpt_vf *cptvf, uint32_t val);
#endif /* __CPTVF_H */
