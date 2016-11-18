/*
 * Copyright (C) 2016 Cavium, Inc.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of version 2 of the GNU General Public License
 * as published by the Free Software Foundation.
 */

#ifndef __CPT_HW_TYPES_H
#define __CPT_HW_TYPES_H

#include "cpt_common.h"

#define NR_CLUSTER (4)
#define CSR_DELAY (30)

#define CPT_NUM_QS_PER_VF (1)
#define CPT_INST_SIZE (64)
#define CPT_VQ_CHUNK_ALIGN (128) /**< 128 byte align */
#define CPT_NEXT_CHUNK_PTR_SIZE (8)
#define CPT_INST_CHUNK_MAX_SIZE (1023)

#define CPT_MAX_CORE_GROUPS (8)
#define CPT_MAX_SE_CORES (10)
#define CPT_MAX_AE_CORES (6)
#define CPT_MAX_TOTAL_CORES (CPT_MAX_SE_CORES + CPT_MAX_AE_CORES)
#define CPT_MAX_VF_NUM (16)
#define CPT_MAX_VQ_NUM (16)
#define CPT_PF_VF_MAILBOX_SIZE (2)

/* MSI-X interrupts */
#define	CPT_PF_MSIX_VECTORS (3)
#define	CPT_VF_MSIX_VECTORS (2)

/* Configuration and Status registers are in BAR 0 */
#define CPT_CSR_BAR 0
#define CPT_MSIX_BAR 4

/**
 * Enumeration cpt_bar_e
 *
 * CPT Base Address Register Enumeration
 * Enumerates the base address registers.
 */
#define CPT_BAR_E_CPTX_PF_BAR0(a) (0x872000000000ll + 0x1000000000ll * (a))
#define CPT_BAR_E_CPTX_PF_BAR4(a) (0x872010000000ll + 0x1000000000ll * (a))
#define CPT_BAR_E_CPTX_VFX_BAR0(a, b) \
	(0x872020000000ll + 0x1000000000ll * (a) + 0x100000ll * (b))
#define CPT_BAR_E_CPTX_VFX_BAR4(a, b) \
	(0x872030000000ll + 0x1000000000ll * (a) + 0x100000ll * (b))

/**
 * Enumeration cpt_comp_e
 *
 * CPT Completion Enumeration
 * Enumerates the values of CPT_RES_S[COMPCODE].
 */
enum cpt_comp_e {
	CPT_COMP_E_NOTDONE = 0x00,
	CPT_COMP_E_GOOD = 0x01,
	CPT_COMP_E_FAULT = 0x02,
	CPT_COMP_E_SWERR = 0x03,
	CPT_COMP_E_LAST_ENTRY = 0xFF
};

/**
 * Enumeration cpt_engine_err_type_e
 *
 * CPT Engine Error Code Enumeration
 * Enumerates the values of CPT_RES_S[COMPCODE].
 */
enum cpt_engine_err_type_e {
	CPT_ENGINE_ERR_TYPE_E_NOERR = 0x00,
	CPT_ENGINE_ERR_TYPE_E_RF = 0x01,
	CPT_ENGINE_ERR_TYPE_E_UC = 0x02,
	CPT_ENGINE_ERR_TYPE_E_WD = 0x04,
	CPT_ENGINE_ERR_TYPE_E_GE = 0x08,
	CPT_ENGINE_ERR_TYPE_E_BUS = 0x20,
	CPT_ENGINE_ERR_TYPE_E_LAST = 0xFF
};

/**
 * Enumeration cpt_eop_e
 *
 * CPT EOP (EPCI Opcodes) Enumeration
 * Opcodes on the epci bus.
 */
enum cpt_eop_e {
	CPT_EOP_E_DMA_RD_LDT = 0x01,
	CPT_EOP_E_DMA_RD_LDI = 0x02,
	CPT_EOP_E_DMA_RD_LDY = 0x06,
	CPT_EOP_E_DMA_RD_LDD = 0x08,
	CPT_EOP_E_DMA_RD_LDE = 0x0b,
	CPT_EOP_E_DMA_RD_LDWB = 0x0d,
	CPT_EOP_E_DMA_WR_STY = 0x0e,
	CPT_EOP_E_DMA_WR_STT = 0x11,
	CPT_EOP_E_DMA_WR_STP = 0x12,
	CPT_EOP_E_ATM_FAA64 = 0x3b,
	CPT_EOP_E_RANDOM1_REQ = 0x61,
	CPT_EOP_E_RANDOM_REQ = 0x60,
	CPT_EOP_E_ERR_REQUEST = 0xfb,
	CPT_EOP_E_UCODE_REQ = 0xfc,
	CPT_EOP_E_MEMB = 0xfd,
	CPT_EOP_E_NEW_WORK_REQ = 0xff,
};

/**
 * Enumeration cpt_pf_int_vec_e
 *
 * CPT PF MSI-X Vector Enumeration
 * Enumerates the MSI-X interrupt vectors.
 */
enum cpt_pf_int_vec_e {
	CPT_PF_INT_VEC_E_ECC0 = 0x00,
	CPT_PF_INT_VEC_E_EXEC = 0x01
};

#define CPT_PF_INT_VEC_E_MBOXX(a) (0x02 + (a))

/**
 * Enumeration cpt_rams_e
 *
 * CPT RAM Field Enumeration
 * Enumerates the relative bit positions within CPT()_PF_ECC0_CTL[CDIS].
 */
enum cpt_rams_e {
	CPT_RAMS_E_NCBI_DATFIF = 0x00,
	CPT_RAMS_E_NCBO_MEM0 = 0x01,
	CPT_RAMS_E_CQM_CTLMEM = 0x02,
	CPT_RAMS_E_CQM_BPTR = 0x03,
	CPT_RAMS_E_CQM_GMID = 0x04,
	CPT_RAMS_E_CQM_INSTFIF0 = 0x05,
	CPT_RAMS_E_CQM_INSTFIF1 = 0x06,
	CPT_RAMS_E_CQM_INSTFIF2 = 0x07,
	CPT_RAMS_E_CQM_INSTFIF3 = 0x08,
	CPT_RAMS_E_CQM_INSTFIF4 = 0x09,
	CPT_RAMS_E_CQM_INSTFIF5 = 0x0a,
	CPT_RAMS_E_CQM_INSTFIF6 = 0x0b,
	CPT_RAMS_E_CQM_INSTFIF7 = 0x0c,
	CPT_RAMS_E_CQM_DONE_CNT = 0x0d,
	CPT_RAMS_E_CQM_DONE_TIMER = 0x0e,
	CPT_RAMS_E_COMP_FIFO = 0x0f,
	CPT_RAMS_E_MBOX_MEM = 0x10,
	CPT_RAMS_E_FPA_MEM = 0x11,
	CPT_RAMS_E_CDEI_UCODE = 0x12,
	CPT_RAMS_E_COMP_ARRAY0 = 0x13,
	CPT_RAMS_E_COMP_ARRAY1 = 0x14,
	CPT_RAMS_E_CSR_VMEM = 0x15,
	CPT_RAMS_E_RSP_MAP = 0x16,
	CPT_RAMS_E_RSP_INST = 0x17,
	CPT_RAMS_E_RSP_NCBO = 0x18,
	CPT_RAMS_E_RSP_RNM = 0x19,
	CPT_RAMS_E_CDEI_FIFO0 = 0x1a,
	CPT_RAMS_E_CDEI_FIFO1 = 0x1b,
	CPT_RAMS_E_EPCO_FIFO0 = 0x1c,
	CPT_RAMS_E_EPCO_FIFO1 = 0x1d,
	CPT_RAMS_E_LAST_ENTRY = 0xff
};

/**
 * Enumeration cpt_vf_int_vec_e
 *
 * CPT VF MSI-X Vector Enumeration
 * Enumerates the MSI-X interrupt vectors.
 */
enum cpt_vf_int_vec_e {
	CPT_VF_INT_VEC_E_MISC = 0x00,
	CPT_VF_INT_VEC_E_DONE = 0x01
};

#define CPT_VF_INTR_MBOX_MASK BIT(0)
#define CPT_VF_INTR_DOVF_MASK BIT(1)
#define CPT_VF_INTR_IRDE_MASK BIT(2)
#define CPT_VF_INTR_NWRP_MASK BIT(3)
#define CPT_VF_INTR_SERR_MASK BIT(4)

/**
 * Structure cpt_inst_s
 *
 * CPT Instruction Structure
 * This structure specifies the instruction layout. Instructions are
 * stored in memory as little-endian unless CPT()_PF_Q()_CTL[INST_BE] is set.
 * cpt_inst_s_s
 * Word 0
 * doneint:1 Done interrupt.
 *	0 = No interrupts related to this instruction.
 *	1 = When the instruction completes, CPT()_VQ()_DONE[DONE] will be
 *	incremented,and based on the rules described there an interrupt may
 *	occur.
 * Word 1
 * res_addr:64 [127: 64] Result IOVA.
 *	If nonzero, specifies where to write CPT_RES_S.
 *	If zero, no result structure will be written.
 *	Address must be 16-byte aligned.
 *	Bits <63:49> are ignored by hardware; software should use a
 *	sign-extended bit <48> for forward compatibility.
 * Word 2
 *  grp:10 [171:162] If [WQ_PTR] is nonzero, the SSO guest-group to use when
 *	CPT submits work SSO.
 *	For the SSO to not discard the add-work request, FPA_PF_MAP() must map
 *	[GRP] and CPT()_PF_Q()_GMCTL[GMID] as valid.
 *  tt:2 [161:160] If [WQ_PTR] is nonzero, the SSO tag type to use when CPT
 *	submits work to SSO
 *  tag:32 [159:128] If [WQ_PTR] is nonzero, the SSO tag to use when CPT
 *	submits work to SSO.
 * Word 3
 *  wq_ptr:64 [255:192] If [WQ_PTR] is nonzero, it is a pointer to a
 *	work-queue entry that CPT submits work to SSO after all context,
 *	output data, and result write operations are visible to other
 *	CNXXXX units and the cores. Bits <2:0> must be zero.
 *	Bits <63:49> are ignored by hardware; software should
 *	use a sign-extended bit <48> for forward compatibility.
 *	Internal:
 *	Bits <63:49>, <2:0> are ignored by hardware, treated as always 0x0.
 * Word 4
 *  ei0:64; [319:256] Engine instruction word 0. Passed to the AE/SE.
 * Word 5
 *  ei1:64; [383:320] Engine instruction word 1. Passed to the AE/SE.
 * Word 6
 *  ei2:64; [447:384] Engine instruction word 1. Passed to the AE/SE.
 * Word 7
 *  ei3:64; [511:448] Engine instruction word 1. Passed to the AE/SE.
 *
 */
union cpt_inst_s {
	uint64_t u[8];
	struct cpt_inst_s_s {
#if defined(__BIG_ENDIAN_BITFIELD) /* Word 0 - Big Endian */
		uint64_t reserved_17_63:47;
		uint64_t doneint:1;
		uint64_t reserved_0_1:16;
#else /* Word 0 - Little Endian */
		uint64_t reserved_0_15:16;
		uint64_t doneint:1;
		uint64_t reserved_17_63:47;
#endif /* Word 0 - End */
#if defined(__BIG_ENDIAN_BITFIELD) /* Word 1 - Big Endian */
		uint64_t res_addr:64;
#else /* Word 1 - Little Endian */
		uint64_t res_addr:64;
#endif /* Word 1 - End */
#if defined(__BIG_ENDIAN_BITFIELD) /* Word 2 - Big Endian */
		uint64_t reserved_172_19:20;
		uint64_t grp:10;
		uint64_t tt:2;
		uint64_t tag:32;
#else /* Word 2 - Little Endian */
		uint64_t tag:32;
		uint64_t tt:2;
		uint64_t grp:10;
		uint64_t reserved_172_191:20;
#endif /* Word 2 - End */
#if defined(__BIG_ENDIAN_BITFIELD) /* Word 3 - Big Endian */
		uint64_t wq_ptr:64;
#else /* Word 3 - Little Endian */
		uint64_t wq_ptr:64;
#endif /* Word 3 - End */
#if defined(__BIG_ENDIAN_BITFIELD) /* Word 4 - Big Endian */
		uint64_t ei0:64;
#else /* Word 4 - Little Endian */
		uint64_t ei0:64;
#endif /* Word 4 - End */
#if defined(__BIG_ENDIAN_BITFIELD) /* Word 5 - Big Endian */
		uint64_t ei1:64;
#else /* Word 5 - Little Endian */
		uint64_t ei1:64;
#endif /* Word 5 - End */
#if defined(__BIG_ENDIAN_BITFIELD) /* Word 6 - Big Endian */
		uint64_t ei2:64;
#else /* Word 6 - Little Endian */
		uint64_t ei2:64;
#endif /* Word 6 - End */
#if defined(__BIG_ENDIAN_BITFIELD) /* Word 7 - Big Endian */
		uint64_t ei3:64;
#else /* Word 7 - Little Endian */
		uint64_t ei3:64;
#endif /* Word 7 - End */
	} s;
};

/**
 * Structure cpt_res_s
 *
 * CPT Result Structure
 * The CPT coprocessor writes the result structure after it completes a
 * CPT_INST_S instruction. The result structure is exactly 16 bytes, and
 * each instruction completion produces exactly one result structure.
 *
 * This structure is stored in memory as little-endian unless
 * CPT()_PF_Q()_CTL[INST_BE] is set.
 * cpt_res_s_s
 * Word 0
 *  doneint:1 [16:16] Done interrupt. This bit is copied from the
 *	corresponding instruction's CPT_INST_S[DONEINT].
 *  compcode:8 [7:0] Indicates completion/error status of the CPT coprocessor
 *	for the	associated instruction, as enumerated by CPT_COMP_E.
 *	Core software may write the memory location containing [COMPCODE] to
 *	0x0 before ringing the doorbell, and then poll for completion by
 *	checking for a nonzero value.
 *	Once the core observes a nonzero [COMPCODE] value in this case,the CPT
 *	coprocessor will have also completed L2/DRAM write operations.
 * Word 1
 *  reserved
 *
 */
union cpt_res_s {
	uint64_t u[2];
	struct cpt_res_s_s {
#if defined(__BIG_ENDIAN_BITFIELD) /* Word 0 - Big Endian */
		uint64_t reserved_17_63:47;
		uint64_t doneint:1;
		uint64_t reserved_8_15:8;
		uint64_t compcode:8;
#else /* Word 0 - Little Endian */
		uint64_t compcode:8;
		uint64_t reserved_8_15:8;
		uint64_t doneint:1;
		uint64_t reserved_17_63:47;
#endif /* Word 0 - End */
#if defined(__BIG_ENDIAN_BITFIELD) /* Word 1 - Big Endian */
		uint64_t reserved_64_127:64;
#else /* Word 1 - Little Endian */
		uint64_t reserved_64_127:64;
#endif /* Word 1 - End */
	} s;
};

/**
 * Register (NCB) cpt#_pf_bist_status
 *
 * CPT PF Control Bist Status Register
 * This register has the BIST status of memories. Each bit is the BIST result
 * of an individual memory (per bit, 0 = pass and 1 = fail).
 * cptx_pf_bist_status_s
 * Word0
 *  bstatus [29:0](RO/H) BIST status. One bit per memory, enumerated by
 *	CPT_RAMS_E.
 */
union cptx_pf_bist_status {
	uint64_t u;
	struct cptx_pf_bist_status_s {
#if defined(__BIG_ENDIAN_BITFIELD) /* Word 0 - Big Endian */
		uint64_t reserved_30_63:34;
		uint64_t bstatus:30;
#else /* Word 0 - Little Endian */
		uint64_t bstatus:30;
		uint64_t reserved_30_63:34;
#endif /* Word 0 - End */
	} s;
};

/**
 * Register (NCB) cpt#_pf_constants
 *
 * CPT PF Constants Register
 * This register contains implementation-related parameters of CPT in CNXXXX.
 * cptx_pf_constants_s
 * Word 0
 *  reserved_40_63:24 [63:40] Reserved.
 *  epcis:8 [39:32](RO) Number of EPCI busses.
 *  grps:8 [31:24](RO) Number of engine groups implemented.
 *  ae:8 [23:16](RO/H) Number of AEs. In CNXXXX, for CPT0 returns 0x0,
 *	for CPT1 returns 0x18, or less if there are fuse-disables.
 *  se:8 [15:8](RO/H) Number of SEs. In CNXXXX, for CPT0 returns 0x30,
 *	or less if there are fuse-disables, for CPT1 returns 0x0.
 *  vq:8 [7:0](RO) Number of VQs.
 * cptx_pf_constants_cn81xx
 * Word 0
 *  reserved_40_63:24 [63:40] Reserved
 *  epcis:8 [39:32](RO) Number of EPCI busses.
 *  grps:8 [31:24](RO) Number of engine groups implemented.
 *  ae:8 [23:16](RO/H) Number of AEs. In CNXXXX, returns 0x6 or less
 *	if there are fuse-disables.
 *  se:8 [15: 8](RO/H) Number of SEs. In CNXXXX, returns 0xA, or less
 *	if there are fuse-disables.
 *  vq:8 [7:0](RO) Number of VQs.
 *
 */
union cptx_pf_constants {
	uint64_t u;
	struct cptx_pf_constants_s {
#if defined(__BIG_ENDIAN_BITFIELD) /* Word 0 - Big Endian */
		uint64_t reserved_40_63:24;
		uint64_t epcis:8;
		uint64_t grps:8;
		uint64_t ae:8;
		uint64_t se:8;
		uint64_t vq:8;
#else /* Word 0 - Little Endian */
		uint64_t vq:8;
		uint64_t se:8;
		uint64_t ae:8;
		uint64_t grps:8;
		uint64_t epcis:8;
		uint64_t reserved_40_63:24;
#endif /* Word 0 - End */
	} s;
	struct cptx_pf_constants_cn81xx {
#if defined(__BIG_ENDIAN_BITFIELD) /* Word 0 - Big Endian */
		uint64_t reserved_40_63:24;
		uint64_t epcis:8;
		uint64_t grps:8;
		uint64_t ae:8;
		uint64_t se:8;
		uint64_t vq:8;
#else /* Word 0 - Little Endian */
		uint64_t vq:8;
		uint64_t se:8;
		uint64_t ae:8;
		uint64_t grps:8;
		uint64_t epcis:8;
		uint64_t reserved_40_63:24;
#endif /* Word 0 - End */
	} cn81xx;
};

/**
 * Register (NCB) cpt#_pf_exe_bist_status
 *
 * CPT PF Engine Bist Status Register
 * This register has the BIST status of each engine.  Each bit is the
 * BIST result of an individual engine (per bit, 0 = pass and 1 = fail).
 * cptx_pf_exe_bist_status_s
 * Word0
 *  reserved_48_63:16 [63:48] reserved
 *  bstatus:48 [47:0](RO/H) BIST status. One bit per engine.
 *
 */
union cptx_pf_exe_bist_status {
	uint64_t u;
	struct cptx_pf_exe_bist_status_s {
#if defined(__BIG_ENDIAN_BITFIELD) /* Word 0 - Big Endian */
		uint64_t reserved_48_63:16;
		uint64_t bstatus:48
#else /* Word 0 - Little Endian */
		uint64_t bstatus:48;
		uint64_t reserved_48_63:16;
#endif /* Word 0 - End */
	} s;
	struct cptx_pf_exe_bist_status_cn81xx {
#if defined(__BIG_ENDIAN_BITFIELD) /* Word 0 - Big Endian */
		uint64_t reserved_16_63:48;
		uint64_t bstatus:16;
#else /* Word 0 - Little Endian */
		uint64_t bstatus:16;
		uint64_t reserved_16_63:48;
#endif /* Word 0 - End */
	} cn81xx;
};

/**
 * Register (NCB) cpt#_pf_exe_ctl
 *
 * CPT PF Engine Control Register
 * This register enables the engines.
 * cptx_pf_exe_ctl_s
 * Word0
 *  enable:64 [63:0](R/W) Individual enables for each of the engines.
 */
union cptx_pf_exe_ctl {
	uint64_t u;
	struct cptx_pf_exe_ctl_s {
#if defined(__BIG_ENDIAN_BITFIELD) /* Word 0 - Big Endian */
		uint64_t enable:64;
#else /* Word 0 - Little Endian */
		uint64_t enable:64;
#endif /* Word 0 - End */
	} s;
};

/**
 * Register (NCB) cpt#_pf_q#_ctl
 *
 * CPT Queue Control Register
 * This register configures queues. This register should be changed only
 * when quiescent (see CPT()_VQ()_INPROG[INFLIGHT]).
 * cptx_pf_qx_ctl_s
 * Word0
 *  reserved_60_63:4 [63:60] reserved.
 *  aura:12; [59:48](R/W) Guest-aura for returning this queue's
 *	instruction-chunk buffers to FPA. Only used when [INST_FREE] is set.
 *	For the FPA to not discard the request, FPA_PF_MAP() must map
 *	[AURA] and CPT()_PF_Q()_GMCTL[GMID] as valid.
 *  reserved_45_47:3 [47:45] reserved.
 *  size:13 [44:32](R/W) Command-buffer size, in number of 64-bit words per
 *	command buffer segment. Must be 8*n + 1, where n is the number of
 *	instructions per buffer segment.
 *  reserved_11_31:21 [31:11] Reserved.
 *  cont_err:1 [10:10](R/W) Continue on error.
 *	0 = When CPT()_VQ()_MISC_INT[NWRP], CPT()_VQ()_MISC_INT[IRDE] or
 *	CPT()_VQ()_MISC_INT[DOVF] are set by hardware or software via
 *	CPT()_VQ()_MISC_INT_W1S, then CPT()_VQ()_CTL[ENA] is cleared.  Due to
 *	pipelining, additional instructions may have been processed between the
 *	instruction causing the error and the next instruction in the disabled
 *	queue (the instruction at CPT()_VQ()_SADDR).
 *	1 = Ignore errors and continue processing instructions.
 *	For diagnostic use only.
 *  inst_free:1 [9:9](R/W) Instruction FPA free. When set, when CPT reaches the
 *	end of an instruction chunk, that chunk will be freed to the FPA.
 *  inst_be:1 [8:8](R/W) Instruction big-endian control. When set, instructions,
 *	instruction next chunk pointers, and result structures are stored in
 *	big-endian format in memory.
 *  iqb_ldwb:1 [7:7](R/W) Instruction load don't write back.
 *	0 = The hardware issues NCB transient load (LDT) towards the cache,
 *	which if the line hits and is is dirty will cause the line to be
 *	written back before being replaced.
 *	1 = The hardware issues NCB LDWB read-and-invalidate command towards
 *	the cache when fetching the last word of instructions; as a result the
 *	line will not be written back when replaced.  This improves
 *	performance, but software must not read the instructions after they are
 *	posted to the hardware.	Reads that do not consume the last word of a
 *	cache line always use LDI.
 *  reserved_4_6:3 [6:4] Reserved.
 *  grp:3; [3:1](R/W) Engine group.
 *  pri:1; [0:0](R/W) Queue priority.
 *	1 = This queue has higher priority. Round-robin between higher
 *	priority queues.
 *	0 = This queue has lower priority. Round-robin between lower
 *	priority queues.
 */
union cptx_pf_qx_ctl {
	uint64_t u;
	struct cptx_pf_qx_ctl_s {
#if defined(__BIG_ENDIAN_BITFIELD) /* Word 0 - Big Endian */
		uint64_t reserved_60_63:4;
		uint64_t aura:12;
		uint64_t reserved_45_47:3;
		uint64_t size:13;
		uint64_t reserved_11_31:21;
		uint64_t cont_err:1;
		uint64_t inst_free:1;
		uint64_t inst_be:1;
		uint64_t iqb_ldwb:1;
		uint64_t reserved_4_6:3;
		uint64_t grp:3;
		uint64_t pri:1;
#else /* Word 0 - Little Endian */
		uint64_t pri:1;
		uint64_t grp:3;
		uint64_t reserved_4_6:3;
		uint64_t iqb_ldwb:1;
		uint64_t inst_be:1;
		uint64_t inst_free:1;
		uint64_t cont_err:1;
		uint64_t reserved_11_31:21;
		uint64_t size:13;
		uint64_t reserved_45_47:3;
		uint64_t aura:12;
		uint64_t reserved_60_63:4;
#endif /* Word 0 - End */
	} s;
    /* struct cptx_pf_qx_ctl_s cn; */
};

/**
 * Register (NCB) cpt#_pf_g#_en
 *
 * CPT PF Group Control Register
 * This register configures engine groups.
 * cptx_pf_gx_en_s
 * Word0
 *  en: 64; [63:0](R/W/H) Engine group enable. One bit corresponds to each
 *	engine, with the bit set to indicate this engine can service this group.
 *	Bits corresponding to unimplemented engines read as zero, i.e. only bit
 *	numbers	less than CPT()_PF_CONSTANTS[AE] + CPT()_PF_CONSTANTS[SE] are
 *	writable. AE engine bits follow SE engine bits.
 *	E.g. if CPT()_PF_CONSTANTS[AE] = 0x1, and CPT()_PF_CONSTANTS[SE] = 0x2,
 *	then bits <2:0> are read/writable with bit <2> corresponding to AE<0>,
 *	and bit <1> to SE<1>, and bit<0> to SE<0>. Before disabling an engine,
 *	the corresponding bit in each group must be cleared. CPT()_PF_EXEC_BUSY
 *	can then be polled to determing when the engine becomes	idle.
 *	At the point, the engine can be disabled.
 */
union cptx_pf_gx_en {
	uint64_t u;
	struct cptx_pf_gx_en_s {
#if defined(__BIG_ENDIAN_BITFIELD) /* Word 0 - Big Endian */
		uint64_t en:64;
#else /* Word 0 - Little Endian */
		uint64_t en:64;
#endif /* Word 0 - End */
	} s;
};

/**
 * Register (NCB) cpt#_vq#_saddr
 *
 * CPT Queue Starting Buffer Address Registers
 * These registers set the instruction buffer starting address.
 * cptx_vqx_saddr_s
 * Word0
 *  reserved_49_63:15 [63:49] Reserved.
 *  ptr:43 [48:6](R/W/H) Instruction buffer IOVA <48:6> (64-byte aligned).
 *	When written, it is the initial buffer starting address; when read,
 *	it is the next read pointer to be requested from L2C. The PTR field
 *	is overwritten with the next pointer each time that the command buffer
 *	segment is exhausted. New commands will then be read from the newly
 *	specified command buffer pointer.
 *  reserved_0_5:6 [5:0] Reserved.
 *
 */
union cptx_vqx_saddr {
	uint64_t u;
	struct cptx_vqx_saddr_s {
#if defined(__BIG_ENDIAN_BITFIELD) /* Word 0 - Big Endian */
		uint64_t reserved_49_63:15;
		uint64_t ptr:43
		uint64_t reserved_0_5:6;
#else /* Word 0 - Little Endian */
		uint64_t reserved_0_5:6;
		uint64_t ptr:43;
		uint64_t reserved_49_63:15;
#endif /* Word 0 - End */
	} s;
};

/**
 * Register (NCB) cpt#_vq#_misc_ena_w1s
 *
 * CPT Queue Misc Interrupt Enable Set Register
 * This register sets interrupt enable bits.
 * cptx_vqx_misc_ena_w1s_s
 * Word0
 * reserved_5_63:59 [63:5] Reserved.
 * swerr:1 [4:4](R/W1S/H) Reads or sets enable for
 *	CPT(0..1)_VQ(0..63)_MISC_INT[SWERR].
 * nwrp:1 [3:3](R/W1S/H) Reads or sets enable for
 *	CPT(0..1)_VQ(0..63)_MISC_INT[NWRP].
 * irde:1 [2:2](R/W1S/H) Reads or sets enable for
 *	CPT(0..1)_VQ(0..63)_MISC_INT[IRDE].
 * dovf:1 [1:1](R/W1S/H) Reads or sets enable for
 *	CPT(0..1)_VQ(0..63)_MISC_INT[DOVF].
 * mbox:1 [0:0](R/W1S/H) Reads or sets enable for
 *	CPT(0..1)_VQ(0..63)_MISC_INT[MBOX].
 *
 */
union cptx_vqx_misc_ena_w1s {
	uint64_t u;
	struct cptx_vqx_misc_ena_w1s_s {
#if defined(__BIG_ENDIAN_BITFIELD) /* Word 0 - Big Endian */
		uint64_t reserved_5_63:59;
		uint64_t swerr:1;
		uint64_t nwrp:1;
		uint64_t irde:1;
		uint64_t dovf:1;
		uint64_t mbox:1;
#else /* Word 0 - Little Endian */
		uint64_t mbox:1;
		uint64_t dovf:1;
		uint64_t irde:1;
		uint64_t nwrp:1;
		uint64_t swerr:1;
		uint64_t reserved_5_63:59;
#endif /* Word 0 - End */
	} s;
	struct cptx_vqx_misc_ena_w1s_cn81xx {
#if defined(__BIG_ENDIAN_BITFIELD) /* Word 0 - Big Endian */
		uint64_t reserved_5_63:59;
		uint64_t swerr:1;
		uint64_t nwrp:1;
		uint64_t irde:1;
		uint64_t dovf:1;
		uint64_t mbox:1;
#else /* Word 0 - Little Endian */
		uint64_t mbox:1;
		uint64_t dovf:1;
		uint64_t irde:1;
		uint64_t nwrp:1;
		uint64_t swerr:1;
		uint64_t reserved_5_63:59;
#endif /* Word 0 - End */
	} cn81xx;
};

/**
 * Register (NCB) cpt#_vq#_doorbell
 *
 * CPT Queue Doorbell Registers
 * Doorbells for the CPT instruction queues.
 * cptx_vqx_doorbell_s
 * Word0
 *  reserved_20_63:44 [63:20] Reserved.
 *  dbell_cnt:20 [19:0](R/W/H) Number of instruction queue 64-bit words to add
 *	to the CPT instruction doorbell count. Readback value is the the
 *	current number of pending doorbell requests. If counter overflows
 *	CPT()_VQ()_MISC_INT[DBELL_DOVF] is set. To reset the count back to
 *	zero, write one to clear CPT()_VQ()_MISC_INT_ENA_W1C[DBELL_DOVF],
 *	then write a value of 2^20 minus the read [DBELL_CNT], then write one
 *	to CPT()_VQ()_MISC_INT_W1C[DBELL_DOVF] and
 *	CPT()_VQ()_MISC_INT_ENA_W1S[DBELL_DOVF]. Must be a multiple of 8.
 *	All CPT instructions are 8 words and require a doorbell count of
 *	multiple of 8.
 */
union cptx_vqx_doorbell {
	uint64_t u;
	struct cptx_vqx_doorbell_s {
#if defined(__BIG_ENDIAN_BITFIELD) /* Word 0 - Big Endian */
		uint64_t reserved_20_63:44;
		uint64_t dbell_cnt:20;
#else /* Word 0 - Little Endian */
		uint64_t dbell_cnt:20;
		uint64_t reserved_20_63:44;
#endif /* Word 0 - End */
	} s;
};

/**
 * Register (NCB) cpt#_vq#_inprog
 *
 * CPT Queue In Progress Count Registers
 * These registers contain the per-queue instruction in flight registers.
 * cptx_vqx_inprog_s
 * Word0
 *  reserved_8_63:56 [63:8] Reserved.
 *  inflight:8 [7:0](RO/H) Inflight count. Counts the number of instructions
 *	for the VF for which CPT is fetching, executing or responding to
 *	instructions. However this does not include any interrupts that are
 *	awaiting software handling (CPT()_VQ()_DONE[DONE] != 0x0).
 *	A queue may not be reconfigured until:
 *	1. CPT()_VQ()_CTL[ENA] is cleared by software.
 *	2. [INFLIGHT] is polled until equals to zero.
 */
union cptx_vqx_inprog {
	uint64_t u;
	struct cptx_vqx_inprog_s {
#if defined(__BIG_ENDIAN_BITFIELD) /* Word 0 - Big Endian */
		uint64_t reserved_8_63:56;
		uint64_t inflight:8;
#else /* Word 0 - Little Endian */
		uint64_t inflight:8;
		uint64_t reserved_8_63:56;
#endif /* Word 0 - End */
	} s;
};

/**
 * Register (NCB) cpt#_vq#_misc_int
 *
 * CPT Queue Misc Interrupt Register
 * These registers contain the per-queue miscellaneous interrupts.
 * cptx_vqx_misc_int_s
 * Word 0
 *  reserved_5_63:59 [63:5] Reserved.
 *  swerr:1 [4:4](R/W1C/H) Software error from engines.
 *  nwrp:1  [3:3](R/W1C/H) NCB result write response error.
 *  irde:1  [2:2](R/W1C/H) Instruction NCB read response error.
 *  dovf:1 [1:1](R/W1C/H) Doorbell overflow.
 *  mbox:1 [0:0](R/W1C/H) PF to VF mailbox interrupt. Set when
 *	CPT()_VF()_PF_MBOX(0) is written.
 *
 */
union cptx_vqx_misc_int {
	uint64_t u;
	struct cptx_vqx_misc_int_s {
#if defined(__BIG_ENDIAN_BITFIELD) /* Word 0 - Big Endian */
		uint64_t reserved_5_63:59;
		uint64_t swerr:1;
		uint64_t nwrp:1;
		uint64_t irde:1;
		uint64_t dovf:1;
		uint64_t mbox:1;
#else /* Word 0 - Little Endian */
		uint64_t mbox:1;
		uint64_t dovf:1;
		uint64_t irde:1;
		uint64_t nwrp:1;
		uint64_t swerr:1;
		uint64_t reserved_5_63:59;
#endif /* Word 0 - End */
	} s;
};

/**
 * Register (NCB) cpt#_vq#_done_ack
 *
 * CPT Queue Done Count Ack Registers
 * This register is written by software to acknowledge interrupts.
 * cptx_vqx_done_ack_s
 * Word0
 *  reserved_20_63:44 [63:20] Reserved.
 *  done_ack:20 [19:0](R/W/H) Number of decrements to CPT()_VQ()_DONE[DONE].
 *	Reads CPT()_VQ()_DONE[DONE]. Written by software to acknowledge
 *	interrupts. If CPT()_VQ()_DONE[DONE] is still nonzero the interrupt
 *	will be re-sent if the conditions described in CPT()_VQ()_DONE[DONE]
 *	are satisfied.
 *
 */
union cptx_vqx_done_ack {
	uint64_t u;
	struct cptx_vqx_done_ack_s {
#if defined(__BIG_ENDIAN_BITFIELD) /* Word 0 - Big Endian */
		uint64_t reserved_20_63:44;
		uint64_t done_ack:20;
#else /* Word 0 - Little Endian */
		uint64_t done_ack:20;
		uint64_t reserved_20_63:44;
#endif /* Word 0 - End */
	} s;
};

/**
 * Register (NCB) cpt#_vq#_done
 *
 * CPT Queue Done Count Registers
 * These registers contain the per-queue instruction done count.
 * cptx_vqx_done_s
 * Word0
 *  reserved_20_63:44 [63:20] Reserved.
 *  done:20 [19:0](R/W/H) Done count. When CPT_INST_S[DONEINT] set and that
 *	instruction completes, CPT()_VQ()_DONE[DONE] is incremented when the
 *	instruction finishes. Write to this field are for diagnostic use only;
 *	instead software writes CPT()_VQ()_DONE_ACK with the number of
 *	decrements for this field.
 *	Interrupts are sent as follows:
 *	* When CPT()_VQ()_DONE[DONE] = 0, then no results are pending, the
 *	interrupt coalescing timer is held to zero, and an interrupt is not
 *	sent.
 *	* When CPT()_VQ()_DONE[DONE] != 0, then the interrupt coalescing timer
 *	counts. If the counter is >= CPT()_VQ()_DONE_WAIT[TIME_WAIT]*1024, or
 *	CPT()_VQ()_DONE[DONE] >= CPT()_VQ()_DONE_WAIT[NUM_WAIT], i.e. enough
 *	time has passed or enough results have arrived, then the interrupt is
 *	sent.
 *	* When CPT()_VQ()_DONE_ACK is written (or CPT()_VQ()_DONE is written
 *	but this is not typical), the interrupt coalescing timer restarts.
 *	Note after decrementing this interrupt equation is recomputed,
 *	for example if CPT()_VQ()_DONE[DONE] >= CPT()_VQ()_DONE_WAIT[NUM_WAIT]
 *	and because the timer is zero, the interrupt will be resent immediately.
 *	(This covers the race case between software acknowledging an interrupt
 *	and a result returning.)
 *	* When CPT()_VQ()_DONE_ENA_W1S[DONE] = 0, interrupts are not sent,
 *	but the counting described above still occurs.
 *	Since CPT instructions complete out-of-order, if software is using
 *	completion interrupts the suggested scheme is to request a DONEINT on
 *	each request, and when an interrupt arrives perform a "greedy" scan for
 *	completions; even if a later command is acknowledged first this will
 *	not result in missing a completion.
 *	Software is responsible for making sure [DONE] does not overflow;
 *	for example by insuring there are not more than 2^20-1 instructions in
 *	flight that may request interrupts.
 *
 */
union cptx_vqx_done {
	uint64_t u;
	struct cptx_vqx_done_s {
#if defined(__BIG_ENDIAN_BITFIELD) /* Word 0 - Big Endian */
		uint64_t reserved_20_63:44;
		uint64_t done:20;
#else /* Word 0 - Little Endian */
		uint64_t done:20;
		uint64_t reserved_20_63:44;
#endif /* Word 0 - End */
	} s;
};

/**
 * Register (NCB) cpt#_vq#_done_wait
 *
 * CPT Queue Done Interrupt Coalescing Wait Registers
 * Specifies the per queue interrupt coalescing settings.
 * cptx_vqx_done_wait_s
 * Word0
 *  reserved_48_63:16 [63:48] Reserved.
 *  time_wait:16; [47:32](R/W) Time hold-off. When CPT()_VQ()_DONE[DONE] = 0
 *	or CPT()_VQ()_DONE_ACK is written a timer is cleared. When the timer
 *	reaches [TIME_WAIT]*1024 then interrupt coalescing ends.
 *	see CPT()_VQ()_DONE[DONE]. If 0x0, time coalescing is disabled.
 *  reserved_20_31:12 [31:20] Reserved.
 *  num_wait:20 [19:0](R/W) Number of messages hold-off.
 *	When CPT()_VQ()_DONE[DONE] >= [NUM_WAIT] then interrupt coalescing ends
 *	see CPT()_VQ()_DONE[DONE]. If 0x0, same behavior as 0x1.
 *
 */
union cptx_vqx_done_wait {
	uint64_t u;
	struct cptx_vqx_done_wait_s {
#if defined(__BIG_ENDIAN_BITFIELD) /* Word 0 - Big Endian */
		uint64_t reserved_48_63:16;
		uint64_t time_wait:16;
		uint64_t reserved_20_31:12;
		uint64_t num_wait:20;
#else /* Word 0 - Little Endian */
		uint64_t num_wait:20;
		uint64_t reserved_20_31:12;
		uint64_t time_wait:16;
		uint64_t reserved_48_63:16;
#endif /* Word 0 - End */
	} s;
};

/**
 * Register (NCB) cpt#_vq#_done_ena_w1s
 *
 * CPT Queue Done Interrupt Enable Set Registers
 * Write 1 to these registers will enable the DONEINT interrupt for the queue.
 * cptx_vqx_done_ena_w1s_s
 * Word0
 *  reserved_1_63:63 [63:1] Reserved.
 *  done:1 [0:0](R/W1S/H) Write 1 will enable DONEINT for this queue.
 *	Write 0 has no effect. Read will return the enable bit.
 */
union cptx_vqx_done_ena_w1s {
	uint64_t u;
	struct cptx_vqx_done_ena_w1s_s {
#if defined(__BIG_ENDIAN_BITFIELD) /* Word 0 - Big Endian */
		uint64_t reserved_1_63:63;
		uint64_t done:1;
#else /* Word 0 - Little Endian */
		uint64_t done:1;
		uint64_t reserved_1_63:63;
#endif /* Word 0 - End */
	} s;
};

/**
 * Register (NCB) cpt#_vq#_ctl
 *
 * CPT VF Queue Control Registers
 * This register configures queues. This register should be changed (other than
 * clearing [ENA]) only when quiescent (see CPT()_VQ()_INPROG[INFLIGHT]).
 * cptx_vqx_ctl_s
 * Word0
 *  reserved_1_63:63 [63:1] Reserved.
 *  ena:1 [0:0](R/W/H) Enables the logical instruction queue.
 *	See also CPT()_PF_Q()_CTL[CONT_ERR] and	CPT()_VQ()_INPROG[INFLIGHT].
 *	1 = Queue is enabled.
 *	0 = Queue is disabled.
 */
union cptx_vqx_ctl {
	uint64_t u;
	struct cptx_vqx_ctl_s {
#if defined(__BIG_ENDIAN_BITFIELD) /* Word 0 - Big Endian */
		uint64_t reserved_1_63:63;
		uint64_t ena:1;
#else /* Word 0 - Little Endian */
		uint64_t ena:1;
		uint64_t reserved_1_63:63;
#endif /* Word 0 - End */
	} s;
};
#endif /*__CPT_HW_TYPES_H*/
