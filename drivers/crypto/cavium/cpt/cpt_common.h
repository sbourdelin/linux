/*
 * Copyright (C) 2016 Cavium, Inc.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of version 2 of the GNU General Public License
 * as published by the Free Software Foundation.
 */

#ifndef __CPT_COMMON_H
#define __CPT_COMMON_H

#include <asm/byteorder.h>
#include <linux/uaccess.h>
#include <linux/types.h>
#include <linux/spinlock.h>
#include <linux/pci.h>
#include <linux/cpumask.h>
#include <linux/string.h>
#include <linux/pci_regs.h>
#include <linux/delay.h>
#include <linux/printk.h>
#include <linux/sched.h>
#include <linux/completion.h>
#include <asm/arch_timer.h>
#include <linux/types.h>

#include "cpt_hw_types.h"

/* configuration space offsets */
#ifndef PCI_VENDOR_ID
#define PCI_VENDOR_ID 0x00 /* 16 bits */
#endif
#ifndef PCI_DEVICE_ID
#define PCI_DEVICE_ID 0x02 /* 16 bits */
#endif
#ifndef PCI_REVISION_ID
#define PCI_REVISION_ID 0x08 /* Revision ID */
#endif
#ifndef PCI_CAPABILITY_LIST
#define PCI_CAPABILITY_LIST 0x34 /* first capability list entry */
#endif

/* Device ID */
#define PCI_VENDOR_ID_CAVIUM 0x177d
#define CPT_81XX_PCI_PF_DEVICE_ID 0xa040
#define CPT_81XX_PCI_VF_DEVICE_ID 0xa041

#define PASS_1_0 0x0

/* CPT Models ((Device ID<<16)|Revision ID) */
/* CPT models */
#define CPT_81XX_PASS1_0 ((CPT_81XX_PCI_PF_DEVICE_ID << 8) | PASS_1_0)
#define CPTVF_81XX_PASS1_0 ((CPT_81XX_PCI_VF_DEVICE_ID << 8) | PASS_1_0)

#define PF 0
#define VF 1

#define DEFAULT_DEVICE_QUEUES CPT_NUM_QS_PER_VF

#define SUCCESS	(0)
#define FAIL	(1)

#ifndef ROUNDUP4
#define ROUNDUP4(val) (((val) + 3) & 0xfffffffc)
#endif

#ifndef ROUNDUP8
#define ROUNDUP8(val) (((val) + 7) & 0xfffffff8)
#endif

#ifndef ROUNDUP16
#define ROUNDUP16(val) (((val) + 15) & 0xfffffff0)
#endif

#define ERR_ADDR_LEN 8

#define CPT_MBOX_MSG_TIMEOUT 2000
#define VF_STATE_DOWN (0)
#define VF_STATE_UP (1)

/**< flags to indicate the features supported */
#define CPT_FLAG_DMA_64BIT (uint16_t)(1 << 0)
#define CPT_FLAG_MSIX_ENABLED (uint16_t)(1 << 1)
#define CPT_FLAG_SRIOV_ENABLED (uint16_t)(1 << 2)
#define CPT_FLAG_VF_DRIVER (uint16_t)(1 << 3)
#define CPT_FLAG_DEVICE_READY (uint16_t)(1 << 4)

#define cpt_msix_enabled(cpt) ((cpt)->flags & CPT_FLAG_MSIX_ENABLED)
#define cpt_sriov_enabled(cpt) ((cpt)->flags & CPT_FLAG_SRIOV_ENABLED)
#define cpt_vf_driver(cpt) ((cpt)->flags & CPT_FLAG_VF_DRIVER)
#define cpt_pf_driver(cpt) (!((cpt)->flags & CPT_FLAG_VF_DRIVER))
#define cpt_device_ready(cpt) ((cpt)->flags & CPT_FLAG_DEVICE_READY)

#define MAX_CPT_DEVICES	2

/* Default command queue length */
#define DEFAULT_CMD_QLEN 2046
#define DEFAULT_CMD_QCHUNK_SIZE 1023

/* Max command queue length allowed. This is to restrict host memory usage */
#define MAX_CMD_QLEN 16000

/* Completion Interrupt threshold */
#define COMPLETION_INTR_THOLD 1

/* Default command timeout in seconds */
#define DEFAULT_COMMAND_TIMEOUT 4

/* Default Mailbox ACK timeout */
#define DEFAULT_MBOX_ACK_TIMEOUT 4

#define CPT_MBOX_MSG_TYPE_REQ 0
#define CPT_MBOX_MSG_TYPE_ACK 1
#define CPT_MBOX_MSG_TYPE_NACK 2
#define CPT_MBOX_MSG_TYPE_NOP 3

#define CPT_COUNT_THOLD 1
#define CPT_TIMER_THOLD	0xFFFF
#define CPT_DBELL_THOLD	1

/*
 * CPT Registers map for 81xx
 */

/* PF registers */
#define CPTX_PF_CONSTANTS(a) (0x0ll + 0x1000000000ll * ((a) & 0x1))
#define CPTX_PF_RESET(a) (0x100ll + 0x1000000000ll * ((a) & 0x1))
#define CPTX_PF_DIAG(a) (0x120ll + 0x1000000000ll * ((a) & 0x1))
#define CPTX_PF_BIST_STATUS(a) (0x160ll + 0x1000000000ll * ((a) & 0x1))
#define CPTX_PF_ECC0_CTL(a) (0x200ll + 0x1000000000ll * ((a) & 0x1))
#define CPTX_PF_ECC0_FLIP(a) (0x210ll + 0x1000000000ll * ((a) & 0x1))
#define CPTX_PF_ECC0_INT(a) (0x220ll + 0x1000000000ll * ((a) & 0x1))
#define CPTX_PF_ECC0_INT_W1S(a) (0x230ll + 0x1000000000ll * ((a) & 0x1))
#define CPTX_PF_ECC0_ENA_W1S(a)	(0x240ll + 0x1000000000ll * ((a) & 0x1))
#define CPTX_PF_ECC0_ENA_W1C(a)	(0x250ll + 0x1000000000ll * ((a) & 0x1))
#define CPTX_PF_MBOX_INTX(a, b)	\
	(0x400ll + 0x1000000000ll * ((a) & 0x1) + 8ll * ((b) & 0x0))
#define CPTX_PF_MBOX_INT_W1SX(a, b) \
	(0x420ll + 0x1000000000ll * ((a) & 0x1) + 8ll * ((b) & 0x0))
#define CPTX_PF_MBOX_ENA_W1CX(a, b) \
	(0x440ll + 0x1000000000ll * ((a) & 0x1) + 8ll * ((b) & 0x0))
#define CPTX_PF_MBOX_ENA_W1SX(a, b) \
	(0x460ll + 0x1000000000ll * ((a) & 0x1) + 8ll * ((b) & 0x0))
#define CPTX_PF_EXEC_INT(a) (0x500ll + 0x1000000000ll * ((a) & 0x1))
#define CPTX_PF_EXEC_INT_W1S(a)	(0x520ll + 0x1000000000ll * ((a) & 0x1))
#define CPTX_PF_EXEC_ENA_W1C(a)	(0x540ll + 0x1000000000ll * ((a) & 0x1))
#define CPTX_PF_EXEC_ENA_W1S(a)	(0x560ll + 0x1000000000ll * ((a) & 0x1))
#define CPTX_PF_GX_EN(a, b) \
	(0x600ll + 0x1000000000ll * ((a) & 0x1) + 8ll * ((b) & 0x7))
#define CPTX_PF_EXEC_INFO(a) (0x700ll + 0x1000000000ll * ((a) & 0x1))
#define CPTX_PF_EXEC_BUSY(a) (0x800ll + 0x1000000000ll * ((a) & 0x1))
#define CPTX_PF_EXEC_INFO0(a) (0x900ll + 0x1000000000ll * ((a) & 0x1))
#define CPTX_PF_EXEC_INFO1(a) (0x910ll + 0x1000000000ll * ((a) & 0x1))
#define CPTX_PF_INST_REQ_PC(a) (0x10000ll + 0x1000000000ll * ((a) & 0x1))
#define CPTX_PF_INST_LATENCY_PC(a) \
	(0x10020ll + 0x1000000000ll * ((a) & 0x1))
#define CPTX_PF_RD_REQ_PC(a) (0x10040ll + 0x1000000000ll * ((a) & 0x1))
#define CPTX_PF_RD_LATENCY_PC(a) (0x10060ll + 0x1000000000ll * ((a) & 0x1))
#define CPTX_PF_RD_UC_PC(a) (0x10080ll + 0x1000000000ll * ((a) & 0x1))
#define CPTX_PF_ACTIVE_CYCLES_PC(a) \
	(0x10100ll + 0x1000000000ll * ((a) & 0x1))
#define CPTX_PF_EXE_CTL(a) (0x4000000ll + 0x1000000000ll * ((a) & 0x1))
#define CPTX_PF_EXE_STATUS(a) (0x4000008ll + 0x1000000000ll * ((a) & 0x1))
#define CPTX_PF_EXE_CLK(a) (0x4000010ll + 0x1000000000ll * ((a) & 0x1))
#define CPTX_PF_EXE_DBG_CTL(a) (0x4000018ll + 0x1000000000ll * ((a) & 0x1))
#define CPTX_PF_EXE_DBG_DATA(a)	(0x4000020ll + 0x1000000000ll * ((a) & 0x1))
#define CPTX_PF_EXE_BIST_STATUS(a) \
	(0x4000028ll + 0x1000000000ll * ((a) & 0x1))
#define CPTX_PF_EXE_REQ_TIMER(a) (0x4000030ll + 0x1000000000ll * ((a) & 0x1))
#define CPTX_PF_EXE_MEM_CTL(a) (0x4000038ll + 0x1000000000ll * ((a) & 0x1))
#define CPTX_PF_EXE_PERF_CTL(a)	(0x4001000ll + 0x1000000000ll * ((a) & 0x1))
#define CPTX_PF_EXE_DBG_CNTX(a, b) \
	(0x4001100ll + 0x1000000000ll * ((a) & 0x1) + 8ll * ((b) & 0xf))
#define CPTX_PF_EXE_PERF_EVENT_CNT(a) \
	(0x4001180ll + 0x1000000000ll * ((a) & 0x1))
#define CPTX_PF_EXE_EPCI_INBX_CNT(a, b) \
	(0x4001200ll + 0x1000000000ll * ((a) & 0x1) + 8ll * ((b) & 0x0))
#define CPTX_PF_EXE_EPCI_OUTBX_CNT(a, b) \
	(0x4001240ll + 0x1000000000ll * ((a) & 0x1) + 8ll * ((b) & 0x0))
#define CPTX_PF_ENGX_UCODE_BASE(a, b) \
	(0x4002000ll + 0x1000000000ll * ((a) & 0x1) + 8ll * ((b) & 0x3f))
#define CPTX_PF_QX_CTL(a, b) \
	(0x8000000ll + 0x1000000000ll * ((a) & 0x1) + 0x100000ll * ((b) & 0xf))
#define CPTX_PF_QX_GMCTL(a, b) \
	(0x8000020ll + 0x1000000000ll * ((a) & 0x1) + 0x100000ll * ((b) & 0xf))
#define CPTX_PF_QX_CTL2(a, b) \
	(0x8000100ll + 0x1000000000ll * ((a) & 0x1) + 0x100000ll * ((b) & 0xf))
#define CPTX_PF_VFX_MBOXX(a, b, c) \
	(0x8001000ll + 0x1000000000ll * ((a) & 0x1) + 0x100000ll * ((b) & 0xf) + 0x100ll * ((c) & 0x1))
#define CPTX_PF_MSIX_VECX_ADDR(a, b) \
	(0x0ll + 0x1000000000ll * ((a) & 0x1) + 0x10ll * ((b) & 0x3))
#define CPTX_PF_MSIX_VECX_CTL(a, b) \
	(0x8ll + 0x1000000000ll * ((a) & 0x1) + 0x10ll * ((b) & 0x3))
#define CPTX_PF_MSIX_PBAX(a, b)	\
	(0xf0000ll + 0x1000000000ll * ((a) & 0x1) + 8ll * ((b) & 0x0))

/* VF registers */
#define CPTX_VQX_CTL(a, b) \
	(0x100ll + 0x1000000000ll * ((a) & 0x0) + 0x100000ll * ((b) & 0xf))
#define CPTX_VQX_SADDR(a, b) \
	(0x200ll + 0x1000000000ll * ((a) & 0x0) + 0x100000ll * ((b) & 0xf))
#define CPTX_VQX_DONE_WAIT(a, b) \
	(0x400ll + 0x1000000000ll * ((a) & 0x0) + 0x100000ll * ((b) & 0xf))
#define CPTX_VQX_INPROG(a, b) \
	(0x410ll + 0x1000000000ll * ((a) & 0x0) + 0x100000ll * ((b) & 0xf))
#define CPTX_VQX_DONE(a, b) \
	(0x420ll + 0x1000000000ll * ((a) & 0x1) + 0x100000ll * ((b) & 0xf))
#define CPTX_VQX_DONE_ACK(a, b) \
	(0x440ll + 0x1000000000ll * ((a) & 0x1) + 0x100000ll * ((b) & 0xf))
#define CPTX_VQX_DONE_INT_W1S(a, b) \
	(0x460ll + 0x1000000000ll * ((a) & 0x1) + 0x100000ll * ((b) & 0xf))
#define CPTX_VQX_DONE_INT_W1C(a, b) \
	(0x468ll + 0x1000000000ll * ((a) & 0x1) + 0x100000ll * ((b) & 0xf))
#define CPTX_VQX_DONE_ENA_W1S(a, b) \
	(0x470ll + 0x1000000000ll * ((a) & 0x1) + 0x100000ll * ((b) & 0xf))
#define CPTX_VQX_DONE_ENA_W1C(a, b) \
	(0x478ll + 0x1000000000ll * ((a) & 0x1) + 0x100000ll * ((b) & 0xf))
#define CPTX_VQX_MISC_INT(a, b)	\
	(0x500ll + 0x1000000000ll * ((a) & 0x1) + 0x100000ll * ((b) & 0xf))
#define CPTX_VQX_MISC_INT_W1S(a, b) \
	(0x508ll + 0x1000000000ll * ((a) & 0x1) + 0x100000ll * ((b) & 0xf))
#define CPTX_VQX_MISC_ENA_W1S(a, b) \
	(0x510ll + 0x1000000000ll * ((a) & 0x1) + 0x100000ll * ((b) & 0xf))
#define CPTX_VQX_MISC_ENA_W1C(a, b) \
	(0x518ll + 0x1000000000ll * ((a) & 0x1) + 0x100000ll * ((b) & 0xf))
#define CPTX_VQX_DOORBELL(a, b)	\
	(0x600ll + 0x1000000000ll * ((a) & 0x1) + 0x100000ll * ((b) & 0xf))
#define CPTX_VFX_PF_MBOXX(a, b, c) \
	(0x1000ll + 0x1000000000ll * ((a) & 0x1) + 0x100000ll * ((b) & 0xf) + 8ll * ((c) & 0x1))
#define CPTX_VFX_MSIX_VECX_ADDR(a, b, c) \
	(0x0ll + 0x1000000000ll * ((a) & 0x1) + 0x100000ll * ((b) & 0xf) + 0x10ll * ((c) & 0x1))
#define CPTX_VFX_MSIX_VECX_CTL(a, b, c) \
	(0x8ll + 0x1000000000ll * ((a) & 0x1) + 0x100000ll * ((b) & 0xf) + 0x10ll * ((c) & 0x1))
#define CPTX_VFX_MSIX_PBAX(a, b, c) \
	(0xf0000ll + 0x1000000000ll * ((a) & 0x1) + 0x100000ll * ((b) & 0xf) + 8ll * ((c) & 0x0))

/* Future extensions */
#define CPTX_BRIDGE_BP_TEST(a) (0x1c0ll + 0x1000000000ll * ((a) & 0x1))
#define CPTX_CQM_CORE_OBS0(a) (0x1a0ll + 0x1000000000ll * ((a) & 0x1))
#define CPTX_CQM_CORE_OBS1(a) (0x1a8ll + 0x1000000000ll * ((a) & 0x1))
#define CPTX_NCBI_OBS(a) (0x190ll + 0x1000000000ll * ((a) & 0x1))
#define CPTX_PF_BP_TEST(a) (0x180ll + 0x1000000000ll * ((a) & 0x1))
#define CPTX_PF_ECO(a) (0x140ll + 0x1000000000ll * ((a) & 0x1))

/*###### PCIE EP-Mode Configuration Registers #########*/
#define PCIEEP0_CFG000 (0x0)
#define PCIEEP0_CFG002 (0x8)
#define PCIEEP0_CFG011 (0x2C)
#define PCIEEP0_CFG020 (0x50)
#define PCIEEP0_CFG025 (0x64)
#define PCIEEP0_CFG030 (0x78)
#define PCIEEP0_CFG044 (0xB0)
#define PCIEEP0_CFG045 (0xB4)
#define PCIEEP0_CFG082 (0x148)
#define PCIEEP0_CFG095 (0x17C)
#define PCIEEP0_CFG096 (0x180)
#define PCIEEP0_CFG097 (0x184)
#define PCIEEP0_CFG103 (0x19C)
#define PCIEEP0_CFG460 (0x730)
#define PCIEEP0_CFG461 (0x734)
#define PCIEEP0_CFG462 (0x738)

/*#######  PCIe EP-Mode SR-IOV Configuration Registers  #####*/
#define PCIEEPVF0_CFG000 (0x0)
#define PCIEEPVF0_CFG002 (0x8)
#define PCIEEPVF0_CFG011 (0x2C)
#define PCIEEPVF0_CFG030 (0x78)
#define PCIEEPVF0_CFG044 (0xB0)

enum vftype {
	AE_TYPES = 1,
	SE_TYPES = 2,
	BAD_CPT_TYPES,
};

static inline int32_t count_set_bits(uint64_t mask)
{
	int32_t count = 0;

	while (mask) {
		if (mask & 1ULL)
			count++;
		mask = mask >> 1;
	}

	return count;
}

static const uint8_t cpt_device_name[] = "CPT81XX";
static const uint8_t cptvf_device_name[] = "CPT81XX-VF";
static const uint8_t cpt_device_file[] = "cpt";
static const uint8_t cptvf_device_file[] = "cptvf";

static const uint8_t cpt_driver_name[] = "CPT Driver";
static const uint8_t cpt_driver_class[] = "crypto";
static const uint8_t cptvf_driver_class[] = "cryptovf";

/* Max CPT devices supported */
enum cpt_mbox_opcode {
	CPT_MSG_VF_CFG = 1,
	CPT_MSG_VF_UP,
	CPT_MSG_VF_DOWN,
	CPT_MSG_CHIPID_VFID,
	CPT_MSG_READY,
	CPT_MSG_QLEN,
	CPT_MSG_QBIND_GRP,
	CPT_MSG_VQ_PRIORITY,
	CPT_MSG_VF_QUERY_HEALTH,
};

union cpt_chipid_vfid {
	uint16_t u16;
	struct {
#if defined(__BIG_ENDIAN_BITFIELD)
		uint16_t chip_id:8;
		uint16_t vfid:8;
#else
		uint16_t vfid:8;
		uint16_t chip_id:8;
#endif
	} s;
};

/* CPT mailbox structure */
struct cpt_mbox {
	uint64_t msg; /* Message type MBOX[0] */
	uint64_t data;/* Data         MBOX[1] */
};

/* The Cryptographic Acceleration Unit can *only* be found in SoCs
 * containing the ThunderX ARM64 CPU implementation.  All accesses to the device
 * registers on this platform are implicitly strongly ordered with respect
 * to memory accesses. So writeq_relaxed() and readq_relaxed() are safe to use
 * with no memory barriers in this driver.  The readq()/writeq() functions add
 * explicit ordering operation which in this case are redundant, and only
 * add overhead.
 */
/* Register read/write APIs */
static inline void cpt_write_csr64(uint8_t __iomem *hw_addr, uint64_t offset,
				   uint64_t val)
{
	uint8_t __iomem *base = ACCESS_ONCE(hw_addr);

	writeq_relaxed(val, base + offset);
}

static inline uint64_t cpt_read_csr64(uint8_t __iomem *hw_addr, uint64_t offset)
{
	uint8_t __iomem *base = ACCESS_ONCE(hw_addr);

	return readq_relaxed(base + offset);
}

static inline void byte_swap_64(uint64_t *data)
{
	uint64_t val = 0ULL;
	uint8_t *a, *b;

	a = (uint8_t *)data;
	b = (uint8_t *)&val;
	b[0] = a[7];
	b[1] = a[6];
	b[2] = a[5];
	b[3] = a[4];
	b[4] = a[3];
	b[5] = a[2];
	b[6] = a[1];
	b[7] = a[0];
	*data = val;
}

static inline void byte_swap_16(uint16_t *data)
{
	uint16_t val = *data;
	*data = (val >> 8) | (val << 8);
}
#endif /* __CPT_COMMON_H */
