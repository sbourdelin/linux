/*
 * Copyright (C) 2017, Amazon.com, Inc. or its affiliates. All Rights Reserved.
 *
 * This file is licensed under the terms of the GNU General Public
 * License version 2. This program is licensed "as is" without any
 * warranty of any kind, whether express or implied.
 */

#ifndef __AL_HW_UDMA_REG_H
#define __AL_HW_UDMA_REG_H

#include "al_hw_udma_regs_m2s.h"
#include "al_hw_udma_regs_s2m.h"


/* Design programming interface  revision ID */
#define UDMA_GEN_DMA_MISC_REVISION_PROGRAMMING_ID_MASK	0xfff
#define UDMA_GEN_DMA_MISC_REVISION_PROGRAMMING_ID_SHIFT	0x0

struct al_iofic_grp_mod {
	u32 grp_int_mod_reg;
	u32 grp_int_tgtid_reg;
};

struct al_iofic_grp_ctrl {
	u32 int_cause_grp;
	u32 rsrvd1;
	u32 int_cause_set_grp;
	u32 rsrvd2;
	u32 int_mask_grp;
	u32 rsrvd3;
	u32 int_mask_clear_grp;
	u32 rsrvd4;
	u32 int_status_grp;
	u32 rsrvd5;
	u32 int_control_grp;
	u32 rsrvd6;
	u32 int_abort_msk_grp;
	u32 rsrvd7;
	u32 int_log_msk_grp;
	u32 rsrvd8;
};

struct al_iofic_regs {
	struct al_iofic_grp_ctrl ctrl[0];
	u32 rsrvd1[0x100];
	struct al_iofic_grp_mod grp_int_mod[0][32];
};

struct udma_iofic_regs {
	struct al_iofic_regs main_iofic;
	u32 rsrvd1[0x700];
	struct al_iofic_grp_ctrl secondary_iofic_ctrl[2];
};

struct udma_gen_dma_misc {
	u32 int_cfg;
	u32 revision;
	u32 general_cfg_1;
	u32 general_cfg_2;
	u32 general_cfg_3;
	u32 general_cfg_4;
	u32 general_cfg_5;
	u32 rsrvd[57];
};

/*
 * Mailbox interrupt generator.
 * Generates interrupt to neighbor DMA
 */
struct udma_gen_mailbox {
	u32 interrupt;
	u32 msg_out;
	u32 msg_in;
	u32 rsrvd[0xd];
};

struct udma_gen_axi {
	u32 cfg_1;
	u32 cfg_2;
	u32 endian_cfg;
	u32 rsrvd[0x3d];
};

struct udma_gen_regs {
	struct udma_iofic_regs interrupt_regs;
	struct udma_gen_dma_misc dma_misc;
	struct udma_gen_mailbox mailbox[4];
	struct udma_gen_axi axi;
};

/* UDMA registers, either m2s or s2m */
union udma_regs {
	struct udma_m2s_regs m2s;
	struct udma_s2m_regs s2m;
};

struct unit_regs {
	struct udma_m2s_regs m2s;
	u32 rsrvd0[0x2c00];
	struct udma_s2m_regs s2m;
	u32 rsrvd1[0x1c00];
	struct udma_gen_regs gen;
};

/*
 * UDMA submission and completion registers, M2S and S2M UDMAs have same
 * stucture
 */
struct udma_rings_regs {
	u32 rsrvd0[8];
	u32 cfg;		/* Descriptor ring configuration */
	u32 status;		/* Descriptor ring status and information */
	u32 drbp_low;		/* Descriptor Ring Base Pointer [31:4] */
	u32 drbp_high;		/* Descriptor Ring Base Pointer [63:32] */
	u32 drl;		/* Descriptor Ring Length[23:2] */
	u32 drhp;		/* Descriptor Ring Head Pointer */
	u32 drtp_inc;		/* Descriptor Tail Pointer increment */
	u32 drtp;		/* Descriptor Tail Pointer */
	u32 dcp;		/* Descriptor Current Pointer */
	u32 crbp_low;		/* Completion Ring Base Pointer [31:4] */
	u32 crbp_high;		/* Completion Ring Base Pointer [63:32] */
	u32 crhp;		/* Completion Ring Head Pointer */
	u32 crhp_internal;	/* Completion Ring Head Pointer internal */
};

/* M2S and S2M generic structure of Q registers */
union udma_q_regs {
	struct udma_rings_regs	rings;
	struct udma_m2s_q	m2s_q;
	struct udma_s2m_q	s2m_q;
};

#endif /* __AL_HW_UDMA_REG_H */
