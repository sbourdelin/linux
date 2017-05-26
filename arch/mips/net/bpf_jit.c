/*
 * Just-In-Time compiler for BPF filters on MIPS
 *
 * Copyright (c) 2014 Imagination Technologies Ltd.
 * Author: Markos Chandras <markos.chandras@imgtec.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; version 2 of the License.
 */

#include <linux/bitops.h>
#include <linux/compiler.h>
#include <linux/errno.h>
#include <linux/filter.h>
#include <linux/bpf.h>
#include <linux/if_vlan.h>
#include <linux/moduleloader.h>
#include <linux/netdevice.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <asm/asm.h>
#include <asm/bitops.h>
#include <asm/byteorder.h>
#include <asm/cacheflush.h>
#include <asm/cpu-features.h>
#include <asm/uasm.h>

#include "bpf_jit.h"

/* ABI
 * r_skb_hl	SKB header length
 * r_data	SKB data pointer
 * r_off	Offset
 * r_A		BPF register A
 * r_X		BPF register X
 * r_skb	*skb
 * r_M		*scratch memory
 * r_skb_len	SKB length
 *
 * On entry (*bpf_func)(*skb, *filter)
 * a0 = MIPS_R_A0 = skb;
 * a1 = MIPS_R_A1 = filter;
 *
 * Stack
 * ...
 * M[15]
 * M[14]
 * M[13]
 * ...
 * M[0] <-- r_M
 * saved reg k-1
 * saved reg k-2
 * ...
 * saved reg 0 <-- r_sp
 * <no argument area>
 *
 *                     Packet layout
 *
 * <--------------------- len ------------------------>
 * <--skb-len(r_skb_hl)-->< ----- skb->data_len ------>
 * ----------------------------------------------------
 * |                  skb->data                       |
 * ----------------------------------------------------
 */

#define ptr typeof(unsigned long)

#define SCRATCH_OFF(k)		(4 * (k))

/* JIT flags */
#define SEEN_CALL		(1 << BPF_MEMWORDS)
#define SEEN_SREG_SFT		(BPF_MEMWORDS + 1)
#define SEEN_SREG_BASE		(1 << SEEN_SREG_SFT)
#define SEEN_SREG(x)		(SEEN_SREG_BASE << (x))
#define SEEN_OFF		SEEN_SREG(2)
#define SEEN_A			SEEN_SREG(3)
#define SEEN_X			SEEN_SREG(4)
#define SEEN_SKB		SEEN_SREG(5)
#define SEEN_MEM		SEEN_SREG(6)
/* SEEN_SK_DATA also implies skb_hl an skb_len */
#define SEEN_SKB_DATA		(SEEN_SREG(7) | SEEN_SREG(1) | SEEN_SREG(0))

/* Arguments used by JIT */
#define ARGS_USED_BY_JIT	2 /* only applicable to 64-bit */

#define SBIT(x)			(1 << (x)) /* Signed version of BIT() */

/* eBPF uses different flags */
#define EBPF_SAVE_S0	BIT(0)
#define EBPF_SAVE_S1	BIT(1)
#define EBPF_SAVE_S2	BIT(2)
#define EBPF_SAVE_S3	BIT(3)
#define EBPF_SAVE_RA	BIT(4)
#define EBPF_SEEN_FP	BIT(5)

/*
 * For the mips64 ISA, we need to track the value range or type for
 * each JIT register.  The BPF machine requires zero extended 32-bit
 * values, but the mips64 ISA requires sign extended 32-bit values.
 * At each point in the BPF program we track the state of every
 * register so that we can zero extend or sign extend as the BPF
 * semantics require.
 */
enum reg_val_type {
	/* uninitialized */
	REG_UNKNOWN,
	/* not known to be 32-bit compatible. */
	REG_64BIT,
	/* 32-bit compatible, no truncation needed for 64-bit ops. */
	REG_64BIT_32BIT,
	/* 32-bit compatible, need truncation for 64-bit ops. */
	REG_32BIT,
	/* 32-bit zero extended. */
	REG_32BIT_ZERO_EX,
	/* 32-bit no sign/zero extension needed. */
	REG_32BIT_POS
};

/**
 * struct jit_ctx - JIT context
 * @skf:		The sk_filter
 * @prologue_bytes:	Number of bytes for prologue
 * @stack_size:		eBPF stack size
 * @tmp_offset:		eBPF $sp offset to 8-byte temporary memory
 * @idx:		Instruction index
 * @flags:		JIT flags
 * @offsets:		Instruction offsets
 * @target:		Memory location for the compiled filter
 * @reg_val_types	Packed enum reg_val_type for each register.
 */
struct jit_ctx {
	const struct bpf_prog *skf;
	unsigned int prologue_bytes;
	int stack_size;
	int tmp_offset;
	u32 idx;
	u32 flags;
	u32 *offsets;
	u32 *target;
	u64 *reg_val_types;
};

static void set_reg_val_type(u64 *rvt, int reg, enum reg_val_type type)
{
	*rvt &= ~(7ull << (reg * 3));
	*rvt |= ((u64)type << (reg * 3));
}

static enum reg_val_type get_reg_val_type(const struct jit_ctx *ctx,
					  int index, int reg)
{
	return (ctx->reg_val_types[index] >> (reg * 3)) & 7;
}

static inline int optimize_div(u32 *k)
{
	/* power of 2 divides can be implemented with right shift */
	if (!(*k & (*k-1))) {
		*k = ilog2(*k);
		return 1;
	}

	return 0;
}

static inline void emit_jit_reg_move(ptr dst, ptr src, struct jit_ctx *ctx);

/* Simply emit the instruction if the JIT memory space has been allocated */
#define emit_instr(ctx, func, ...)			\
do {							\
	if ((ctx)->target != NULL) {			\
		u32 *p = &(ctx)->target[ctx->idx];	\
		uasm_i_##func(&p, ##__VA_ARGS__);	\
	}						\
	(ctx)->idx++;					\
} while (0)

/*
 * Similar to emit_instr but it must be used when we need to emit
 * 32-bit or 64-bit instructions
 */
#define emit_long_instr(ctx, func, ...)			\
do {							\
	if ((ctx)->target != NULL) {			\
		u32 *p = &(ctx)->target[ctx->idx];	\
		UASM_i_##func(&p, ##__VA_ARGS__);	\
	}						\
	(ctx)->idx++;					\
} while (0)

/* Determine if immediate is within the 16-bit signed range */
static inline bool is_range16(s32 imm)
{
	return !(imm >= SBIT(15) || imm < -SBIT(15));
}

static inline void emit_addu(unsigned int dst, unsigned int src1,
			     unsigned int src2, struct jit_ctx *ctx)
{
	emit_instr(ctx, addu, dst, src1, src2);
}

static inline void emit_nop(struct jit_ctx *ctx)
{
	emit_instr(ctx, nop);
}

/* Load a u32 immediate to a register */
static inline void emit_load_imm(unsigned int dst, u32 imm, struct jit_ctx *ctx)
{
	if (ctx->target != NULL) {
		/* addiu can only handle s16 */
		if (!is_range16(imm)) {
			u32 *p = &ctx->target[ctx->idx];
			uasm_i_lui(&p, r_tmp_imm, (s32)imm >> 16);
			p = &ctx->target[ctx->idx + 1];
			uasm_i_ori(&p, dst, r_tmp_imm, imm & 0xffff);
		} else {
			u32 *p = &ctx->target[ctx->idx];
			uasm_i_addiu(&p, dst, r_zero, imm);
		}
	}
	ctx->idx++;

	if (!is_range16(imm))
		ctx->idx++;
}

static inline void emit_or(unsigned int dst, unsigned int src1,
			   unsigned int src2, struct jit_ctx *ctx)
{
	emit_instr(ctx, or, dst, src1, src2);
}

static inline void emit_ori(unsigned int dst, unsigned src, u32 imm,
			    struct jit_ctx *ctx)
{
	if (imm >= BIT(16)) {
		emit_load_imm(r_tmp, imm, ctx);
		emit_or(dst, src, r_tmp, ctx);
	} else {
		emit_instr(ctx, ori, dst, src, imm);
	}
}

static inline void emit_daddiu(unsigned int dst, unsigned int src,
			       int imm, struct jit_ctx *ctx)
{
	/*
	 * Only used for stack, so the imm is relatively small
	 * and it fits in 15-bits
	 */
	emit_instr(ctx, daddiu, dst, src, imm);
}

static inline void emit_addiu(unsigned int dst, unsigned int src,
			      u32 imm, struct jit_ctx *ctx)
{
	if (!is_range16(imm)) {
		emit_load_imm(r_tmp, imm, ctx);
		emit_addu(dst, r_tmp, src, ctx);
	} else {
		emit_instr(ctx, addiu, dst, src, imm);
	}
}

static inline void emit_and(unsigned int dst, unsigned int src1,
			    unsigned int src2, struct jit_ctx *ctx)
{
	emit_instr(ctx, and, dst, src1, src2);
}

static inline void emit_andi(unsigned int dst, unsigned int src,
			     u32 imm, struct jit_ctx *ctx)
{
	/* If imm does not fit in u16 then load it to register */
	if (imm >= BIT(16)) {
		emit_load_imm(r_tmp, imm, ctx);
		emit_and(dst, src, r_tmp, ctx);
	} else {
		emit_instr(ctx, andi, dst, src, imm);
	}
}

static inline void emit_xor(unsigned int dst, unsigned int src1,
			    unsigned int src2, struct jit_ctx *ctx)
{
	emit_instr(ctx, xor, dst, src1, src2);
}

static inline void emit_xori(ptr dst, ptr src, u32 imm, struct jit_ctx *ctx)
{
	/* If imm does not fit in u16 then load it to register */
	if (imm >= BIT(16)) {
		emit_load_imm(r_tmp, imm, ctx);
		emit_xor(dst, src, r_tmp, ctx);
	} else {
		emit_instr(ctx, xori, dst, src, imm);
	}
}

static inline void emit_stack_offset(int offset, struct jit_ctx *ctx)
{
	emit_long_instr(ctx, ADDIU, r_sp, r_sp, offset);
}

static inline void emit_subu(unsigned int dst, unsigned int src1,
			     unsigned int src2, struct jit_ctx *ctx)
{
	emit_instr(ctx, subu, dst, src1, src2);
}

static inline void emit_neg(unsigned int reg, struct jit_ctx *ctx)
{
	emit_subu(reg, r_zero, reg, ctx);
}

static inline void emit_sllv(unsigned int dst, unsigned int src,
			     unsigned int sa, struct jit_ctx *ctx)
{
	emit_instr(ctx, sllv, dst, src, sa);
}

static inline void emit_sll(unsigned int dst, unsigned int src,
			    unsigned int sa, struct jit_ctx *ctx)
{
	/* sa is 5-bits long */
	if (sa >= BIT(5))
		/* Shifting >= 32 results in zero */
		emit_jit_reg_move(dst, r_zero, ctx);
	else
		emit_instr(ctx, sll, dst, src, sa);
}

static inline void emit_srlv(unsigned int dst, unsigned int src,
			     unsigned int sa, struct jit_ctx *ctx)
{
	emit_instr(ctx, srlv, dst, src, sa);
}

static inline void emit_srl(unsigned int dst, unsigned int src,
			    unsigned int sa, struct jit_ctx *ctx)
{
	/* sa is 5-bits long */
	if (sa >= BIT(5))
		/* Shifting >= 32 results in zero */
		emit_jit_reg_move(dst, r_zero, ctx);
	else
		emit_instr(ctx, srl, dst, src, sa);
}

static inline void emit_slt(unsigned int dst, unsigned int src1,
			    unsigned int src2, struct jit_ctx *ctx)
{
	emit_instr(ctx, slt, dst, src1, src2);
}

static inline void emit_sltu(unsigned int dst, unsigned int src1,
			     unsigned int src2, struct jit_ctx *ctx)
{
	emit_instr(ctx, sltu, dst, src1, src2);
}

static inline void emit_sltiu(unsigned dst, unsigned int src,
			      unsigned int imm, struct jit_ctx *ctx)
{
	/* 16 bit immediate */
	if (!is_range16((s32)imm)) {
		emit_load_imm(r_tmp, imm, ctx);
		emit_sltu(dst, src, r_tmp, ctx);
	} else {
		emit_instr(ctx, sltiu, dst, src, imm);
	}

}

/* Store register on the stack */
static inline void emit_store_stack_reg(ptr reg, ptr base,
					unsigned int offset,
					struct jit_ctx *ctx)
{
	emit_long_instr(ctx, SW, reg, offset, base);
}

static inline void emit_store(ptr reg, ptr base, unsigned int offset,
			      struct jit_ctx *ctx)
{
	emit_instr(ctx, sw, reg, offset, base);
}

static inline void emit_load_stack_reg(ptr reg, ptr base,
				       unsigned int offset,
				       struct jit_ctx *ctx)
{
	emit_long_instr(ctx, LW, reg, offset, base);
}

static inline void emit_load(unsigned int reg, unsigned int base,
			     unsigned int offset, struct jit_ctx *ctx)
{
	emit_instr(ctx, lw, reg, offset, base);
}

static inline void emit_load_byte(unsigned int reg, unsigned int base,
				  unsigned int offset, struct jit_ctx *ctx)
{
	emit_instr(ctx, lb, reg, offset, base);
}

static inline void emit_half_load(unsigned int reg, unsigned int base,
				  unsigned int offset, struct jit_ctx *ctx)
{
	emit_instr(ctx, lh, reg, offset, base);
}

static inline void emit_half_load_unsigned(unsigned int reg, unsigned int base,
					   unsigned int offset, struct jit_ctx *ctx)
{
	emit_instr(ctx, lhu, reg, offset, base);
}

static inline void emit_mul(unsigned int dst, unsigned int src1,
			    unsigned int src2, struct jit_ctx *ctx)
{
	emit_instr(ctx, mul, dst, src1, src2);
}

static inline void emit_div(unsigned int dst, unsigned int src,
			    struct jit_ctx *ctx)
{
	if (ctx->target != NULL) {
		u32 *p = &ctx->target[ctx->idx];
		uasm_i_divu(&p, dst, src);
		p = &ctx->target[ctx->idx + 1];
		uasm_i_mflo(&p, dst);
	}
	ctx->idx += 2; /* 2 insts */
}

static inline void emit_mod(unsigned int dst, unsigned int src,
			    struct jit_ctx *ctx)
{
	if (ctx->target != NULL) {
		u32 *p = &ctx->target[ctx->idx];
		uasm_i_divu(&p, dst, src);
		p = &ctx->target[ctx->idx + 1];
		uasm_i_mfhi(&p, dst);
	}
	ctx->idx += 2; /* 2 insts */
}

static inline void emit_dsll(unsigned int dst, unsigned int src,
			     unsigned int sa, struct jit_ctx *ctx)
{
	emit_instr(ctx, dsll, dst, src, sa);
}

static inline void emit_dsrl32(unsigned int dst, unsigned int src,
			       unsigned int sa, struct jit_ctx *ctx)
{
	emit_instr(ctx, dsrl32, dst, src, sa);
}

static inline void emit_wsbh(unsigned int dst, unsigned int src,
			     struct jit_ctx *ctx)
{
	emit_instr(ctx, wsbh, dst, src);
}

/* load pointer to register */
static inline void emit_load_ptr(unsigned int dst, unsigned int src,
				     int imm, struct jit_ctx *ctx)
{
	/* src contains the base addr of the 32/64-pointer */
	emit_long_instr(ctx, LW, dst, imm, src);
}

/* load a function pointer to register */
static inline void emit_load_func(unsigned int reg, ptr imm,
				  struct jit_ctx *ctx)
{
	if (IS_ENABLED(CONFIG_64BIT)) {
		/* At this point imm is always 64-bit */
		emit_load_imm(r_tmp, (u64)imm >> 32, ctx);
		emit_dsll(r_tmp_imm, r_tmp, 16, ctx); /* left shift by 16 */
		emit_ori(r_tmp, r_tmp_imm, (imm >> 16) & 0xffff, ctx);
		emit_dsll(r_tmp_imm, r_tmp, 16, ctx); /* left shift by 16 */
		emit_ori(reg, r_tmp_imm, imm & 0xffff, ctx);
	} else {
		emit_load_imm(reg, imm, ctx);
	}
}

/* Move to real MIPS register */
static inline void emit_reg_move(ptr dst, ptr src, struct jit_ctx *ctx)
{
	emit_long_instr(ctx, ADDU, dst, src, r_zero);
}

/* Move to JIT (32-bit) register */
static inline void emit_jit_reg_move(ptr dst, ptr src, struct jit_ctx *ctx)
{
	emit_addu(dst, src, r_zero, ctx);
}

/* Compute the immediate value for PC-relative branches. */
static inline u32 b_imm(unsigned int tgt, struct jit_ctx *ctx)
{
	if (ctx->target == NULL)
		return 0;

	/*
	 * We want a pc-relative branch.  tgt is the instruction offset
	 * we want to jump to.

	 * Branch on MIPS:
	 * I: target_offset <- sign_extend(offset)
	 * I+1: PC += target_offset (delay slot)
	 *
	 * ctx->idx currently points to the branch instruction
	 * but the offset is added to the delay slot so we need
	 * to subtract 4.
	 */
	return ctx->offsets[tgt] -
		(ctx->idx * 4 - ctx->prologue_bytes) - 4;
}

static inline void emit_bcond(int cond, unsigned int reg1, unsigned int reg2,
			     unsigned int imm, struct jit_ctx *ctx)
{
	if (ctx->target != NULL) {
		u32 *p = &ctx->target[ctx->idx];

		switch (cond) {
		case MIPS_COND_EQ:
			uasm_i_beq(&p, reg1, reg2, imm);
			break;
		case MIPS_COND_NE:
			uasm_i_bne(&p, reg1, reg2, imm);
			break;
		case MIPS_COND_ALL:
			uasm_i_b(&p, imm);
			break;
		default:
			pr_warn("%s: Unhandled branch conditional: %d\n",
				__func__, cond);
		}
	}
	ctx->idx++;
}

static inline void emit_b(unsigned int imm, struct jit_ctx *ctx)
{
	emit_bcond(MIPS_COND_ALL, r_zero, r_zero, imm, ctx);
}

static inline void emit_jalr(unsigned int link, unsigned int reg,
			     struct jit_ctx *ctx)
{
	emit_instr(ctx, jalr, link, reg);
}

static inline void emit_jr(unsigned int reg, struct jit_ctx *ctx)
{
	emit_instr(ctx, jr, reg);
}

static inline u16 align_sp(unsigned int num)
{
	/* Double word alignment for 32-bit, quadword for 64-bit */
	unsigned int align = IS_ENABLED(CONFIG_64BIT) ? 16 : 8;
	num = (num + (align - 1)) & -align;
	return num;
}

static void save_bpf_jit_regs(struct jit_ctx *ctx, unsigned offset)
{
	int i = 0, real_off = 0;
	u32 sflags, tmp_flags;

	/* Adjust the stack pointer */
	if (offset)
		emit_stack_offset(-align_sp(offset), ctx);

	tmp_flags = sflags = ctx->flags >> SEEN_SREG_SFT;
	/* sflags is essentially a bitmap */
	while (tmp_flags) {
		if ((sflags >> i) & 0x1) {
			emit_store_stack_reg(MIPS_R_S0 + i, r_sp, real_off,
					     ctx);
			real_off += SZREG;
		}
		i++;
		tmp_flags >>= 1;
	}

	/* save return address */
	if (ctx->flags & SEEN_CALL) {
		emit_store_stack_reg(r_ra, r_sp, real_off, ctx);
		real_off += SZREG;
	}

	/* Setup r_M leaving the alignment gap if necessary */
	if (ctx->flags & SEEN_MEM) {
		if (real_off % (SZREG * 2))
			real_off += SZREG;
		emit_long_instr(ctx, ADDIU, r_M, r_sp, real_off);
	}
}

static void restore_bpf_jit_regs(struct jit_ctx *ctx,
				 unsigned int offset)
{
	int i, real_off = 0;
	u32 sflags, tmp_flags;

	tmp_flags = sflags = ctx->flags >> SEEN_SREG_SFT;
	/* sflags is a bitmap */
	i = 0;
	while (tmp_flags) {
		if ((sflags >> i) & 0x1) {
			emit_load_stack_reg(MIPS_R_S0 + i, r_sp, real_off,
					    ctx);
			real_off += SZREG;
		}
		i++;
		tmp_flags >>= 1;
	}

	/* restore return address */
	if (ctx->flags & SEEN_CALL)
		emit_load_stack_reg(r_ra, r_sp, real_off, ctx);

	/* Restore the sp and discard the scrach memory */
	if (offset)
		emit_stack_offset(align_sp(offset), ctx);
}

static unsigned int get_stack_depth(struct jit_ctx *ctx)
{
	int sp_off = 0;


	/* How may s* regs do we need to preserved? */
	sp_off += hweight32(ctx->flags >> SEEN_SREG_SFT) * SZREG;

	if (ctx->flags & SEEN_MEM)
		sp_off += 4 * BPF_MEMWORDS; /* BPF_MEMWORDS are 32-bit */

	if (ctx->flags & SEEN_CALL)
		sp_off += SZREG; /* Space for our ra register */

	return sp_off;
}

static void build_prologue(struct jit_ctx *ctx)
{
	int sp_off;

	/* Calculate the total offset for the stack pointer */
	sp_off = get_stack_depth(ctx);
	save_bpf_jit_regs(ctx, sp_off);

	if (ctx->flags & SEEN_SKB)
		emit_reg_move(r_skb, MIPS_R_A0, ctx);

	if (ctx->flags & SEEN_SKB_DATA) {
		/* Load packet length */
		emit_load(r_skb_len, r_skb, offsetof(struct sk_buff, len),
			  ctx);
		emit_load(r_tmp, r_skb, offsetof(struct sk_buff, data_len),
			  ctx);
		/* Load the data pointer */
		emit_load_ptr(r_skb_data, r_skb,
			      offsetof(struct sk_buff, data), ctx);
		/* Load the header length */
		emit_subu(r_skb_hl, r_skb_len, r_tmp, ctx);
	}

	if (ctx->flags & SEEN_X)
		emit_jit_reg_move(r_X, r_zero, ctx);

	/*
	 * Do not leak kernel data to userspace, we only need to clear
	 * r_A if it is ever used.  In fact if it is never used, we
	 * will not save/restore it, so clearing it in this case would
	 * corrupt the state of the caller.
	 */
	if (bpf_needs_clear_a(&ctx->skf->insns[0]) &&
	    (ctx->flags & SEEN_A))
		emit_jit_reg_move(r_A, r_zero, ctx);
}

static void build_epilogue(struct jit_ctx *ctx)
{
	unsigned int sp_off;

	/* Calculate the total offset for the stack pointer */

	sp_off = get_stack_depth(ctx);
	restore_bpf_jit_regs(ctx, sp_off);

	/* Return */
	emit_jr(r_ra, ctx);
	emit_nop(ctx);
}

#define CHOOSE_LOAD_FUNC(K, func) \
	((int)K < 0 ? ((int)K >= SKF_LL_OFF ? func##_negative : func) : \
	 func##_positive)

static int build_body(struct jit_ctx *ctx)
{
	const struct bpf_prog *prog = ctx->skf;
	const struct sock_filter *inst;
	unsigned int i, off, condt;
	u32 k, b_off __maybe_unused;
	u8 (*sk_load_func)(unsigned long *skb, int offset);

	for (i = 0; i < prog->len; i++) {
		u16 code;

		inst = &(prog->insns[i]);
		pr_debug("%s: code->0x%02x, jt->0x%x, jf->0x%x, k->0x%x\n",
			 __func__, inst->code, inst->jt, inst->jf, inst->k);
		k = inst->k;
		code = bpf_anc_helper(inst);

		if (ctx->target == NULL)
			ctx->offsets[i] = ctx->idx * 4;

		switch (code) {
		case BPF_LD | BPF_IMM:
			/* A <- k ==> li r_A, k */
			ctx->flags |= SEEN_A;
			emit_load_imm(r_A, k, ctx);
			break;
		case BPF_LD | BPF_W | BPF_LEN:
			BUILD_BUG_ON(FIELD_SIZEOF(struct sk_buff, len) != 4);
			/* A <- len ==> lw r_A, offset(skb) */
			ctx->flags |= SEEN_SKB | SEEN_A;
			off = offsetof(struct sk_buff, len);
			emit_load(r_A, r_skb, off, ctx);
			break;
		case BPF_LD | BPF_MEM:
			/* A <- M[k] ==> lw r_A, offset(M) */
			ctx->flags |= SEEN_MEM | SEEN_A;
			emit_load(r_A, r_M, SCRATCH_OFF(k), ctx);
			break;
		case BPF_LD | BPF_W | BPF_ABS:
			/* A <- P[k:4] */
			sk_load_func = CHOOSE_LOAD_FUNC(k, sk_load_word);
			goto load;
		case BPF_LD | BPF_H | BPF_ABS:
			/* A <- P[k:2] */
			sk_load_func = CHOOSE_LOAD_FUNC(k, sk_load_half);
			goto load;
		case BPF_LD | BPF_B | BPF_ABS:
			/* A <- P[k:1] */
			sk_load_func = CHOOSE_LOAD_FUNC(k, sk_load_byte);
load:
			emit_load_imm(r_off, k, ctx);
load_common:
			ctx->flags |= SEEN_CALL | SEEN_OFF |
				SEEN_SKB | SEEN_A | SEEN_SKB_DATA;

			emit_load_func(r_s0, (ptr)sk_load_func, ctx);
			emit_reg_move(MIPS_R_A0, r_skb, ctx);
			emit_jalr(MIPS_R_RA, r_s0, ctx);
			/* Load second argument to delay slot */
			emit_reg_move(MIPS_R_A1, r_off, ctx);
			/* Check the error value */
			emit_bcond(MIPS_COND_EQ, r_ret, 0, b_imm(i + 1, ctx),
				   ctx);
			/* Load return register on DS for failures */
			emit_reg_move(r_ret, r_zero, ctx);
			/* Return with error */
			emit_b(b_imm(prog->len, ctx), ctx);
			emit_nop(ctx);
			break;
		case BPF_LD | BPF_W | BPF_IND:
			/* A <- P[X + k:4] */
			sk_load_func = sk_load_word;
			goto load_ind;
		case BPF_LD | BPF_H | BPF_IND:
			/* A <- P[X + k:2] */
			sk_load_func = sk_load_half;
			goto load_ind;
		case BPF_LD | BPF_B | BPF_IND:
			/* A <- P[X + k:1] */
			sk_load_func = sk_load_byte;
load_ind:
			ctx->flags |= SEEN_OFF | SEEN_X;
			emit_addiu(r_off, r_X, k, ctx);
			goto load_common;
		case BPF_LDX | BPF_IMM:
			/* X <- k */
			ctx->flags |= SEEN_X;
			emit_load_imm(r_X, k, ctx);
			break;
		case BPF_LDX | BPF_MEM:
			/* X <- M[k] */
			ctx->flags |= SEEN_X | SEEN_MEM;
			emit_load(r_X, r_M, SCRATCH_OFF(k), ctx);
			break;
		case BPF_LDX | BPF_W | BPF_LEN:
			/* X <- len */
			ctx->flags |= SEEN_X | SEEN_SKB;
			off = offsetof(struct sk_buff, len);
			emit_load(r_X, r_skb, off, ctx);
			break;
		case BPF_LDX | BPF_B | BPF_MSH:
			/* X <- 4 * (P[k:1] & 0xf) */
			ctx->flags |= SEEN_X | SEEN_CALL | SEEN_SKB;
			/* Load offset to a1 */
			emit_load_func(r_s0, (ptr)sk_load_byte, ctx);
			/*
			 * This may emit two instructions so it may not fit
			 * in the delay slot. So use a0 in the delay slot.
			 */
			emit_load_imm(MIPS_R_A1, k, ctx);
			emit_jalr(MIPS_R_RA, r_s0, ctx);
			emit_reg_move(MIPS_R_A0, r_skb, ctx); /* delay slot */
			/* Check the error value */
			emit_bcond(MIPS_COND_NE, r_ret, 0,
				   b_imm(prog->len, ctx), ctx);
			emit_reg_move(r_ret, r_zero, ctx);
			/* We are good */
			/* X <- P[1:K] & 0xf */
			emit_andi(r_X, r_A, 0xf, ctx);
			/* X << 2 */
			emit_b(b_imm(i + 1, ctx), ctx);
			emit_sll(r_X, r_X, 2, ctx); /* delay slot */
			break;
		case BPF_ST:
			/* M[k] <- A */
			ctx->flags |= SEEN_MEM | SEEN_A;
			emit_store(r_A, r_M, SCRATCH_OFF(k), ctx);
			break;
		case BPF_STX:
			/* M[k] <- X */
			ctx->flags |= SEEN_MEM | SEEN_X;
			emit_store(r_X, r_M, SCRATCH_OFF(k), ctx);
			break;
		case BPF_ALU | BPF_ADD | BPF_K:
			/* A += K */
			ctx->flags |= SEEN_A;
			emit_addiu(r_A, r_A, k, ctx);
			break;
		case BPF_ALU | BPF_ADD | BPF_X:
			/* A += X */
			ctx->flags |= SEEN_A | SEEN_X;
			emit_addu(r_A, r_A, r_X, ctx);
			break;
		case BPF_ALU | BPF_SUB | BPF_K:
			/* A -= K */
			ctx->flags |= SEEN_A;
			emit_addiu(r_A, r_A, -k, ctx);
			break;
		case BPF_ALU | BPF_SUB | BPF_X:
			/* A -= X */
			ctx->flags |= SEEN_A | SEEN_X;
			emit_subu(r_A, r_A, r_X, ctx);
			break;
		case BPF_ALU | BPF_MUL | BPF_K:
			/* A *= K */
			/* Load K to scratch register before MUL */
			ctx->flags |= SEEN_A;
			emit_load_imm(r_s0, k, ctx);
			emit_mul(r_A, r_A, r_s0, ctx);
			break;
		case BPF_ALU | BPF_MUL | BPF_X:
			/* A *= X */
			ctx->flags |= SEEN_A | SEEN_X;
			emit_mul(r_A, r_A, r_X, ctx);
			break;
		case BPF_ALU | BPF_DIV | BPF_K:
			/* A /= k */
			if (k == 1)
				break;
			if (optimize_div(&k)) {
				ctx->flags |= SEEN_A;
				emit_srl(r_A, r_A, k, ctx);
				break;
			}
			ctx->flags |= SEEN_A;
			emit_load_imm(r_s0, k, ctx);
			emit_div(r_A, r_s0, ctx);
			break;
		case BPF_ALU | BPF_MOD | BPF_K:
			/* A %= k */
			if (k == 1) {
				ctx->flags |= SEEN_A;
				emit_jit_reg_move(r_A, r_zero, ctx);
			} else {
				ctx->flags |= SEEN_A;
				emit_load_imm(r_s0, k, ctx);
				emit_mod(r_A, r_s0, ctx);
			}
			break;
		case BPF_ALU | BPF_DIV | BPF_X:
			/* A /= X */
			ctx->flags |= SEEN_X | SEEN_A;
			/* Check if r_X is zero */
			emit_bcond(MIPS_COND_EQ, r_X, r_zero,
				   b_imm(prog->len, ctx), ctx);
			emit_load_imm(r_ret, 0, ctx); /* delay slot */
			emit_div(r_A, r_X, ctx);
			break;
		case BPF_ALU | BPF_MOD | BPF_X:
			/* A %= X */
			ctx->flags |= SEEN_X | SEEN_A;
			/* Check if r_X is zero */
			emit_bcond(MIPS_COND_EQ, r_X, r_zero,
				   b_imm(prog->len, ctx), ctx);
			emit_load_imm(r_ret, 0, ctx); /* delay slot */
			emit_mod(r_A, r_X, ctx);
			break;
		case BPF_ALU | BPF_OR | BPF_K:
			/* A |= K */
			ctx->flags |= SEEN_A;
			emit_ori(r_A, r_A, k, ctx);
			break;
		case BPF_ALU | BPF_OR | BPF_X:
			/* A |= X */
			ctx->flags |= SEEN_A;
			emit_ori(r_A, r_A, r_X, ctx);
			break;
		case BPF_ALU | BPF_XOR | BPF_K:
			/* A ^= k */
			ctx->flags |= SEEN_A;
			emit_xori(r_A, r_A, k, ctx);
			break;
		case BPF_ANC | SKF_AD_ALU_XOR_X:
		case BPF_ALU | BPF_XOR | BPF_X:
			/* A ^= X */
			ctx->flags |= SEEN_A;
			emit_xor(r_A, r_A, r_X, ctx);
			break;
		case BPF_ALU | BPF_AND | BPF_K:
			/* A &= K */
			ctx->flags |= SEEN_A;
			emit_andi(r_A, r_A, k, ctx);
			break;
		case BPF_ALU | BPF_AND | BPF_X:
			/* A &= X */
			ctx->flags |= SEEN_A | SEEN_X;
			emit_and(r_A, r_A, r_X, ctx);
			break;
		case BPF_ALU | BPF_LSH | BPF_K:
			/* A <<= K */
			ctx->flags |= SEEN_A;
			emit_sll(r_A, r_A, k, ctx);
			break;
		case BPF_ALU | BPF_LSH | BPF_X:
			/* A <<= X */
			ctx->flags |= SEEN_A | SEEN_X;
			emit_sllv(r_A, r_A, r_X, ctx);
			break;
		case BPF_ALU | BPF_RSH | BPF_K:
			/* A >>= K */
			ctx->flags |= SEEN_A;
			emit_srl(r_A, r_A, k, ctx);
			break;
		case BPF_ALU | BPF_RSH | BPF_X:
			ctx->flags |= SEEN_A | SEEN_X;
			emit_srlv(r_A, r_A, r_X, ctx);
			break;
		case BPF_ALU | BPF_NEG:
			/* A = -A */
			ctx->flags |= SEEN_A;
			emit_neg(r_A, ctx);
			break;
		case BPF_JMP | BPF_JA:
			/* pc += K */
			emit_b(b_imm(i + k + 1, ctx), ctx);
			emit_nop(ctx);
			break;
		case BPF_JMP | BPF_JEQ | BPF_K:
			/* pc += ( A == K ) ? pc->jt : pc->jf */
			condt = MIPS_COND_EQ | MIPS_COND_K;
			goto jmp_cmp;
		case BPF_JMP | BPF_JEQ | BPF_X:
			ctx->flags |= SEEN_X;
			/* pc += ( A == X ) ? pc->jt : pc->jf */
			condt = MIPS_COND_EQ | MIPS_COND_X;
			goto jmp_cmp;
		case BPF_JMP | BPF_JGE | BPF_K:
			/* pc += ( A >= K ) ? pc->jt : pc->jf */
			condt = MIPS_COND_GE | MIPS_COND_K;
			goto jmp_cmp;
		case BPF_JMP | BPF_JGE | BPF_X:
			ctx->flags |= SEEN_X;
			/* pc += ( A >= X ) ? pc->jt : pc->jf */
			condt = MIPS_COND_GE | MIPS_COND_X;
			goto jmp_cmp;
		case BPF_JMP | BPF_JGT | BPF_K:
			/* pc += ( A > K ) ? pc->jt : pc->jf */
			condt = MIPS_COND_GT | MIPS_COND_K;
			goto jmp_cmp;
		case BPF_JMP | BPF_JGT | BPF_X:
			ctx->flags |= SEEN_X;
			/* pc += ( A > X ) ? pc->jt : pc->jf */
			condt = MIPS_COND_GT | MIPS_COND_X;
jmp_cmp:
			/* Greater or Equal */
			if ((condt & MIPS_COND_GE) ||
			    (condt & MIPS_COND_GT)) {
				if (condt & MIPS_COND_K) { /* K */
					ctx->flags |= SEEN_A;
					emit_sltiu(r_s0, r_A, k, ctx);
				} else { /* X */
					ctx->flags |= SEEN_A |
						SEEN_X;
					emit_sltu(r_s0, r_A, r_X, ctx);
				}
				/* A < (K|X) ? r_scrach = 1 */
				b_off = b_imm(i + inst->jf + 1, ctx);
				emit_bcond(MIPS_COND_NE, r_s0, r_zero, b_off,
					   ctx);
				emit_nop(ctx);
				/* A > (K|X) ? scratch = 0 */
				if (condt & MIPS_COND_GT) {
					/* Checking for equality */
					ctx->flags |= SEEN_A | SEEN_X;
					if (condt & MIPS_COND_K)
						emit_load_imm(r_s0, k, ctx);
					else
						emit_jit_reg_move(r_s0, r_X,
								  ctx);
					b_off = b_imm(i + inst->jf + 1, ctx);
					emit_bcond(MIPS_COND_EQ, r_A, r_s0,
						   b_off, ctx);
					emit_nop(ctx);
					/* Finally, A > K|X */
					b_off = b_imm(i + inst->jt + 1, ctx);
					emit_b(b_off, ctx);
					emit_nop(ctx);
				} else {
					/* A >= (K|X) so jump */
					b_off = b_imm(i + inst->jt + 1, ctx);
					emit_b(b_off, ctx);
					emit_nop(ctx);
				}
			} else {
				/* A == K|X */
				if (condt & MIPS_COND_K) { /* K */
					ctx->flags |= SEEN_A;
					emit_load_imm(r_s0, k, ctx);
					/* jump true */
					b_off = b_imm(i + inst->jt + 1, ctx);
					emit_bcond(MIPS_COND_EQ, r_A, r_s0,
						   b_off, ctx);
					emit_nop(ctx);
					/* jump false */
					b_off = b_imm(i + inst->jf + 1,
						      ctx);
					emit_bcond(MIPS_COND_NE, r_A, r_s0,
						   b_off, ctx);
					emit_nop(ctx);
				} else { /* X */
					/* jump true */
					ctx->flags |= SEEN_A | SEEN_X;
					b_off = b_imm(i + inst->jt + 1,
						      ctx);
					emit_bcond(MIPS_COND_EQ, r_A, r_X,
						   b_off, ctx);
					emit_nop(ctx);
					/* jump false */
					b_off = b_imm(i + inst->jf + 1, ctx);
					emit_bcond(MIPS_COND_NE, r_A, r_X,
						   b_off, ctx);
					emit_nop(ctx);
				}
			}
			break;
		case BPF_JMP | BPF_JSET | BPF_K:
			ctx->flags |= SEEN_A;
			/* pc += (A & K) ? pc -> jt : pc -> jf */
			emit_load_imm(r_s1, k, ctx);
			emit_and(r_s0, r_A, r_s1, ctx);
			/* jump true */
			b_off = b_imm(i + inst->jt + 1, ctx);
			emit_bcond(MIPS_COND_NE, r_s0, r_zero, b_off, ctx);
			emit_nop(ctx);
			/* jump false */
			b_off = b_imm(i + inst->jf + 1, ctx);
			emit_b(b_off, ctx);
			emit_nop(ctx);
			break;
		case BPF_JMP | BPF_JSET | BPF_X:
			ctx->flags |= SEEN_X | SEEN_A;
			/* pc += (A & X) ? pc -> jt : pc -> jf */
			emit_and(r_s0, r_A, r_X, ctx);
			/* jump true */
			b_off = b_imm(i + inst->jt + 1, ctx);
			emit_bcond(MIPS_COND_NE, r_s0, r_zero, b_off, ctx);
			emit_nop(ctx);
			/* jump false */
			b_off = b_imm(i + inst->jf + 1, ctx);
			emit_b(b_off, ctx);
			emit_nop(ctx);
			break;
		case BPF_RET | BPF_A:
			ctx->flags |= SEEN_A;
			if (i != prog->len - 1)
				/*
				 * If this is not the last instruction
				 * then jump to the epilogue
				 */
				emit_b(b_imm(prog->len, ctx), ctx);
			emit_reg_move(r_ret, r_A, ctx); /* delay slot */
			break;
		case BPF_RET | BPF_K:
			/*
			 * It can emit two instructions so it does not fit on
			 * the delay slot.
			 */
			emit_load_imm(r_ret, k, ctx);
			if (i != prog->len - 1) {
				/*
				 * If this is not the last instruction
				 * then jump to the epilogue
				 */
				emit_b(b_imm(prog->len, ctx), ctx);
				emit_nop(ctx);
			}
			break;
		case BPF_MISC | BPF_TAX:
			/* X = A */
			ctx->flags |= SEEN_X | SEEN_A;
			emit_jit_reg_move(r_X, r_A, ctx);
			break;
		case BPF_MISC | BPF_TXA:
			/* A = X */
			ctx->flags |= SEEN_A | SEEN_X;
			emit_jit_reg_move(r_A, r_X, ctx);
			break;
		/* AUX */
		case BPF_ANC | SKF_AD_PROTOCOL:
			/* A = ntohs(skb->protocol */
			ctx->flags |= SEEN_SKB | SEEN_OFF | SEEN_A;
			BUILD_BUG_ON(FIELD_SIZEOF(struct sk_buff,
						  protocol) != 2);
			off = offsetof(struct sk_buff, protocol);
			emit_half_load(r_A, r_skb, off, ctx);
#ifdef CONFIG_CPU_LITTLE_ENDIAN
			/* This needs little endian fixup */
			if (cpu_has_wsbh) {
				/* R2 and later have the wsbh instruction */
				emit_wsbh(r_A, r_A, ctx);
			} else {
				/* Get first byte */
				emit_andi(r_tmp_imm, r_A, 0xff, ctx);
				/* Shift it */
				emit_sll(r_tmp, r_tmp_imm, 8, ctx);
				/* Get second byte */
				emit_srl(r_tmp_imm, r_A, 8, ctx);
				emit_andi(r_tmp_imm, r_tmp_imm, 0xff, ctx);
				/* Put everyting together in r_A */
				emit_or(r_A, r_tmp, r_tmp_imm, ctx);
			}
#endif
			break;
		case BPF_ANC | SKF_AD_CPU:
			ctx->flags |= SEEN_A | SEEN_OFF;
			/* A = current_thread_info()->cpu */
			BUILD_BUG_ON(FIELD_SIZEOF(struct thread_info,
						  cpu) != 4);
			off = offsetof(struct thread_info, cpu);
			/* $28/gp points to the thread_info struct */
			emit_load(r_A, 28, off, ctx);
			break;
		case BPF_ANC | SKF_AD_IFINDEX:
			/* A = skb->dev->ifindex */
		case BPF_ANC | SKF_AD_HATYPE:
			/* A = skb->dev->type */
			ctx->flags |= SEEN_SKB | SEEN_A;
			off = offsetof(struct sk_buff, dev);
			/* Load *dev pointer */
			emit_load_ptr(r_s0, r_skb, off, ctx);
			/* error (0) in the delay slot */
			emit_bcond(MIPS_COND_EQ, r_s0, r_zero,
				   b_imm(prog->len, ctx), ctx);
			emit_reg_move(r_ret, r_zero, ctx);
			if (code == (BPF_ANC | SKF_AD_IFINDEX)) {
				BUILD_BUG_ON(FIELD_SIZEOF(struct net_device, ifindex) != 4);
				off = offsetof(struct net_device, ifindex);
				emit_load(r_A, r_s0, off, ctx);
			} else { /* (code == (BPF_ANC | SKF_AD_HATYPE) */
				BUILD_BUG_ON(FIELD_SIZEOF(struct net_device, type) != 2);
				off = offsetof(struct net_device, type);
				emit_half_load_unsigned(r_A, r_s0, off, ctx);
			}
			break;
		case BPF_ANC | SKF_AD_MARK:
			ctx->flags |= SEEN_SKB | SEEN_A;
			BUILD_BUG_ON(FIELD_SIZEOF(struct sk_buff, mark) != 4);
			off = offsetof(struct sk_buff, mark);
			emit_load(r_A, r_skb, off, ctx);
			break;
		case BPF_ANC | SKF_AD_RXHASH:
			ctx->flags |= SEEN_SKB | SEEN_A;
			BUILD_BUG_ON(FIELD_SIZEOF(struct sk_buff, hash) != 4);
			off = offsetof(struct sk_buff, hash);
			emit_load(r_A, r_skb, off, ctx);
			break;
		case BPF_ANC | SKF_AD_VLAN_TAG:
		case BPF_ANC | SKF_AD_VLAN_TAG_PRESENT:
			ctx->flags |= SEEN_SKB | SEEN_A;
			BUILD_BUG_ON(FIELD_SIZEOF(struct sk_buff,
						  vlan_tci) != 2);
			off = offsetof(struct sk_buff, vlan_tci);
			emit_half_load_unsigned(r_s0, r_skb, off, ctx);
			if (code == (BPF_ANC | SKF_AD_VLAN_TAG)) {
				emit_andi(r_A, r_s0, (u16)~VLAN_TAG_PRESENT, ctx);
			} else {
				emit_andi(r_A, r_s0, VLAN_TAG_PRESENT, ctx);
				/* return 1 if present */
				emit_sltu(r_A, r_zero, r_A, ctx);
			}
			break;
		case BPF_ANC | SKF_AD_PKTTYPE:
			ctx->flags |= SEEN_SKB;

			emit_load_byte(r_tmp, r_skb, PKT_TYPE_OFFSET(), ctx);
			/* Keep only the last 3 bits */
			emit_andi(r_A, r_tmp, PKT_TYPE_MAX, ctx);
#ifdef __BIG_ENDIAN_BITFIELD
			/* Get the actual packet type to the lower 3 bits */
			emit_srl(r_A, r_A, 5, ctx);
#endif
			break;
		case BPF_ANC | SKF_AD_QUEUE:
			ctx->flags |= SEEN_SKB | SEEN_A;
			BUILD_BUG_ON(FIELD_SIZEOF(struct sk_buff,
						  queue_mapping) != 2);
			BUILD_BUG_ON(offsetof(struct sk_buff,
					      queue_mapping) > 0xff);
			off = offsetof(struct sk_buff, queue_mapping);
			emit_half_load_unsigned(r_A, r_skb, off, ctx);
			break;
		default:
			pr_debug("%s: Unhandled opcode: 0x%02x\n", __FILE__,
				 inst->code);
			return -1;
		}
	}

	/* compute offsets only during the first pass */
	if (ctx->target == NULL)
		ctx->offsets[i] = ctx->idx * 4;

	return 0;
}

int bpf_jit_enable __read_mostly;

void bpf_jit_compile(struct bpf_prog *fp)
{
	struct jit_ctx ctx;
	unsigned int alloc_size, tmp_idx;

	if (!bpf_jit_enable)
		return;

	memset(&ctx, 0, sizeof(ctx));

	ctx.offsets = kcalloc(fp->len + 1, sizeof(*ctx.offsets), GFP_KERNEL);
	if (ctx.offsets == NULL)
		return;

	ctx.skf = fp;

	if (build_body(&ctx))
		goto out;

	tmp_idx = ctx.idx;
	build_prologue(&ctx);
	ctx.prologue_bytes = (ctx.idx - tmp_idx) * 4;
	/* just to complete the ctx.idx count */
	build_epilogue(&ctx);

	alloc_size = 4 * ctx.idx;
	ctx.target = module_alloc(alloc_size);
	if (ctx.target == NULL)
		goto out;

	/* Clean it */
	memset(ctx.target, 0, alloc_size);

	ctx.idx = 0;

	/* Generate the actual JIT code */
	build_prologue(&ctx);
	build_body(&ctx);
	build_epilogue(&ctx);

	/* Update the icache */
	flush_icache_range((ptr)ctx.target, (ptr)(ctx.target + ctx.idx));

	if (bpf_jit_enable > 1)
		/* Dump JIT code */
		bpf_jit_dump(fp->len, alloc_size, 2, ctx.target);

	fp->bpf_func = (void *)ctx.target;
	fp->jited = 1;

out:
	kfree(ctx.offsets);
}

void bpf_jit_free(struct bpf_prog *fp)
{
	if (fp->jited)
		module_memfree(fp->bpf_func);

	bpf_prog_unlock_free(fp);
}

enum which_ebpf_reg {
	src_reg,
	src_reg_no_fp,
	dst_reg,
	dst_reg_fp_ok
};

/*
 * For eBPF, the register mapping naturally falls out of the
 * requirements of eBPF and the MIPS n64 ABI.  We don't maintain a
 * separate frame pointer, so BPF_REG_10 relative accesses are
 * adjusted to be $sp relative.
 */
int ebpf_to_mips_reg(struct jit_ctx *ctx, const struct bpf_insn *insn,
		     enum which_ebpf_reg w)
{
	int ebpf_reg = (w == src_reg || w == src_reg_no_fp) ?
		insn->src_reg : insn->dst_reg;

	switch (ebpf_reg) {
	case BPF_REG_0:
		return MIPS_R_V0;
	case BPF_REG_1:
		return MIPS_R_A0;
	case BPF_REG_2:
		return MIPS_R_A1;
	case BPF_REG_3:
		return MIPS_R_A2;
	case BPF_REG_4:
		return MIPS_R_A3;
	case BPF_REG_5:
		return MIPS_R_A4;
	case BPF_REG_6:
		ctx->flags |= EBPF_SAVE_S0;
		return MIPS_R_S0;
	case BPF_REG_7:
		ctx->flags |= EBPF_SAVE_S1;
		return MIPS_R_S1;
	case BPF_REG_8:
		ctx->flags |= EBPF_SAVE_S2;
		return MIPS_R_S2;
	case BPF_REG_9:
		ctx->flags |= EBPF_SAVE_S3;
		return MIPS_R_S3;
	case BPF_REG_10:
		if (w == dst_reg || w == src_reg_no_fp)
			goto bad_reg;
		ctx->flags |= EBPF_SEEN_FP;
		/*
		 * Needs special handling, return something that
		 * cannot be clobbered just in case.
		 */
		return MIPS_R_ZERO;
	default:
bad_reg:
		WARN(1, "Illegal bpf reg: %d\n", ebpf_reg);
		return -EINVAL;
	}
}
/*
 * eBPF stack frame will be something like:
 *
 *  Entry $sp ------>   +--------------------------------+
 *                      |   $ra  (optional)              |
 *                      +--------------------------------+
 *                      |   $s0  (optional)              |
 *                      +--------------------------------+
 *                      |   $s1  (optional)              |
 *                      +--------------------------------+
 *                      |   $s2  (optional)              |
 *                      +--------------------------------+
 *                      |   $s3  (optional)              |
 *                      +--------------------------------+
 *                      |   tmp-storage  (if $ra saved)  |
 * $sp + tmp_offset --> +--------------------------------+ <--BPF_REG_10
 *                      |   BPF_REG_10 relative storage  |
 *                      |    MAX_BPF_STACK (optional)    |
 *                      |      .                         |
 *                      |      .                         |
 *                      |      .                         |
 *     $sp -------->    +--------------------------------+
 *
 * If BPF_REG_10 is never referenced, then the MAX_BPF_STACK sized
 * area is not allocated.
 */
static int gen_int_prologue(struct jit_ctx *ctx)
{
	int stack_adjust = 0;
	int store_offset;
	int locals_size;

	if (ctx->flags & EBPF_SAVE_RA)
		/*
		 * If RA we are doing a function call and may need
		 * extra 8-byte tmp area.
		 */
		stack_adjust += 16;
	if (ctx->flags & EBPF_SAVE_S0)
		stack_adjust += 8;
	if (ctx->flags & EBPF_SAVE_S1)
		stack_adjust += 8;
	if (ctx->flags & EBPF_SAVE_S2)
		stack_adjust += 8;
	if (ctx->flags & EBPF_SAVE_S3)
		stack_adjust += 8;

	BUILD_BUG_ON(MAX_BPF_STACK & 7);
	locals_size = (ctx->flags & EBPF_SEEN_FP) ? MAX_BPF_STACK : 0;

	stack_adjust += locals_size;
	ctx->tmp_offset = locals_size;

	ctx->stack_size = stack_adjust;
	if (stack_adjust)
		emit_instr(ctx, daddiu, MIPS_R_SP, MIPS_R_SP, -stack_adjust);
	else
		return 0;

	store_offset = stack_adjust - 8;

	if (ctx->flags & EBPF_SAVE_RA) {
		emit_instr(ctx, sd, MIPS_R_RA, store_offset, MIPS_R_SP);
		store_offset -= 8;
	}
	if (ctx->flags & EBPF_SAVE_S0) {
		emit_instr(ctx, sd, MIPS_R_S0, store_offset, MIPS_R_SP);
		store_offset -= 8;
	}
	if (ctx->flags & EBPF_SAVE_S1) {
		emit_instr(ctx, sd, MIPS_R_S1, store_offset, MIPS_R_SP);
		store_offset -= 8;
	}
	if (ctx->flags & EBPF_SAVE_S2) {
		emit_instr(ctx, sd, MIPS_R_S2, store_offset, MIPS_R_SP);
		store_offset -= 8;
	}
	if (ctx->flags & EBPF_SAVE_S3) {
		emit_instr(ctx, sd, MIPS_R_S3, store_offset, MIPS_R_SP);
		store_offset -= 8;
	}

	return 0;
}

static int build_int_epilogue(struct jit_ctx *ctx)
{
	const struct bpf_prog *prog = ctx->skf;
	int stack_adjust = ctx->stack_size;
	int store_offset = stack_adjust - 8;
	int r0 = MIPS_R_V0;

	if (get_reg_val_type(ctx, prog->len, BPF_REG_0) == REG_32BIT_ZERO_EX)
		/* Don't let zero extended value escape. */
		emit_instr(ctx, sll, r0, r0, 0);

	if (ctx->flags & EBPF_SAVE_RA) {
		emit_instr(ctx, ld, MIPS_R_RA, store_offset, MIPS_R_SP);
		store_offset -= 8;
	}
	if (ctx->flags & EBPF_SAVE_S0) {
		emit_instr(ctx, ld, MIPS_R_S0, store_offset, MIPS_R_SP);
		store_offset -= 8;
	}
	if (ctx->flags & EBPF_SAVE_S1) {
		emit_instr(ctx, ld, MIPS_R_S1, store_offset, MIPS_R_SP);
		store_offset -= 8;
	}
	if (ctx->flags & EBPF_SAVE_S2) {
		emit_instr(ctx, ld, MIPS_R_S2, store_offset, MIPS_R_SP);
		store_offset -= 8;
	}
	if (ctx->flags & EBPF_SAVE_S3) {
		emit_instr(ctx, ld, MIPS_R_S3, store_offset, MIPS_R_SP);
		store_offset -= 8;
	}
	emit_jr(MIPS_R_RA, ctx);

	if (stack_adjust)
		emit_instr(ctx, daddiu, MIPS_R_SP, MIPS_R_SP, stack_adjust);
	else
		emit_nop(ctx);

	return 0;
}

static void gen_imm_to_reg(const struct bpf_insn *insn, int reg,
			   struct jit_ctx *ctx)
{
	if (insn->imm >= S16_MIN && insn->imm <= S16_MAX) {
		emit_instr(ctx, addiu, reg, MIPS_R_ZERO, insn->imm);
	} else {
		int lower = (s16)(insn->imm & 0xffff);
		int upper = insn->imm - lower;

		emit_instr(ctx, lui, reg, upper >> 16);
		emit_instr(ctx, addiu, reg, reg, lower);
	}

}

static int gen_imm_insn(const struct bpf_insn *insn, struct jit_ctx *ctx,
			int idx)
{
	int upper_bound, lower_bound;
	int dst = ebpf_to_mips_reg(ctx, insn, dst_reg);

	if (dst < 0)
		return dst;

	switch (BPF_OP(insn->code)) {
	case BPF_MOV:
	case BPF_ADD:
		upper_bound = S16_MAX;
		lower_bound = S16_MIN;
		break;
	case BPF_SUB:
		upper_bound = -(int)S16_MIN;
		lower_bound = -(int)S16_MAX;
		break;
	case BPF_AND:
	case BPF_OR:
	case BPF_XOR:
		upper_bound = 0xffff;
		lower_bound = 0;
		break;
	case BPF_RSH:
	case BPF_LSH:
	case BPF_ARSH:
		upper_bound = (BPF_CLASS(insn->code) == BPF_ALU64) ? 63 : 31;
		lower_bound = 0;
		break;
	default:
		return -EINVAL;
	}

	/*
	 * Immediate move clobbers the register, so no sign/zero
	 * extension needed.
	 */
	if (BPF_CLASS(insn->code) == BPF_ALU64 &&
	    BPF_OP(insn->code) != BPF_MOV &&
	    get_reg_val_type(ctx, idx, insn->dst_reg) == REG_32BIT)
		emit_instr(ctx, dinsu, dst, MIPS_R_ZERO, 32, 32);
	/* BPF_ALU | BPF_LSH doesn't need separate sign extension */
	if (BPF_CLASS(insn->code) == BPF_ALU &&
	    BPF_OP(insn->code) != BPF_LSH &&
	    BPF_OP(insn->code) != BPF_MOV &&
	    get_reg_val_type(ctx, idx, insn->dst_reg) != REG_32BIT)
		emit_instr(ctx, sll, dst, dst, 0);

	if (insn->imm >= lower_bound && insn->imm <= upper_bound) {
		/* single insn immediate case */
		switch (BPF_OP(insn->code) | BPF_CLASS(insn->code)) {
		case BPF_ALU64 | BPF_MOV:
			emit_instr(ctx, daddiu, dst, MIPS_R_ZERO, insn->imm);
			break;
		case BPF_ALU64 | BPF_AND:
		case BPF_ALU | BPF_AND:
			emit_instr(ctx, andi, dst, dst, insn->imm);
			break;
		case BPF_ALU64 | BPF_OR:
		case BPF_ALU | BPF_OR:
			emit_instr(ctx, ori, dst, dst, insn->imm);
			break;
		case BPF_ALU64 | BPF_XOR:
		case BPF_ALU | BPF_XOR:
			emit_instr(ctx, xori, dst, dst, insn->imm);
			break;
		case BPF_ALU64 | BPF_ADD:
			emit_instr(ctx, daddiu, dst, dst, insn->imm);
			break;
		case BPF_ALU64 | BPF_SUB:
			emit_instr(ctx, daddiu, dst, dst, -insn->imm);
			break;
		case BPF_ALU64 | BPF_RSH:
			emit_instr(ctx, dsrl_safe, dst, dst, insn->imm);
			break;
		case BPF_ALU | BPF_RSH:
			emit_instr(ctx, srl, dst, dst, insn->imm);
			break;
		case BPF_ALU64 | BPF_LSH:
			emit_instr(ctx, dsll_safe, dst, dst, insn->imm);
			break;
		case BPF_ALU | BPF_LSH:
			emit_instr(ctx, sll, dst, dst, insn->imm);
			break;
		case BPF_ALU64 | BPF_ARSH:
			emit_instr(ctx, dsra_safe, dst, dst, insn->imm);
			break;
		case BPF_ALU | BPF_ARSH:
			emit_instr(ctx, sra, dst, dst, insn->imm);
			break;
		case BPF_ALU | BPF_MOV:
			emit_instr(ctx, addiu, dst, MIPS_R_ZERO, insn->imm);
			break;
		case BPF_ALU | BPF_ADD:
			emit_instr(ctx, addiu, dst, dst, insn->imm);
			break;
		case BPF_ALU | BPF_SUB:
			emit_instr(ctx, addiu, dst, dst, -insn->imm);
			break;
		default:
			return -EINVAL;
		}
	} else {
		/* multi insn immediate case */
		if (BPF_OP(insn->code) == BPF_MOV) {
			gen_imm_to_reg(insn, dst, ctx);
		} else {
			gen_imm_to_reg(insn, MIPS_R_AT, ctx);
			switch (BPF_OP(insn->code) | BPF_CLASS(insn->code)) {
			case BPF_ALU64 | BPF_AND:
			case BPF_ALU | BPF_AND:
				emit_instr(ctx, and, dst, dst, MIPS_R_AT);
				break;
			case BPF_ALU64 | BPF_OR:
			case BPF_ALU | BPF_OR:
				emit_instr(ctx, or, dst, dst, MIPS_R_AT);
				break;
			case BPF_ALU64 | BPF_XOR:
			case BPF_ALU | BPF_XOR:
				emit_instr(ctx, xor, dst, dst, MIPS_R_AT);
				break;
			case BPF_ALU64 | BPF_ADD:
				emit_instr(ctx, daddu, dst, dst, MIPS_R_AT);
				break;
			case BPF_ALU64 | BPF_SUB:
				emit_instr(ctx, dsubu, dst, dst, MIPS_R_AT);
				break;
			case BPF_ALU | BPF_ADD:
				emit_instr(ctx, addu, dst, dst, MIPS_R_AT);
				break;
			case BPF_ALU | BPF_SUB:
				emit_instr(ctx, subu, dst, dst, MIPS_R_AT);
				break;
			default:
				return -EINVAL;
			}
		}
	}

	return 0;
}

static void * __must_check
ool_skb_header_pointer(const struct sk_buff *skb, int offset,
		       int len, void *buffer)
{
	return skb_header_pointer(skb, offset, len, buffer);
}

static int size_to_len(const struct bpf_insn *insn)
{
	switch (BPF_SIZE(insn->code)) {
	case BPF_B:
		return 1;
	case BPF_H:
		return 2;
	case BPF_W:
		return 4;
	case BPF_DW:
		return 8;
	}
	return 0;
}

static void emit_const_to_reg(struct jit_ctx *ctx, int dst, u64 value)
{
	if (value >= 0xffffffffffff8000ull || value < 0x8000ull) {
		emit_instr(ctx, daddiu, dst, MIPS_R_ZERO, (int)value);
	} else if (value >= 0xffffffff80000000ull ||
		   (value < 0x80000000 && value > 0xffff)) {
		emit_instr(ctx, lui, dst, (int)(value >> 16));
		emit_instr(ctx, ori, dst, dst, (unsigned int)(value & 0xffff));
	} else {
		int i;
		bool seen_part = false;
		int needed_shift = 0;

		for (i = 0; i < 4; i++) {
			u64 part = (value >> (16 * (3 - i))) & 0xffff;

			if (seen_part && needed_shift > 0 && (part || i == 3)) {
				emit_instr(ctx, dsll_safe, dst, dst, needed_shift);
				needed_shift = 0;
			}
			if (part) {
				emit_instr(ctx, ori, dst, seen_part ? dst : MIPS_R_ZERO, (unsigned int)part);
				seen_part = true;
			}
			if (seen_part)
				needed_shift += 16;
		}
	}
}

static bool use_bbit_insns(void)
{
	switch (current_cpu_type()) {
	case CPU_CAVIUM_OCTEON:
	case CPU_CAVIUM_OCTEON_PLUS:
	case CPU_CAVIUM_OCTEON2:
	case CPU_CAVIUM_OCTEON3:
		return true;
	default:
		return false;
	}
}

static bool is_bad_offset(int b_off)
{
	return b_off > 0x1ffff || b_off < -0x20000;
}

/* Returns the number of insn slots consumed. */
static int build_one_insn(const struct bpf_insn *insn, struct jit_ctx *ctx,
			  int this_idx, int exit_idx)
{
	int src, dst, r, td, ts, mem_off, b_off;
	bool need_swap, did_move, cmp_eq;
	u64 t64;
	s64 t64s;

	switch (insn->code) {
	case BPF_ALU64 | BPF_ADD | BPF_K: /* ALU64_IMM */
	case BPF_ALU64 | BPF_SUB | BPF_K: /* ALU64_IMM */
	case BPF_ALU64 | BPF_OR | BPF_K: /* ALU64_IMM */
	case BPF_ALU64 | BPF_AND | BPF_K: /* ALU64_IMM */
	case BPF_ALU64 | BPF_LSH | BPF_K: /* ALU64_IMM */
	case BPF_ALU64 | BPF_RSH | BPF_K: /* ALU64_IMM */
	case BPF_ALU64 | BPF_XOR | BPF_K: /* ALU64_IMM */
	case BPF_ALU64 | BPF_ARSH | BPF_K: /* ALU64_IMM */
	case BPF_ALU64 | BPF_MOV | BPF_K: /* ALU64_IMM */
	case BPF_ALU | BPF_MOV | BPF_K: /* ALU32_IMM */
	case BPF_ALU | BPF_ADD | BPF_K: /* ALU32_IMM */
	case BPF_ALU | BPF_SUB | BPF_K: /* ALU32_IMM */
	case BPF_ALU | BPF_OR | BPF_K: /* ALU64_IMM */
	case BPF_ALU | BPF_AND | BPF_K: /* ALU64_IMM */
	case BPF_ALU | BPF_LSH | BPF_K: /* ALU64_IMM */
	case BPF_ALU | BPF_RSH | BPF_K: /* ALU64_IMM */
	case BPF_ALU | BPF_XOR | BPF_K: /* ALU64_IMM */
	case BPF_ALU | BPF_ARSH | BPF_K: /* ALU64_IMM */
		r = gen_imm_insn(insn, ctx, this_idx);
		if (r < 0)
			return r;
		break;
	case BPF_ALU64 | BPF_MUL | BPF_K: /* ALU64_IMM */
		dst = ebpf_to_mips_reg(ctx, insn, dst_reg);
		if (dst < 0)
			return dst;
		if (get_reg_val_type(ctx, this_idx, insn->dst_reg) == REG_32BIT)
			emit_instr(ctx, dinsu, dst, MIPS_R_ZERO, 32, 32);
		if (insn->imm == 1) /* Mult by 1 is a nop */
			break;
		gen_imm_to_reg(insn, MIPS_R_AT, ctx);
		emit_instr(ctx, dmultu, MIPS_R_AT, dst);
		emit_instr(ctx, mflo, dst);
		break;
	case BPF_ALU64 | BPF_NEG | BPF_K: /* ALU64_IMM */
		dst = ebpf_to_mips_reg(ctx, insn, dst_reg);
		if (dst < 0)
			return dst;
		if (get_reg_val_type(ctx, this_idx, insn->dst_reg) == REG_32BIT)
			emit_instr(ctx, dinsu, dst, MIPS_R_ZERO, 32, 32);
		emit_instr(ctx, dsubu, dst, MIPS_R_ZERO, dst);
		break;
	case BPF_ALU | BPF_MUL | BPF_K: /* ALU_IMM */
		dst = ebpf_to_mips_reg(ctx, insn, dst_reg);
		if (dst < 0)
			return dst;
		td = get_reg_val_type(ctx, this_idx, insn->dst_reg);
		if (td == REG_64BIT || td == REG_32BIT_ZERO_EX) {
			/* sign extend */
			emit_instr(ctx, sll, dst, dst, 0);
		}
		if (insn->imm == 1) /* Mult by 1 is a nop */
			break;
		gen_imm_to_reg(insn, MIPS_R_AT, ctx);
		emit_instr(ctx, multu, dst, MIPS_R_AT);
		emit_instr(ctx, mflo, dst);
		break;
	case BPF_ALU | BPF_NEG | BPF_K: /* ALU_IMM */
		dst = ebpf_to_mips_reg(ctx, insn, dst_reg);
		if (dst < 0)
			return dst;
		td = get_reg_val_type(ctx, this_idx, insn->dst_reg);
		if (td == REG_64BIT || td == REG_32BIT_ZERO_EX) {
			/* sign extend */
			emit_instr(ctx, sll, dst, dst, 0);
		}
		emit_instr(ctx, subu, dst, MIPS_R_ZERO, dst);
		break;
	case BPF_ALU | BPF_DIV | BPF_K: /* ALU_IMM */
	case BPF_ALU | BPF_MOD | BPF_K: /* ALU_IMM */
		dst = ebpf_to_mips_reg(ctx, insn, dst_reg);
		if (dst < 0)
			return dst;
		if (insn->imm == 0) { /* Div by zero */
			b_off = b_imm(exit_idx, ctx);
			if (is_bad_offset(b_off))
				return -E2BIG;
			emit_instr(ctx, beq, MIPS_R_ZERO, MIPS_R_ZERO, b_off);
			emit_instr(ctx, addu, MIPS_R_V0, MIPS_R_ZERO, MIPS_R_ZERO);
		}
		td = get_reg_val_type(ctx, this_idx, insn->dst_reg);
		if (td == REG_64BIT || td == REG_32BIT_ZERO_EX)
			/* sign extend */
			emit_instr(ctx, sll, dst, dst, 0);
		if (insn->imm == 1) {
			/* div by 1 is a nop, mod by 1 is zero */
			if (BPF_OP(insn->code) == BPF_MOD)
				emit_instr(ctx, addu, dst, MIPS_R_ZERO, MIPS_R_ZERO);
			break;
		}
		gen_imm_to_reg(insn, MIPS_R_AT, ctx);
		emit_instr(ctx, divu, dst, MIPS_R_AT);
		if (BPF_OP(insn->code) == BPF_DIV)
			emit_instr(ctx, mflo, dst);
		else
			emit_instr(ctx, mfhi, dst);
		break;
	case BPF_ALU64 | BPF_DIV | BPF_K: /* ALU_IMM */
	case BPF_ALU64 | BPF_MOD | BPF_K: /* ALU_IMM */
		dst = ebpf_to_mips_reg(ctx, insn, dst_reg);
		if (dst < 0)
			return dst;
		if (insn->imm == 0) { /* Div by zero */
			b_off = b_imm(exit_idx, ctx);
			if (is_bad_offset(b_off))
				return -E2BIG;
			emit_instr(ctx, beq, MIPS_R_ZERO, MIPS_R_ZERO, b_off);
			emit_instr(ctx, addu, MIPS_R_V0, MIPS_R_ZERO, MIPS_R_ZERO);
		}
		if (get_reg_val_type(ctx, this_idx, insn->dst_reg) == REG_32BIT)
			emit_instr(ctx, dinsu, dst, MIPS_R_ZERO, 32, 32);

		if (insn->imm == 1) {
			/* div by 1 is a nop, mod by 1 is zero */
			if (BPF_OP(insn->code) == BPF_MOD)
				emit_instr(ctx, addu, dst, MIPS_R_ZERO, MIPS_R_ZERO);
			break;
		}
		gen_imm_to_reg(insn, MIPS_R_AT, ctx);
		emit_instr(ctx, ddivu, dst, MIPS_R_AT);
		if (BPF_OP(insn->code) == BPF_DIV)
			emit_instr(ctx, mflo, dst);
		else
			emit_instr(ctx, mfhi, dst);
		break;
	case BPF_ALU64 | BPF_MOV | BPF_X: /* ALU64_REG */
	case BPF_ALU64 | BPF_ADD | BPF_X: /* ALU64_REG */
	case BPF_ALU64 | BPF_SUB | BPF_X: /* ALU64_REG */
	case BPF_ALU64 | BPF_XOR | BPF_X: /* ALU64_REG */
	case BPF_ALU64 | BPF_OR | BPF_X: /* ALU64_REG */
	case BPF_ALU64 | BPF_AND | BPF_X: /* ALU64_REG */
	case BPF_ALU64 | BPF_MUL | BPF_X: /* ALU64_REG */
	case BPF_ALU64 | BPF_DIV | BPF_X: /* ALU64_REG */
	case BPF_ALU64 | BPF_MOD | BPF_X: /* ALU64_REG */
	case BPF_ALU64 | BPF_LSH | BPF_X: /* ALU64_REG */
	case BPF_ALU64 | BPF_RSH | BPF_X: /* ALU64_REG */
	case BPF_ALU64 | BPF_ARSH | BPF_X: /* ALU64_REG */
		src = ebpf_to_mips_reg(ctx, insn, src_reg);
		dst = ebpf_to_mips_reg(ctx, insn, dst_reg);
		if (src < 0 || dst < 0)
			return -EINVAL;
		if (get_reg_val_type(ctx, this_idx, insn->dst_reg) == REG_32BIT)
			emit_instr(ctx, dinsu, dst, MIPS_R_ZERO, 32, 32);
		did_move = false;
		if (insn->src_reg == BPF_REG_10) {
			if (BPF_OP(insn->code) == BPF_MOV) {
				emit_instr(ctx, daddiu, dst, MIPS_R_SP, MAX_BPF_STACK);
				did_move = true;
			} else {
				emit_instr(ctx, daddiu, MIPS_R_AT, MIPS_R_SP, MAX_BPF_STACK);
				src = MIPS_R_AT;
			}
		} else if (get_reg_val_type(ctx, this_idx, insn->src_reg) == REG_32BIT) {
			int tmp_reg = MIPS_R_AT;

			if (BPF_OP(insn->code) == BPF_MOV) {
				tmp_reg = dst;
				did_move = true;
			}
			emit_instr(ctx, daddu, tmp_reg, src, MIPS_R_ZERO);
			emit_instr(ctx, dinsu, tmp_reg, MIPS_R_ZERO, 32, 32);
			src = MIPS_R_AT;
		}
		switch (BPF_OP(insn->code)) {
		case BPF_MOV:
			if (!did_move)
				emit_instr(ctx, daddu, dst, src, MIPS_R_ZERO);
			break;
		case BPF_ADD:
			emit_instr(ctx, daddu, dst, dst, src);
			break;
		case BPF_SUB:
			emit_instr(ctx, dsubu, dst, dst, src);
			break;
		case BPF_XOR:
			emit_instr(ctx, xor, dst, dst, src);
			break;
		case BPF_OR:
			emit_instr(ctx, or, dst, dst, src);
			break;
		case BPF_AND:
			emit_instr(ctx, and, dst, dst, src);
			break;
		case BPF_MUL:
			emit_instr(ctx, dmultu, dst, src);
			emit_instr(ctx, mflo, dst);
			break;
		case BPF_DIV:
		case BPF_MOD:
			b_off = b_imm(exit_idx, ctx);
			if (is_bad_offset(b_off))
				return -E2BIG;
			emit_instr(ctx, beq, src, MIPS_R_ZERO, b_off);
			emit_instr(ctx, movz, MIPS_R_V0, MIPS_R_ZERO, src);
			emit_instr(ctx, ddivu, dst, src);
			if (BPF_OP(insn->code) == BPF_DIV)
				emit_instr(ctx, mflo, dst);
			else
				emit_instr(ctx, mfhi, dst);
			break;
		case BPF_LSH:
			emit_instr(ctx, dsllv, dst, dst, src);
			break;
		case BPF_RSH:
			emit_instr(ctx, dsrlv, dst, dst, src);
			break;
		case BPF_ARSH:
			emit_instr(ctx, dsrav, dst, dst, src);
			break;
		default:
			pr_err("ALU64_REG NOT HANDLED\n");
			return -EINVAL;
		}
		break;
	case BPF_ALU | BPF_MOV | BPF_X: /* ALU_REG */
	case BPF_ALU | BPF_ADD | BPF_X: /* ALU_REG */
	case BPF_ALU | BPF_SUB | BPF_X: /* ALU_REG */
	case BPF_ALU | BPF_XOR | BPF_X: /* ALU_REG */
	case BPF_ALU | BPF_OR | BPF_X: /* ALU_REG */
	case BPF_ALU | BPF_AND | BPF_X: /* ALU_REG */
	case BPF_ALU | BPF_MUL | BPF_X: /* ALU_REG */
	case BPF_ALU | BPF_DIV | BPF_X: /* ALU_REG */
	case BPF_ALU | BPF_MOD | BPF_X: /* ALU_REG */
	case BPF_ALU | BPF_LSH | BPF_X: /* ALU_REG */
	case BPF_ALU | BPF_RSH | BPF_X: /* ALU_REG */
		src = ebpf_to_mips_reg(ctx, insn, src_reg_no_fp);
		dst = ebpf_to_mips_reg(ctx, insn, dst_reg);
		if (src < 0 || dst < 0)
			return -EINVAL;
		td = get_reg_val_type(ctx, this_idx, insn->dst_reg);
		if (td == REG_64BIT || td == REG_32BIT_ZERO_EX) {
			/* sign extend */
			emit_instr(ctx, sll, dst, dst, 0);
		}
		did_move = false;
		ts = get_reg_val_type(ctx, this_idx, insn->src_reg);
		if (ts == REG_64BIT || ts == REG_32BIT_ZERO_EX) {
			int tmp_reg = MIPS_R_AT;

			if (BPF_OP(insn->code) == BPF_MOV) {
				tmp_reg = dst;
				did_move = true;
			}
			/* sign extend */
			emit_instr(ctx, sll, tmp_reg, src, 0);
			src = MIPS_R_AT;
		}
		switch (BPF_OP(insn->code)) {
		case BPF_MOV:
			if (!did_move)
				emit_instr(ctx, addu, dst, src, MIPS_R_ZERO);
			break;
		case BPF_ADD:
			emit_instr(ctx, addu, dst, dst, src);
			break;
		case BPF_SUB:
			emit_instr(ctx, subu, dst, dst, src);
			break;
		case BPF_XOR:
			emit_instr(ctx, xor, dst, dst, src);
			break;
		case BPF_OR:
			emit_instr(ctx, or, dst, dst, src);
			break;
		case BPF_AND:
			emit_instr(ctx, and, dst, dst, src);
			break;
		case BPF_MUL:
			emit_instr(ctx, mul, dst, dst, src);
			break;
		case BPF_DIV:
		case BPF_MOD:
			b_off = b_imm(exit_idx, ctx);
			if (is_bad_offset(b_off))
				return -E2BIG;
			emit_instr(ctx, beq, src, MIPS_R_ZERO, b_off);
			emit_instr(ctx, movz, MIPS_R_V0, MIPS_R_ZERO, src);
			emit_instr(ctx, divu, dst, src);
			if (BPF_OP(insn->code) == BPF_DIV)
				emit_instr(ctx, mflo, dst);
			else
				emit_instr(ctx, mfhi, dst);
			break;
		case BPF_LSH:
			emit_instr(ctx, sllv, dst, dst, src);
			break;
		case BPF_RSH:
			emit_instr(ctx, srlv, dst, dst, src);
			break;
		default:
			pr_err("ALU_REG NOT HANDLED\n");
			return -EINVAL;
		}
		break;
	case BPF_JMP | BPF_EXIT:
		if (this_idx + 1 < exit_idx) {
			b_off = b_imm(exit_idx, ctx);
			if (is_bad_offset(b_off))
				return -E2BIG;
			emit_instr(ctx, beq, MIPS_R_ZERO, MIPS_R_ZERO, b_off);
			emit_nop(ctx);
		}
		break;
	case BPF_JMP | BPF_JEQ | BPF_K: /* JMP_IMM */
	case BPF_JMP | BPF_JNE | BPF_K: /* JMP_IMM */
		cmp_eq = (BPF_OP(insn->code) == BPF_JEQ);
		dst = ebpf_to_mips_reg(ctx, insn, dst_reg_fp_ok);
		if (dst < 0)
			return dst;
		if (insn->imm == 0) {
			src = MIPS_R_ZERO;
		} else {
			gen_imm_to_reg(insn, MIPS_R_AT, ctx);
			src = MIPS_R_AT;
		}
		goto jeq_common;
	case BPF_JMP | BPF_JEQ | BPF_X: /* JMP_REG */
	case BPF_JMP | BPF_JNE | BPF_X:
	case BPF_JMP | BPF_JSGT | BPF_X:
	case BPF_JMP | BPF_JSGE | BPF_X:
	case BPF_JMP | BPF_JGT | BPF_X:
	case BPF_JMP | BPF_JGE | BPF_X:
	case BPF_JMP | BPF_JSET | BPF_X:
		src = ebpf_to_mips_reg(ctx, insn, src_reg_no_fp);
		dst = ebpf_to_mips_reg(ctx, insn, dst_reg);
		if (src < 0 || dst < 0)
			return -EINVAL;
		td = get_reg_val_type(ctx, this_idx, insn->dst_reg);
		ts = get_reg_val_type(ctx, this_idx, insn->src_reg);
		if (td == REG_32BIT && ts != REG_32BIT) {
			emit_instr(ctx, sll, MIPS_R_AT, src, 0);
			src = MIPS_R_AT;
		} else if (ts == REG_32BIT && td != REG_32BIT) {
			emit_instr(ctx, sll, MIPS_R_AT, dst, 0);
			dst = MIPS_R_AT;
		}
		if (BPF_OP(insn->code) == BPF_JSET) {
			emit_instr(ctx, and, MIPS_R_AT, dst, src);
			cmp_eq = false;
			dst = MIPS_R_AT;
			src = MIPS_R_ZERO;
		} else if (BPF_OP(insn->code) == BPF_JSGT) {
			emit_instr(ctx, dsubu, MIPS_R_AT, dst, src);
			if ((insn + 1)->code == (BPF_JMP | BPF_EXIT) && insn->off == 1) {
				b_off = b_imm(exit_idx, ctx);
				if (is_bad_offset(b_off))
					return -E2BIG;
				emit_instr(ctx, blez, MIPS_R_AT, b_off);
				emit_nop(ctx);
				return 2; /* We consumed the exit. */
			}
			b_off = b_imm(this_idx + insn->off + 1, ctx);
			if (is_bad_offset(b_off))
				return -E2BIG;
			emit_instr(ctx, bgtz, MIPS_R_AT, b_off);
			emit_nop(ctx);
			break;
		} else if (BPF_OP(insn->code) == BPF_JSGE) {
			emit_instr(ctx, slt, MIPS_R_AT, dst, src);
			cmp_eq = true;
			dst = MIPS_R_AT;
			src = MIPS_R_ZERO;
		} else if (BPF_OP(insn->code) == BPF_JGT) {
			/* dst or src could be AT */
			emit_instr(ctx, dsubu, MIPS_R_T8, dst, src);
			emit_instr(ctx, sltu, MIPS_R_AT, dst, src);
			/* SP known to be non-zero, movz becomes boolean not */
			emit_instr(ctx, movz, MIPS_R_T9, MIPS_R_SP, MIPS_R_T8);
			emit_instr(ctx, movn, MIPS_R_T9, MIPS_R_ZERO, MIPS_R_T8);
			emit_instr(ctx, or, MIPS_R_AT, MIPS_R_T9, MIPS_R_AT);
			cmp_eq = true;
			dst = MIPS_R_AT;
			src = MIPS_R_ZERO;
		} else if (BPF_OP(insn->code) == BPF_JGE) {
			emit_instr(ctx, sltu, MIPS_R_AT, dst, src);
			cmp_eq = true;
			dst = MIPS_R_AT;
			src = MIPS_R_ZERO;
		} else { /* JNE/JEQ case */
			cmp_eq = (BPF_OP(insn->code) == BPF_JEQ);
		}
jeq_common:
		/*
		 * If the next insn is EXIT and we are jumping arround
		 * only it, invert the sense of the compare and
		 * conditionally jump to the exit.  Poor man's branch
		 * chaining.
		 */
		if ((insn + 1)->code == (BPF_JMP | BPF_EXIT) && insn->off == 1) {
			b_off = b_imm(exit_idx, ctx);
			if (is_bad_offset(b_off))
				return -E2BIG;
			if (cmp_eq)
				emit_instr(ctx, bne, dst, src, b_off);
			else
				emit_instr(ctx, beq, dst, src, b_off);
			emit_nop(ctx);
			return 2; /* We consumed the exit. */
		}
		b_off = b_imm(this_idx + insn->off + 1, ctx);
		if (is_bad_offset(b_off))
			return -E2BIG;
		if (cmp_eq)
			emit_instr(ctx, beq, dst, src, b_off);
		else
			emit_instr(ctx, bne, dst, src, b_off);
		emit_nop(ctx);
		break;
	case BPF_JMP | BPF_JSGT | BPF_K: /* JMP_IMM */
	case BPF_JMP | BPF_JSGE | BPF_K: /* JMP_IMM */
		cmp_eq = (BPF_OP(insn->code) == BPF_JSGE);
		dst = ebpf_to_mips_reg(ctx, insn, dst_reg_fp_ok);
		if (dst < 0)
			return dst;

		if (insn->imm == 0) {
			if ((insn + 1)->code == (BPF_JMP | BPF_EXIT) && insn->off == 1) {
				b_off = b_imm(exit_idx, ctx);
				if (is_bad_offset(b_off))
					return -E2BIG;
				if (cmp_eq)
					emit_instr(ctx, bltz, dst, b_off);
				else
					emit_instr(ctx, blez, dst, b_off);
				emit_nop(ctx);
				return 2; /* We consumed the exit. */
			}
			b_off = b_imm(this_idx + insn->off + 1, ctx);
			if (is_bad_offset(b_off))
				return -E2BIG;
			if (cmp_eq)
				emit_instr(ctx, bgez, dst, b_off);
			else
				emit_instr(ctx, bgtz, dst, b_off);
			emit_nop(ctx);
			break;
		}
		/*
		 * only "LT" compare available, so we must use imm + 1
		 * to generate "GT"
		 */
		t64s = insn->imm + (cmp_eq ? 0 : 1);
		if (t64s >= S16_MIN && t64s <= S16_MAX) {
			emit_instr(ctx, slti, MIPS_R_AT, dst, (int)t64s);
			src = MIPS_R_AT;
			dst = MIPS_R_ZERO;
			cmp_eq = true;
			goto jeq_common;
		}
		emit_const_to_reg(ctx, MIPS_R_AT, (u64)t64s);
		emit_instr(ctx, slt, MIPS_R_AT, dst, MIPS_R_AT);
		src = MIPS_R_AT;
		dst = MIPS_R_ZERO;
		cmp_eq = true;
		goto jeq_common;

	case BPF_JMP | BPF_JGT | BPF_K:
	case BPF_JMP | BPF_JGE | BPF_K:
		cmp_eq = (BPF_OP(insn->code) == BPF_JGE);
		dst = ebpf_to_mips_reg(ctx, insn, dst_reg_fp_ok);
		if (dst < 0)
			return dst;
		/*
		 * only "LT" compare available, so we must use imm + 1
		 * to generate "GT"
		 */
		t64s = (u64)(u32)(insn->imm) + (cmp_eq ? 0 : 1);
		if (t64s >= 0 && t64s <= S16_MAX) {
			emit_instr(ctx, sltiu, MIPS_R_AT, dst, (int)t64s);
			src = MIPS_R_AT;
			dst = MIPS_R_ZERO;
			cmp_eq = true;
			goto jeq_common;
		}
		emit_const_to_reg(ctx, MIPS_R_AT, (u64)t64s);
		emit_instr(ctx, sltu, MIPS_R_AT, dst, MIPS_R_AT);
		src = MIPS_R_AT;
		dst = MIPS_R_ZERO;
		cmp_eq = true;
		goto jeq_common;

	case BPF_JMP | BPF_JSET | BPF_K: /* JMP_IMM */
		dst = ebpf_to_mips_reg(ctx, insn, dst_reg_fp_ok);
		if (dst < 0)
			return dst;

		if (use_bbit_insns() && hweight32((u32)insn->imm) == 1) {
			if ((insn + 1)->code == (BPF_JMP | BPF_EXIT) && insn->off == 1) {
				b_off = b_imm(exit_idx, ctx);
				if (is_bad_offset(b_off))
					return -E2BIG;
				emit_instr(ctx, bbit0, dst, ffs((u32)insn->imm) - 1, b_off);
				emit_nop(ctx);
				return 2; /* We consumed the exit. */
			}
			b_off = b_imm(this_idx + insn->off + 1, ctx);
			if (is_bad_offset(b_off))
				return -E2BIG;
			emit_instr(ctx, bbit1, dst, ffs((u32)insn->imm) - 1, b_off);
			emit_nop(ctx);
			break;
		}
		t64 = (u32)insn->imm;
		emit_const_to_reg(ctx, MIPS_R_AT, t64);
		emit_instr(ctx, and, MIPS_R_AT, dst, MIPS_R_AT);
		src = MIPS_R_AT;
		dst = MIPS_R_ZERO;
		cmp_eq = false;
		goto jeq_common;

	case BPF_JMP | BPF_JA:
		b_off = b_imm(this_idx + insn->off + 1, ctx);
		if (is_bad_offset(b_off))
			return -E2BIG;
		emit_instr(ctx, b, b_off);
		emit_nop(ctx);
		break;
	case BPF_LD | BPF_DW | BPF_IMM:
		if (insn->src_reg != 0)
			return -EINVAL;
		dst = ebpf_to_mips_reg(ctx, insn, dst_reg);
		if (dst < 0)
			return dst;
		t64 = ((u64)(u32)insn->imm) | ((u64)(insn + 1)->imm << 32);
		emit_const_to_reg(ctx, dst, t64);
		return 2; /* Double slot insn */

	case BPF_JMP | BPF_CALL:
		ctx->flags |= EBPF_SAVE_RA;
		t64s = (s64)insn->imm + (s64)__bpf_call_base;
		emit_const_to_reg(ctx, MIPS_R_T9, (u64)t64s);
		emit_jalr(MIPS_R_RA, MIPS_R_T9, ctx);
		/* delay slot */
		emit_instr(ctx, nop);
		break;

	case BPF_LD | BPF_B | BPF_ABS:
	case BPF_LD | BPF_H | BPF_ABS:
	case BPF_LD | BPF_W | BPF_ABS:
	case BPF_LD | BPF_DW | BPF_ABS:
		ctx->flags |= EBPF_SAVE_RA;

		gen_imm_to_reg(insn, MIPS_R_A1, ctx);
		emit_instr(ctx, addiu, MIPS_R_A2, MIPS_R_ZERO, size_to_len(insn));

		if (insn->imm < 0) {
			emit_const_to_reg(ctx, MIPS_R_T9, (u64)bpf_internal_load_pointer_neg_helper);
		} else {
			emit_const_to_reg(ctx, MIPS_R_T9, (u64)ool_skb_header_pointer);
			emit_instr(ctx, daddiu, MIPS_R_A3, MIPS_R_SP, ctx->tmp_offset);
		}
		goto ld_skb_common;

	case BPF_LD | BPF_B | BPF_IND:
	case BPF_LD | BPF_H | BPF_IND:
	case BPF_LD | BPF_W | BPF_IND:
	case BPF_LD | BPF_DW | BPF_IND:
		ctx->flags |= EBPF_SAVE_RA;
		src = ebpf_to_mips_reg(ctx, insn, src_reg_no_fp);
		if (src < 0)
			return src;
		ts = get_reg_val_type(ctx, this_idx, insn->src_reg);
		if (ts == REG_32BIT_ZERO_EX) {
			/* sign extend */
			emit_instr(ctx, sll, MIPS_R_A1, src, 0);
			src = MIPS_R_A1;
		}
		if (insn->imm >= S16_MIN && insn->imm <= S16_MAX) {
			emit_instr(ctx, daddiu, MIPS_R_A1, src, insn->imm);
		} else {
			gen_imm_to_reg(insn, MIPS_R_AT, ctx);
			emit_instr(ctx, daddu, MIPS_R_A1, MIPS_R_AT, src);
		}
		/* truncate to 32-bit int */
		emit_instr(ctx, sll, MIPS_R_A1, MIPS_R_A1, 0);
		emit_instr(ctx, daddiu, MIPS_R_A3, MIPS_R_SP, ctx->tmp_offset);
		emit_instr(ctx, slt, MIPS_R_AT, MIPS_R_A1, MIPS_R_ZERO);

		emit_const_to_reg(ctx, MIPS_R_T8, (u64)bpf_internal_load_pointer_neg_helper);
		emit_const_to_reg(ctx, MIPS_R_T9, (u64)ool_skb_header_pointer);
		emit_instr(ctx, addiu, MIPS_R_A2, MIPS_R_ZERO, size_to_len(insn));
		emit_instr(ctx, movn, MIPS_R_T9, MIPS_R_T8, MIPS_R_AT);

ld_skb_common:
		emit_jalr(MIPS_R_RA, MIPS_R_T9, ctx);
		/* delay slot */
		emit_reg_move(MIPS_R_A0, MIPS_R_S0, ctx);

		/* Check the error value */
		b_off = b_imm(exit_idx, ctx);
		if (is_bad_offset(b_off))
			return -E2BIG;
		emit_instr(ctx, beq, MIPS_R_V0, MIPS_R_ZERO, b_off);
		emit_nop(ctx);

#ifdef __BIG_ENDIAN
		need_swap = false;
#else
		need_swap = true;
#endif
		dst = MIPS_R_V0;
		switch (BPF_SIZE(insn->code)) {
		case BPF_B:
			emit_instr(ctx, lbu, dst, 0, MIPS_R_V0);
			break;
		case BPF_H:
			emit_instr(ctx, lhu, dst, 0, MIPS_R_V0);
			if (need_swap)
				emit_instr(ctx, wsbh, dst, dst);
			break;
		case BPF_W:
			emit_instr(ctx, lw, dst, 0, MIPS_R_V0);
			if (need_swap) {
				emit_instr(ctx, wsbh, dst, dst);
				emit_instr(ctx, rotr, dst, dst, 16);
			}
			break;
		case BPF_DW:
			emit_instr(ctx, ld, dst, 0, MIPS_R_V0);
			if (need_swap) {
				emit_instr(ctx, dsbh, dst, dst);
				emit_instr(ctx, dshd, dst, dst);
			}
			break;
		}

		break;
	case BPF_ALU | BPF_END | BPF_FROM_BE:
	case BPF_ALU | BPF_END | BPF_FROM_LE:
		dst = ebpf_to_mips_reg(ctx, insn, dst_reg);
		if (dst < 0)
			return dst;
		td = get_reg_val_type(ctx, this_idx, insn->dst_reg);
		if (insn->imm == 64 && td == REG_32BIT)
			emit_instr(ctx, dinsu, dst, MIPS_R_ZERO, 32, 32);

		if (insn->imm != 64 &&
		    (td == REG_64BIT || td == REG_32BIT_ZERO_EX)) {
			/* sign extend */
			emit_instr(ctx, sll, dst, dst, 0);
		}

#ifdef __BIG_ENDIAN
		need_swap = (BPF_SRC(insn->code) == BPF_FROM_LE);
#else
		need_swap = (BPF_SRC(insn->code) == BPF_FROM_BE);
#endif
		if (insn->imm == 16) {
			if (need_swap)
				emit_instr(ctx, wsbh, dst, dst);
			emit_instr(ctx, andi, dst, dst, 0xffff);
		} else if (insn->imm == 32) {
			if (need_swap) {
				emit_instr(ctx, wsbh, dst, dst);
				emit_instr(ctx, rotr, dst, dst, 16);
			}
		} else { /* 64-bit*/
			if (need_swap) {
				emit_instr(ctx, dsbh, dst, dst);
				emit_instr(ctx, dshd, dst, dst);
			}
		}
		break;

	case BPF_ST | BPF_B | BPF_MEM:
	case BPF_ST | BPF_H | BPF_MEM:
	case BPF_ST | BPF_W | BPF_MEM:
	case BPF_ST | BPF_DW | BPF_MEM:
		if (insn->dst_reg == BPF_REG_10) {
			ctx->flags |= EBPF_SEEN_FP;
			dst = MIPS_R_SP;
			mem_off = insn->off - MAX_BPF_STACK;
		} else {
			dst = ebpf_to_mips_reg(ctx, insn, dst_reg);
			if (dst < 0)
				return dst;
			mem_off = insn->off;
		}
		gen_imm_to_reg(insn, MIPS_R_AT, ctx);
		switch (BPF_SIZE(insn->code)) {
		case BPF_B:
			emit_instr(ctx, sb, MIPS_R_AT, mem_off, dst);
			break;
		case BPF_H:
			emit_instr(ctx, sh, MIPS_R_AT, mem_off, dst);
			break;
		case BPF_W:
			emit_instr(ctx, sw, MIPS_R_AT, mem_off, dst);
			break;
		case BPF_DW:
			emit_instr(ctx, sd, MIPS_R_AT, mem_off, dst);
			break;
		}
		break;

	case BPF_LDX | BPF_B | BPF_MEM:
	case BPF_LDX | BPF_H | BPF_MEM:
	case BPF_LDX | BPF_W | BPF_MEM:
	case BPF_LDX | BPF_DW | BPF_MEM:
		if (insn->src_reg == BPF_REG_10) {
			ctx->flags |= EBPF_SEEN_FP;
			src = MIPS_R_SP;
			mem_off = insn->off - MAX_BPF_STACK;
		} else {
			src = ebpf_to_mips_reg(ctx, insn, src_reg_no_fp);
			if (src < 0)
				return src;
			mem_off = insn->off;
		}
		dst = ebpf_to_mips_reg(ctx, insn, dst_reg);
		if (dst < 0)
			return dst;
		switch (BPF_SIZE(insn->code)) {
		case BPF_B:
			emit_instr(ctx, lbu, dst, mem_off, src);
			break;
		case BPF_H:
			emit_instr(ctx, lhu, dst, mem_off, src);
			break;
		case BPF_W:
			emit_instr(ctx, lw, dst, mem_off, src);
			break;
		case BPF_DW:
			emit_instr(ctx, ld, dst, mem_off, src);
			break;
		}
		break;

	case BPF_STX | BPF_B | BPF_MEM:
	case BPF_STX | BPF_H | BPF_MEM:
	case BPF_STX | BPF_W | BPF_MEM:
	case BPF_STX | BPF_DW | BPF_MEM:
	case BPF_STX | BPF_W | BPF_XADD:
	case BPF_STX | BPF_DW | BPF_XADD:
		if (insn->dst_reg == BPF_REG_10) {
			ctx->flags |= EBPF_SEEN_FP;
			dst = MIPS_R_SP;
			mem_off = insn->off - MAX_BPF_STACK;
		} else {
			dst = ebpf_to_mips_reg(ctx, insn, dst_reg);
			if (dst < 0)
				return dst;
			mem_off = insn->off;
		}
		src = ebpf_to_mips_reg(ctx, insn, src_reg_no_fp);
		if (src < 0)
			return dst;
		if (BPF_MODE(insn->code) == BPF_XADD) {
			switch (BPF_SIZE(insn->code)) {
			case BPF_W:
				if (get_reg_val_type(ctx, this_idx, insn->src_reg) == REG_32BIT) {
					emit_instr(ctx, sll, MIPS_R_AT, src, 0);
					src = MIPS_R_AT;
				}
				emit_instr(ctx, ll, MIPS_R_T8, mem_off, dst);
				emit_instr(ctx, addu, MIPS_R_T8, MIPS_R_T8, src);
				emit_instr(ctx, sc, MIPS_R_T8, mem_off, dst);
				/*
				 * On failure back up to LL (-4
				 * instructions of 4 bytes each
				 */
				emit_instr(ctx, beq, MIPS_R_T8, MIPS_R_ZERO, -4 * 4);
				emit_instr(ctx, nop);
				break;
			case BPF_DW:
				if (get_reg_val_type(ctx, this_idx, insn->src_reg) == REG_32BIT) {
					emit_instr(ctx, daddu, MIPS_R_AT, src, MIPS_R_ZERO);
					emit_instr(ctx, dinsu, MIPS_R_AT, MIPS_R_ZERO, 32, 32);
					src = MIPS_R_AT;
				}
				emit_instr(ctx, lld, MIPS_R_T8, mem_off, dst);
				emit_instr(ctx, daddu, MIPS_R_T8, MIPS_R_T8, src);
				emit_instr(ctx, scd, MIPS_R_T8, mem_off, dst);
				emit_instr(ctx, beq, MIPS_R_T8, MIPS_R_ZERO, -4 * 4);
				emit_instr(ctx, nop);
				break;
			}
		} else { /* BPF_MEM */
			switch (BPF_SIZE(insn->code)) {
			case BPF_B:
				emit_instr(ctx, sb, src, mem_off, dst);
				break;
			case BPF_H:
				emit_instr(ctx, sh, src, mem_off, dst);
				break;
			case BPF_W:
				emit_instr(ctx, sw, src, mem_off, dst);
				break;
			case BPF_DW:
				if (get_reg_val_type(ctx, this_idx, insn->src_reg) == REG_32BIT) {
					emit_instr(ctx, daddu, MIPS_R_AT, src, MIPS_R_ZERO);
					emit_instr(ctx, dinsu, MIPS_R_AT, MIPS_R_ZERO, 32, 32);
					src = MIPS_R_AT;
				}
				emit_instr(ctx, sd, src, mem_off, dst);
				break;
			}
		}
		break;

	default:
		pr_err("NOT HANDLED %d - (%02x)\n",
		       this_idx, (unsigned int)insn->code);
		return -EINVAL;
	}
	return 1;
}

#define RVT_VISITED_MASK 0xc000000000000000ull
#define RVT_FALL_THROUGH 0x4000000000000000ull
#define RVT_BRANCH_TAKEN 0x8000000000000000ull
#define RVT_DONE (RVT_FALL_THROUGH | RVT_BRANCH_TAKEN)

static int build_int_body(struct jit_ctx *ctx)
{
	const struct bpf_prog *prog = ctx->skf;
	const struct bpf_insn *insn;
	int i, r;

	for (i = 0; i < prog->len; ) {
		insn = prog->insnsi + i;
		if ((ctx->reg_val_types[i] & RVT_VISITED_MASK) == 0) {
			/* dead instruction, don't emit it. */
			i++;
			continue;
		}

		if (ctx->target == NULL)
			ctx->offsets[i] = ctx->idx * 4;

		r = build_one_insn(insn, ctx, i, prog->len);
		if (r < 0)
			return r;
		i += r;
	}
	/* epilogue offset */
	if (ctx->target == NULL)
		ctx->offsets[i] = ctx->idx * 4;

	/*
	 * All exits have an offset of the epilogue, some offsets may
	 * not have been set due to banch-around threading, so set
	 * them now.
	 */
	if (ctx->target == NULL)
		for (i = 0; i < prog->len; i++) {
			insn = prog->insnsi + i;
			if (insn->code == (BPF_JMP | BPF_EXIT))
				ctx->offsets[i] = ctx->idx * 4;
		}
	return 0;
}

/* return the last idx processed, or negative for error */
static int reg_val_propagate_range(struct jit_ctx *ctx, u64 initial_rvt,
				   int start_idx, bool follow_taken)
{
	const struct bpf_prog *prog = ctx->skf;
	const struct bpf_insn *insn;
	u64 exit_rvt = initial_rvt;
	u64 *rvt = ctx->reg_val_types;
	int idx;
	int reg;

	for (idx = start_idx; idx < prog->len; idx++) {
		rvt[idx] = (rvt[idx] & RVT_VISITED_MASK) | exit_rvt;
		insn = prog->insnsi + idx;
		switch (BPF_CLASS(insn->code)) {
		case BPF_ALU:
			switch (BPF_OP(insn->code)) {
			case BPF_ADD:
			case BPF_SUB:
			case BPF_MUL:
			case BPF_DIV:
			case BPF_OR:
			case BPF_AND:
			case BPF_LSH:
			case BPF_RSH:
			case BPF_NEG:
			case BPF_MOD:
			case BPF_XOR:
				set_reg_val_type(&exit_rvt, insn->dst_reg, REG_32BIT);
				break;
			case BPF_MOV:
				if (BPF_SRC(insn->code)) {
					set_reg_val_type(&exit_rvt, insn->dst_reg, REG_32BIT);
				} else {
					/* IMM to REG move*/
					if (insn->imm >= 0)
						set_reg_val_type(&exit_rvt, insn->dst_reg, REG_32BIT_POS);
					else
						set_reg_val_type(&exit_rvt, insn->dst_reg, REG_32BIT);
				}
				break;
			case BPF_END:
				if (insn->imm == 64)
					set_reg_val_type(&exit_rvt, insn->dst_reg, REG_64BIT);
				else if (insn->imm == 32)
					set_reg_val_type(&exit_rvt, insn->dst_reg, REG_32BIT);
				else /* insn->imm == 16 */
					set_reg_val_type(&exit_rvt, insn->dst_reg, REG_32BIT_POS);
				break;
			}
			rvt[idx] |= RVT_DONE;
			break;
		case BPF_ALU64:
			switch (BPF_OP(insn->code)) {
			case BPF_MOV:
				if (BPF_SRC(insn->code)) {
					/* REG to REG move*/
					set_reg_val_type(&exit_rvt, insn->dst_reg, REG_64BIT);
				} else {
					/* IMM to REG move*/
					if (insn->imm >= 0)
						set_reg_val_type(&exit_rvt, insn->dst_reg, REG_32BIT_POS);
					else
						set_reg_val_type(&exit_rvt, insn->dst_reg, REG_64BIT_32BIT);
				}
				break;
			default:
				set_reg_val_type(&exit_rvt, insn->dst_reg, REG_64BIT);
			}
			rvt[idx] |= RVT_DONE;
			break;
		case BPF_LD:
			switch (BPF_SIZE(insn->code)) {
			case BPF_DW:
				if (BPF_MODE(insn->code) == BPF_IMM) {
					s64 val;

					val = (s64)((u32)insn->imm | ((u64)(insn + 1)->imm << 32));
					if (val > 0 && val <= S32_MAX)
						set_reg_val_type(&exit_rvt, insn->dst_reg, REG_32BIT_POS);
					else if (val >= S32_MIN && val <= S32_MAX)
						set_reg_val_type(&exit_rvt, insn->dst_reg, REG_64BIT_32BIT);
					else
						set_reg_val_type(&exit_rvt, insn->dst_reg, REG_64BIT);
					rvt[idx] |= RVT_DONE;
					idx++;
				} else {
					set_reg_val_type(&exit_rvt, insn->dst_reg, REG_64BIT);
				}
				break;
			case BPF_B:
			case BPF_H:
				set_reg_val_type(&exit_rvt, insn->dst_reg, REG_32BIT_POS);
				break;
			case BPF_W:
				if (BPF_MODE(insn->code) == BPF_IMM)
					set_reg_val_type(&exit_rvt, insn->dst_reg,
							 insn->imm >= 0 ? REG_32BIT_POS : REG_32BIT);
				else
					set_reg_val_type(&exit_rvt, insn->dst_reg, REG_32BIT);
				break;
			}
			rvt[idx] |= RVT_DONE;
			break;
		case BPF_LDX:
			switch (BPF_SIZE(insn->code)) {
			case BPF_DW:
				set_reg_val_type(&exit_rvt, insn->dst_reg, REG_64BIT);
				break;
			case BPF_B:
			case BPF_H:
				set_reg_val_type(&exit_rvt, insn->dst_reg, REG_32BIT_POS);
				break;
			case BPF_W:
				set_reg_val_type(&exit_rvt, insn->dst_reg, REG_32BIT);
				break;
			}
			rvt[idx] |= RVT_DONE;
			break;
		case BPF_JMP:
			switch (BPF_OP(insn->code)) {
			case BPF_EXIT:
				rvt[idx] = RVT_DONE | exit_rvt;
				rvt[prog->len] = exit_rvt;
				return idx;
			case BPF_JA:
				rvt[idx] |= RVT_DONE;
				idx += insn->off;
				break;
			case BPF_JEQ:
			case BPF_JGT:
			case BPF_JGE:
			case BPF_JSET:
			case BPF_JNE:
			case BPF_JSGT:
			case BPF_JSGE:
				if (follow_taken) {
					rvt[idx] |= RVT_BRANCH_TAKEN;
					idx += insn->off;
					follow_taken = false;
				} else {
					rvt[idx] |= RVT_FALL_THROUGH;
				}
				break;
			case BPF_CALL:
				set_reg_val_type(&exit_rvt, BPF_REG_0, REG_64BIT);
				/* Upon call return, argument registers are clobbered. */
				for (reg = BPF_REG_0; reg <= BPF_REG_5; reg++)
					set_reg_val_type(&exit_rvt, reg, REG_64BIT);

				rvt[idx] |= RVT_DONE;
				break;
			default:
				WARN(1, "Unhandled BPF_JMP case.\n");
				rvt[idx] |= RVT_DONE;
				break;
			}
			break;
		default:
			rvt[idx] |= RVT_DONE;
			break;
		}
	}
	return idx;
}

/*
 * Track the value range (i.e. 32-bit vs. 64-bit) of each register at
 * each eBPF insn.  This allows unneeded sign and zero extension
 * operations to be omitted.
 *
 * Doesn't handle yet confluence of control paths with conflicting
 * ranges, but it is good enough for most sane code.
 */
static int reg_val_propagate(struct jit_ctx *ctx)
{
	const struct bpf_prog *prog = ctx->skf;
	u64 exit_rvt;
	int reg;
	int i;

	/*
	 * 11 registers * 3 bits/reg leaves top bits free for other
	 * uses.  Bit-62..63 used to see if we have visited an insn.
	 */
	exit_rvt = 0;

	/* Upon entry, argument registers are 64-bit. */
	for (reg = BPF_REG_1; reg <= BPF_REG_5; reg++)
		set_reg_val_type(&exit_rvt, reg, REG_64BIT);

	/*
	 * First follow all conditional branches on the fall-through
	 * edge of control flow..
	 */
	reg_val_propagate_range(ctx, exit_rvt, 0, false);
restart_search:
	/*
	 * Then repeatedly find the first conditional branch where
	 * both edges of control flow have not been taken, and follow
	 * the branch taken edge.  We will end up restarting the
	 * search once per conditional branch insn.
	 */
	for (i = 0; i < prog->len; i++) {
		u64 rvt = ctx->reg_val_types[i];

		if ((rvt & RVT_VISITED_MASK) == RVT_DONE ||
		    (rvt & RVT_VISITED_MASK) == 0)
			continue;
		if ((rvt & RVT_VISITED_MASK) == RVT_FALL_THROUGH) {
			reg_val_propagate_range(ctx, rvt & ~RVT_VISITED_MASK, i, true);
		} else { /* RVT_BRANCH_TAKEN */
			WARN(1, "Unexpected RVT_BRANCH_TAKEN case.\n");
			reg_val_propagate_range(ctx, rvt & ~RVT_VISITED_MASK, i, false);
		}
		goto restart_search;
	}
	/*
	 * Eventually all conditional branches have been followed on
	 * both branches and we are done.  Any insn that has not been
	 * visited at this point is dead.
	 */

	return 0;
}

struct bpf_prog *bpf_int_jit_compile(struct bpf_prog *prog)
{
	struct jit_ctx ctx;
	unsigned int alloc_size;

	/* Only 64-bit kernel supports eBPF */
	if (!IS_ENABLED(CONFIG_64BIT) || !bpf_jit_enable)
		return prog;

	memset(&ctx, 0, sizeof(ctx));

	ctx.offsets = kcalloc(prog->len + 1, sizeof(*ctx.offsets), GFP_KERNEL);
	if (ctx.offsets == NULL)
		goto out;

	ctx.reg_val_types = kcalloc(prog->len + 1, sizeof(*ctx.reg_val_types), GFP_KERNEL);
	if (ctx.reg_val_types == NULL)
		goto out;

	ctx.skf = prog;

	if (reg_val_propagate(&ctx))
		goto out;

	/* First pass discovers used resources */
	if (build_int_body(&ctx))
		goto out;

	/* Second pass generates offsets */
	ctx.idx = 0;
	if (gen_int_prologue(&ctx))
		goto out;
	if (build_int_body(&ctx))
		goto out;
	if (build_int_epilogue(&ctx))
		goto out;

	alloc_size = 4 * ctx.idx;

	ctx.target = module_alloc(alloc_size);
	if (ctx.target == NULL)
		goto out;

	/* Clean it */
	memset(ctx.target, 0, alloc_size);

	/* Third pass generates the code */
	ctx.idx = 0;
	if (gen_int_prologue(&ctx))
		goto out;
	if (build_int_body(&ctx))
		goto out;
	if (build_int_epilogue(&ctx))
		goto out;
	/* Update the icache */
	flush_icache_range((ptr)ctx.target, (ptr)(ctx.target + ctx.idx));

	if (bpf_jit_enable > 1)
		/* Dump JIT code */
		bpf_jit_dump(prog->len, alloc_size, 2, ctx.target);

	prog->bpf_func = (void *)ctx.target;
	prog->jited = 1;

out:
	kfree(ctx.offsets);
	kfree(ctx.reg_val_types);

	return prog;
}
