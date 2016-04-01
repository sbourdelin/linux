/*
 * bpf_jit_comp64.c: eBPF JIT compiler
 *
 * Copyright 2016 Naveen N. Rao <naveen.n.rao@linux.vnet.ibm.com>
 *		  IBM Corporation
 *
 * Based on the powerpc classic BPF compiler by Matt Evans
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; version 2
 * of the License.
 */
#include <linux/moduleloader.h>
#include <asm/cacheflush.h>
#include <linux/netdevice.h>
#include <linux/filter.h>
#include <linux/if_vlan.h>

#include "bpf_jit64.h"

int bpf_jit_enable __read_mostly;

#define TMP_REG_1 (MAX_BPF_REG + 0)
#define TMP_REG_2 (MAX_BPF_REG + 1)

/* BPF to ppc register mappings */
static const int b2p[] = {
	/* function return value */
	[BPF_REG_0] = 10,
	/* function arguments */
	[BPF_REG_1] = 3,
	[BPF_REG_2] = 4,
	[BPF_REG_3] = 5,
	[BPF_REG_4] = 6,
	[BPF_REG_5] = 7,
	/* non volatile registers */
	[BPF_REG_6] = 30,
	[BPF_REG_7] = 29,
	[BPF_REG_8] = 28,
	[BPF_REG_9] = 26,
	/* frame pointer aka BPF_REG_10 */
	[BPF_REG_FP] = 31,
	/* eBPF jit internal registers */
	[TMP_REG_1] = 8,
	[TMP_REG_2] = 9,
};

static inline bool bpf_is_seen_register(struct codegen_context *ctx, int i)
{
	return (ctx->seen & (1 << (31 - b2p[i])));
}

static void bpf_jit_build_prologue(struct bpf_prog *fp, u32 *image,
				   struct codegen_context *ctx)
{
	int i;
	int new_stack_frame = 0;

	/*
	 * We only need a stack frame if:
	 * - we call other functions (kernel helpers), or
	 * - the bpf program uses its stack area
	 * The latter condition is deduced from the usage of BPF_REG_FP
	 */
	if (bpf_is_seen_register(ctx, BPF_REG_FP) || ctx->seen & SEEN_FUNC) {
		new_stack_frame = 1;

		/*
		 * We need a stack frame, but we don't necessarily need to
		 * save/restore LR unless we call other functions
		 */
		if (ctx->seen & SEEN_FUNC) {
			EMIT(PPC_INST_MFLR | __PPC_RT(R0));
			PPC_BPF_STL(0, 1, PPC_LR_STKOFF);
		}

		PPC_BPF_STLU(1, 1, -BPF_PPC_STACKFRAME);
	}

	/*
	 * Back up non-volatile regs -- BPF registers 6-10
	 * If we haven't created our own stack frame, we save these
	 * in the protected zone below the previous stack frame
	 */
	for (i = BPF_REG_6; i <= BPF_REG_10; i++)
		if (bpf_is_seen_register(ctx, i))
			PPC_BPF_STL(b2p[i], 1,
				(new_stack_frame ? BPF_PPC_STACKFRAME : 0) -
					(8 * (32 - b2p[i])));

	/* Setup frame pointer to point to the bpf stack area */
	if (bpf_is_seen_register(ctx, BPF_REG_FP))
		PPC_ADDI(b2p[BPF_REG_FP], 1,
				BPF_PPC_STACKFRAME - BPF_PPC_STACK_SAVE);
}

static void bpf_jit_build_epilogue(u32 *image, struct codegen_context *ctx)
{
	int i;
	int new_stack_frame = 0;

	/* Move result to r3 */
	PPC_ADDI(3, b2p[BPF_REG_0], 0);

	/* Did we create our own stack frame? */
	if (bpf_is_seen_register(ctx, BPF_REG_FP) || ctx->seen & SEEN_FUNC)
		new_stack_frame = 1;

	/* Restore NVRs */
	for (i = BPF_REG_6; i <= BPF_REG_10; i++)
		if (bpf_is_seen_register(ctx, i))
			PPC_BPF_LL(b2p[i], 1,
				(new_stack_frame ? BPF_PPC_STACKFRAME : 0) -
					(8 * (32 - b2p[i])));

	/* Tear down our stack frame */
	if (new_stack_frame) {
		PPC_ADDI(1, 1, BPF_PPC_STACKFRAME);
		if (ctx->seen & SEEN_FUNC) {
			PPC_BPF_LL(0, 1, PPC_LR_STKOFF);
			PPC_MTLR(0);
		}
	}

	PPC_BLR();
}

/* Assemble the body code between the prologue & epilogue */
static int bpf_jit_build_body(struct bpf_prog *fp, u32 *image,
			      struct codegen_context *ctx,
			      u32 *addrs)
{
	const struct bpf_insn *insn = fp->insnsi;
	int flen = fp->len;
	int i;

	/* Start of epilogue code - will only be valid 2nd pass onwards */
	u32 exit_addr = addrs[flen];

	for (i = 0; i < flen; i++) {
		u32 code = insn[i].code;
		u32 dst_reg = b2p[insn[i].dst_reg];
		u32 src_reg = b2p[insn[i].src_reg];
		s16 off = insn[i].off;
		s32 imm = insn[i].imm;
		u64 imm64;
		u8 *func;
		u32 true_cond;
		int stack_local_off;

		/*
		 * addrs[] maps a BPF bytecode address into a real offset from
		 * the start of the body code.
		 */
		addrs[i] = ctx->idx * 4;

		/*
		 * As an optimization, we note down which non-volatile registers
		 * are used so that we can only save/restore those in our
		 * prologue and epilogue. We do this here regardless of whether
		 * the actual BPF instruction uses src/dst registers or not
		 * (for instance, BPF_CALL does not use them). The expectation
		 * is that those instructions will have src_reg/dst_reg set to
		 * 0. Even otherwise, we just lose some prologue/epilogue
		 * optimization but everything else should work without
		 * any issues.
		 */
		if (dst_reg >= 26 && dst_reg <= 31)
			ctx->seen |= (1 << (31 - dst_reg));
		if (src_reg >= 26 && src_reg <= 31)
			ctx->seen |= (1 << (31 - src_reg));

		switch (code) {
		/*
		 * Arithmetic operations: ADD/SUB/MUL/DIV/MOD/NEG
		 */
		case BPF_ALU | BPF_ADD | BPF_X: /* (u32) dst += (u32) src */
		case BPF_ALU64 | BPF_ADD | BPF_X: /* dst += src */
			PPC_ADD(dst_reg, dst_reg, src_reg);
			PPC_CLEAR32();
			break;
		case BPF_ALU | BPF_SUB | BPF_K: /* (u32) dst -= (u32) imm */
		case BPF_ALU64 | BPF_SUB | BPF_K: /* dst -= imm */
			imm = -imm;
			/* fall through */
		case BPF_ALU | BPF_ADD | BPF_K: /* (u32) dst += (u32) imm */
		case BPF_ALU64 | BPF_ADD | BPF_K: /* dst += imm */
			if (!imm)
				break;
			if (imm >= -32768 && imm < 32768)
				PPC_ADDI(dst_reg, dst_reg, IMM_L(imm));
			else {
				PPC_LI32(b2p[TMP_REG_1], imm);
				PPC_ADD(dst_reg, dst_reg, b2p[TMP_REG_1]);
			}
			PPC_CLEAR32();
			break;
		case BPF_ALU | BPF_SUB | BPF_X: /* (u32) dst -= (u32) src */
		case BPF_ALU64 | BPF_SUB | BPF_X: /* dst -= src */
			PPC_SUB(dst_reg, dst_reg, src_reg);
			PPC_CLEAR32();
			break;
		case BPF_ALU | BPF_MUL | BPF_X: /* (u32) dst *= (u32) src */
			PPC_MULW(dst_reg, dst_reg, src_reg);
			PPC_CLEAR32();
			break;
		case BPF_ALU64 | BPF_MUL | BPF_X: /* dst *= src */
			PPC_MULD(dst_reg, dst_reg, src_reg);
			break;
		case BPF_ALU | BPF_MUL | BPF_K: /* (u32) dst *= (u32) imm */
		case BPF_ALU64 | BPF_MUL | BPF_K: /* dst *= imm */
			if (imm >= -32768 && imm < 32768)
				PPC_MULI(dst_reg, dst_reg, IMM_L(imm));
			else {
				PPC_LI32(b2p[TMP_REG_1], imm);
				if (BPF_CLASS(code) == BPF_ALU)
					PPC_MULW(dst_reg, dst_reg,
							b2p[TMP_REG_1]);
				else
					PPC_MULD(dst_reg, dst_reg,
							b2p[TMP_REG_1]);
			}
			PPC_CLEAR32();
			break;
		case BPF_ALU | BPF_DIV | BPF_X: /* (u32) dst /= (u32) src */
		case BPF_ALU | BPF_MOD | BPF_X: /* (u32) dst %= (u32) src */
			PPC_CMPWI(src_reg, 0);
			PPC_BCC_SHORT(COND_NE, (ctx->idx * 4) + 12);
			PPC_LI(b2p[BPF_REG_0], 0);
			PPC_JMP(exit_addr);
			if (BPF_OP(code) == BPF_MOD) {
				PPC_DIVWU(b2p[TMP_REG_1], dst_reg, src_reg);
				PPC_MULW(b2p[TMP_REG_1], src_reg,
						b2p[TMP_REG_1]);
				PPC_SUB(dst_reg, dst_reg, b2p[TMP_REG_1]);
			} else
				PPC_DIVWU(dst_reg, dst_reg, src_reg);
			PPC_CLEAR32();
			break;
		case BPF_ALU64 | BPF_DIV | BPF_X: /* dst /= src */
		case BPF_ALU64 | BPF_MOD | BPF_X: /* dst %= src */
			PPC_CMPDI(src_reg, 0);
			PPC_BCC_SHORT(COND_NE, (ctx->idx * 4) + 12);
			PPC_LI(b2p[BPF_REG_0], 0);
			PPC_JMP(exit_addr);
			if (BPF_OP(code) == BPF_MOD) {
				PPC_DIVD(b2p[TMP_REG_1], dst_reg, src_reg);
				PPC_MULD(b2p[TMP_REG_1], src_reg,
						b2p[TMP_REG_1]);
				PPC_SUB(dst_reg, dst_reg, b2p[TMP_REG_1]);
			} else
				PPC_DIVD(dst_reg, dst_reg, src_reg);
			break;
		case BPF_ALU | BPF_MOD | BPF_K: /* (u32) dst %= (u32) imm */
		case BPF_ALU | BPF_DIV | BPF_K: /* (u32) dst /= (u32) imm */
		case BPF_ALU64 | BPF_MOD | BPF_K: /* dst %= imm */
		case BPF_ALU64 | BPF_DIV | BPF_K: /* dst /= imm */
			if (imm == 0)
				return -EINVAL;
			else if (imm == 1)
				break;
			PPC_LI32(b2p[TMP_REG_1], imm);
			switch (BPF_CLASS(code)) {
			case BPF_ALU:
				if (BPF_OP(code) == BPF_MOD) {
					PPC_DIVWU(b2p[TMP_REG_2], dst_reg,
							b2p[TMP_REG_1]);
					PPC_MULW(b2p[TMP_REG_1],
							b2p[TMP_REG_1],
							b2p[TMP_REG_2]);
					PPC_SUB(dst_reg, dst_reg,
							b2p[TMP_REG_1]);
				} else
					PPC_DIVWU(dst_reg, dst_reg,
							b2p[TMP_REG_1]);
				PPC_CLEAR32();
				break;
			case BPF_ALU64:
				if (BPF_OP(code) == BPF_MOD) {
					PPC_DIVD(b2p[TMP_REG_2], dst_reg,
							b2p[TMP_REG_1]);
					PPC_MULD(b2p[TMP_REG_1],
							b2p[TMP_REG_1],
							b2p[TMP_REG_2]);
					PPC_SUB(dst_reg, dst_reg,
							b2p[TMP_REG_1]);
				} else
					PPC_DIVD(dst_reg, dst_reg,
							b2p[TMP_REG_1]);
			}
			break;
		case BPF_ALU | BPF_NEG: /* (u32) dst = -dst */
		case BPF_ALU64 | BPF_NEG: /* dst = -dst */
			PPC_NEG(dst_reg, dst_reg);
			PPC_CLEAR32();
			break;

		/*
		 * Logical operations: AND/OR/XOR/[A]LSH/[A]RSH
		 */
		case BPF_ALU | BPF_AND | BPF_X: /* (u32) dst = dst & src */
		case BPF_ALU64 | BPF_AND | BPF_X: /* dst = dst & src */
			PPC_AND(dst_reg, dst_reg, src_reg);
			PPC_CLEAR32();
			break;
		case BPF_ALU | BPF_AND | BPF_K: /* (u32) dst = dst & imm */
		case BPF_ALU64 | BPF_AND | BPF_K: /* dst = dst & imm */
			if (!IMM_H(imm))
				PPC_ANDI(dst_reg, dst_reg, IMM_L(imm));
			else {
				/* Sign-extended */
				PPC_LI32(b2p[TMP_REG_1], imm);
				PPC_AND(dst_reg, dst_reg, b2p[TMP_REG_1]);
			}
			PPC_CLEAR32();
			break;
		case BPF_ALU | BPF_OR | BPF_X: /* dst = (u32) dst | (u32) src */
		case BPF_ALU64 | BPF_OR | BPF_X: /* dst = dst | src */
			PPC_OR(dst_reg, dst_reg, src_reg);
			PPC_CLEAR32();
			break;
		case BPF_ALU | BPF_OR | BPF_K:/* dst = (u32) dst | (u32) imm */
		case BPF_ALU64 | BPF_OR | BPF_K:/* dst = dst | imm */
			if (imm < 0 && BPF_CLASS(code) == BPF_ALU64) {
				/* Sign-extended */
				PPC_LI32(b2p[TMP_REG_1], imm);
				PPC_OR(dst_reg, dst_reg, b2p[TMP_REG_1]);
			} else {
				if (IMM_L(imm))
					PPC_ORI(dst_reg, dst_reg, IMM_L(imm));
				if (IMM_H(imm))
					PPC_ORIS(dst_reg, dst_reg, IMM_H(imm));
			}
			PPC_CLEAR32();
			break;
		case BPF_ALU | BPF_XOR | BPF_X: /* (u32) dst ^= src */
		case BPF_ALU64 | BPF_XOR | BPF_X: /* dst ^= src */
			PPC_XOR(dst_reg, dst_reg, src_reg);
			PPC_CLEAR32();
			break;
		case BPF_ALU | BPF_XOR | BPF_K: /* (u32) dst ^= (u32) imm */
		case BPF_ALU64 | BPF_XOR | BPF_K: /* dst ^= imm */
			if (imm < 0 && BPF_CLASS(code) == BPF_ALU64) {
				/* Sign-extended */
				PPC_LI32(b2p[TMP_REG_1], imm);
				PPC_XOR(dst_reg, dst_reg, b2p[TMP_REG_1]);
			} else {
				if (IMM_L(imm))
					PPC_XORI(dst_reg, dst_reg, IMM_L(imm));
				if (IMM_H(imm))
					PPC_XORIS(dst_reg, dst_reg, IMM_H(imm));
			}
			PPC_CLEAR32();
			break;
		case BPF_ALU | BPF_LSH | BPF_X: /* (u32) dst <<= (u32) src */
			/* slw clears top 32 bits */
			PPC_SLW(dst_reg, dst_reg, src_reg);
			break;
		case BPF_ALU64 | BPF_LSH | BPF_X: /* dst <<= src; */
			PPC_SLD(dst_reg, dst_reg, src_reg);
			break;
		case BPF_ALU | BPF_LSH | BPF_K: /* (u32) dst <<== (u32) imm */
			/* with imm 0, we still need to clear top 32 bits */
			PPC_SLWI(dst_reg, dst_reg, imm);
			break;
		case BPF_ALU64 | BPF_LSH | BPF_K: /* dst <<== imm */
			if (imm != 0)
				PPC_SLDI(dst_reg, dst_reg, imm);
			break;
		case BPF_ALU | BPF_RSH | BPF_X: /* (u32) dst >>= (u32) src */
			PPC_SRW(dst_reg, dst_reg, src_reg);
			break;
		case BPF_ALU64 | BPF_RSH | BPF_X: /* dst >>= src */
			PPC_SRD(dst_reg, dst_reg, src_reg);
			break;
		case BPF_ALU | BPF_RSH | BPF_K: /* (u32) dst >>= (u32) imm */
			PPC_SRWI(dst_reg, dst_reg, imm);
			break;
		case BPF_ALU64 | BPF_RSH | BPF_K: /* dst >>= imm */
			if (imm != 0)
				PPC_SRDI(dst_reg, dst_reg, imm);
			break;
		case BPF_ALU64 | BPF_ARSH | BPF_X: /* (s64) dst >>= src */
			PPC_SRAD(dst_reg, dst_reg, src_reg);
			break;
		case BPF_ALU64 | BPF_ARSH | BPF_K: /* (s64) dst >>= imm */
			if (imm != 0)
				PPC_SRADI(dst_reg, dst_reg, imm);
			break;

		/*
		 * MOV
		 */
		case BPF_ALU | BPF_MOV | BPF_X: /* (u32) dst = src */
		case BPF_ALU64 | BPF_MOV | BPF_X: /* dst = src */
			PPC_ADDI(dst_reg, src_reg, 0);
			PPC_CLEAR32();
			break;
		case BPF_ALU | BPF_MOV | BPF_K: /* (u32) dst = imm */
			PPC_LI32U(dst_reg, imm);
			break;
		case BPF_ALU64 | BPF_MOV | BPF_K: /* dst = (s64) imm */
			PPC_LI32(dst_reg, imm);
			break;

		/*
		 * BPF_FROM_BE/LE
		 */
		case BPF_ALU | BPF_END | BPF_FROM_LE:
		case BPF_ALU | BPF_END | BPF_FROM_BE:
#ifdef __BIG_ENDIAN__
			if (BPF_SRC(code) == BPF_FROM_BE)
				goto emit_clear;
#else /* !__BIG_ENDIAN__ */
			if (BPF_SRC(code) == BPF_FROM_LE)
				goto emit_clear;
#endif
			switch (imm) {
			case 16:
				/* Rotate 8 bits left & mask with 0x0000ff00 */
				PPC_RLWINM(b2p[TMP_REG_1], dst_reg, 8, 16, 23);
				/* Rotate 8 bits right & insert LSB to reg */
				PPC_RLWIMI(b2p[TMP_REG_1], dst_reg, 24, 24, 31);
				/* Move result back to dst_reg */
				PPC_ADDI(dst_reg, b2p[TMP_REG_1], 0);
				break;
			case 32:
				/*
				 * Rotate word left by 8 bits:
				 * 2 bytes are already in their final position
				 * -- byte 2 and 4 (of bytes 1, 2, 3 and 4)
				 */
				PPC_RLWINM(b2p[TMP_REG_1], dst_reg, 8, 0, 31);
				/* Rotate 24 bits and insert byte 1 */
				PPC_RLWIMI(b2p[TMP_REG_1], dst_reg, 24, 0, 7);
				/* Rotate 24 bits and insert byte 3 */
				PPC_RLWIMI(b2p[TMP_REG_1], dst_reg, 24, 16, 23);
				PPC_ADDI(dst_reg, b2p[TMP_REG_1], 0);
				break;
			case 64:
				/*
				 * Way easier and faster to store the value
				 * into stack and then use ldbrx
				 *
				 * First, determine where in stack we can store
				 * this:
				 * - if we have allotted a stack frame, then we
				 *   will utilize the area set aside by
				 *   BPF_PPC_STACK_LOCALS
				 * - else, we use the area beneath the NV GPR
				 *   save area
				 *
				 * ctx->seen will be reliable in pass2, but
				 * the instructions generated will remain the
				 * same across all passes
				 */
				if (bpf_is_seen_register(ctx, BPF_REG_FP) ||
							ctx->seen & SEEN_FUNC)
					stack_local_off = STACK_FRAME_MIN_SIZE;
				else
					stack_local_off = -(BPF_PPC_STACK_SAVE +
								8);

				PPC_STD(dst_reg, 1, stack_local_off);
				PPC_ADDI(b2p[TMP_REG_1], 1, stack_local_off);
				PPC_LDBRX(dst_reg, 0, b2p[TMP_REG_1]);
				break;
			}
			break;
emit_clear:
			switch (imm) {
			case 16:
				/* zero-extend 16 bits into 64 bits */
				PPC_RLDICL(dst_reg, dst_reg, 0, 48);
				break;
			case 32:
				/* zero-extend 32 bits into 64 bits */
				PPC_RLDICL(dst_reg, dst_reg, 0, 32);
				break;
			case 64:
				/* nop */
				break;
			}
			break;

		/*
		 * BPF_ST(X)
		 */
		case BPF_STX | BPF_MEM | BPF_B: /* *(u8 *)(dst + off) = src */
		case BPF_ST | BPF_MEM | BPF_B: /* *(u8 *)(dst + off) = imm */
			if (BPF_CLASS(code) == BPF_ST) {
				PPC_LI(b2p[TMP_REG_1], imm);
				src_reg = b2p[TMP_REG_1];
			}
			PPC_STB(src_reg, dst_reg, off);
			break;
		case BPF_STX | BPF_MEM | BPF_H: /* (u16 *)(dst + off) = src */
		case BPF_ST | BPF_MEM | BPF_H: /* (u16 *)(dst + off) = imm */
			if (BPF_CLASS(code) == BPF_ST) {
				PPC_LI(b2p[TMP_REG_1], imm);
				src_reg = b2p[TMP_REG_1];
			}
			PPC_STH(src_reg, dst_reg, off);
			break;
		case BPF_STX | BPF_MEM | BPF_W: /* *(u32 *)(dst + off) = src */
		case BPF_ST | BPF_MEM | BPF_W: /* *(u32 *)(dst + off) = imm */
			if (BPF_CLASS(code) == BPF_ST) {
				PPC_LI32(b2p[TMP_REG_1], imm);
				src_reg = b2p[TMP_REG_1];
			}
			PPC_STW(src_reg, dst_reg, off);
			break;
		case BPF_STX | BPF_MEM | BPF_DW: /* (u64 *)(dst + off) = src */
		case BPF_ST | BPF_MEM | BPF_DW: /* *(u64 *)(dst + off) = imm */
			if (BPF_CLASS(code) == BPF_ST) {
				PPC_LI32(b2p[TMP_REG_1], imm);
				src_reg = b2p[TMP_REG_1];
			}
			PPC_STD(src_reg, dst_reg, off);
			break;

		/*
		 * BPF_STX XADD (atomic_add)
		 */
		/* *(u32 *)(dst + off) += src */
		case BPF_STX | BPF_XADD | BPF_W:
			/* Get EA into TMP_REG_1 */
			PPC_ADDI(b2p[TMP_REG_1], dst_reg, off);
			/* error if EA is not word-aligned */
			PPC_ANDI(b2p[TMP_REG_2], b2p[TMP_REG_1], 0x03);
			PPC_BCC_SHORT(COND_EQ, (ctx->idx * 4) + 12);
			PPC_LI(b2p[BPF_REG_0], 0);
			PPC_JMP(exit_addr);
			/* load value from memory into TMP_REG_2 */
			PPC_LWARX(b2p[TMP_REG_2], 0, b2p[TMP_REG_1], 0);
			/* add value from src_reg into this */
			PPC_ADD(b2p[TMP_REG_2], b2p[TMP_REG_2], src_reg);
			/* store result back */
			PPC_STWCX(b2p[TMP_REG_2], 0, b2p[TMP_REG_1]);
			break;
		/* *(u64 *)(dst + off) += src */
		case BPF_STX | BPF_XADD | BPF_DW:
			PPC_ADDI(b2p[TMP_REG_1], dst_reg, off);
			/* error if EA is not doubleword-aligned */
			PPC_ANDI(b2p[TMP_REG_2], b2p[TMP_REG_1], 0x07);
			PPC_BCC_SHORT(COND_EQ, (ctx->idx * 4) + 12);
			PPC_LI(b2p[BPF_REG_0], 0);
			PPC_JMP(exit_addr);
			PPC_LDARX(b2p[TMP_REG_2], 0, b2p[TMP_REG_1], 0);
			PPC_ADD(b2p[TMP_REG_2], b2p[TMP_REG_2], src_reg);
			PPC_STDCX(b2p[TMP_REG_2], 0, b2p[TMP_REG_1]);
			break;

		/*
		 * BPF_LDX
		 */
		/* dst = *(u8 *)(ul) (src + off) */
		case BPF_LDX | BPF_MEM | BPF_B:
			PPC_LBZ(dst_reg, src_reg, off);
			break;
		/* dst = *(u16 *)(ul) (src + off) */
		case BPF_LDX | BPF_MEM | BPF_H:
			PPC_LHZ(dst_reg, src_reg, off);
			break;
		/* dst = *(u32 *)(ul) (src + off) */
		case BPF_LDX | BPF_MEM | BPF_W:
			PPC_LWZ(dst_reg, src_reg, off);
			break;
		/* dst = *(u64 *)(ul) (src + off) */
		case BPF_LDX | BPF_MEM | BPF_DW:
			PPC_LD(dst_reg, src_reg, off);
			break;

		/*
		 * Doubleword load
		 * 16 byte instruction that uses two 'struct bpf_insn'
		 */
		case BPF_LD | BPF_IMM | BPF_DW: /* dst = (u64) imm */
			imm64 = ((u64)(u32) insn[i].imm) |
				    (((u64)(u32) insn[i+1].imm) << 32);
			/* Adjust for two bpf instructions */
			addrs[++i] = ctx->idx * 4;
			PPC_LI64(dst_reg, imm64);
			break;

		/*
		 * Return/Exit
		 */
		case BPF_JMP | BPF_EXIT:
			/*
			 * If this isn't the very last instruction, branch to
			 * the epilogue. If we _are_ the last instruction,
			 * we'll just fall through to the epilogue.
			 */
			if (i != flen - 1)
				PPC_JMP(exit_addr);
			/* else fall through to the epilogue */
			break;

		/*
		 * Call kernel helper
		 */
		case BPF_JMP | BPF_CALL:
			ctx->seen |= SEEN_FUNC;
			func = (u8 *) __bpf_call_base + imm;
			if (bpf_helper_changes_skb_data(func))
				return -ENOTSUPP; /* TODO */
#if !defined(_CALL_ELF) || _CALL_ELF != 2
			/* func points to the function descriptor */
			PPC_LI64(b2p[TMP_REG_2], (u64)func);
			/* Load actual entry point from function descriptor */
			PPC_BPF_LL(b2p[TMP_REG_1], b2p[TMP_REG_2], 0);
			/* Load TOC from function descriptor at offset 8*/
			PPC_BPF_LL(2, b2p[TMP_REG_2], 8);
			/* Load function entry point to LR */
			PPC_MTLR(b2p[TMP_REG_1]);
#elif defined(_CALL_ELF) && _CALL_ELF == 2
			/* we can clobber r12 */
			PPC_FUNC_ADDR(12, func);
			PPC_MTLR(12);
#endif
			PPC_BLRL();
			/* move return value from r3 to BPF_REG_0 */
			PPC_ADDI(b2p[BPF_REG_0], 3, 0);
			break;

		/*
		 * Jumps and branches
		 */
		case BPF_JMP | BPF_JA:
			PPC_JMP(addrs[i + 1 + off]);
			break;

		case BPF_JMP | BPF_JGT | BPF_K:
		case BPF_JMP | BPF_JGT | BPF_X:
		case BPF_JMP | BPF_JSGT | BPF_K:
		case BPF_JMP | BPF_JSGT | BPF_X:
			true_cond = COND_GT;
			goto cond_branch;
		case BPF_JMP | BPF_JGE | BPF_K:
		case BPF_JMP | BPF_JGE | BPF_X:
		case BPF_JMP | BPF_JSGE | BPF_K:
		case BPF_JMP | BPF_JSGE | BPF_X:
			true_cond = COND_GE;
			goto cond_branch;
		case BPF_JMP | BPF_JEQ | BPF_K:
		case BPF_JMP | BPF_JEQ | BPF_X:
			true_cond = COND_EQ;
			goto cond_branch;
		case BPF_JMP | BPF_JNE | BPF_K:
		case BPF_JMP | BPF_JNE | BPF_X:
			true_cond = COND_NE;
			goto cond_branch;
		case BPF_JMP | BPF_JSET | BPF_K:
		case BPF_JMP | BPF_JSET | BPF_X:
			true_cond = COND_NE;
			/* Fall through */

cond_branch:
			switch (code) {
			case BPF_JMP | BPF_JGT | BPF_X:
			case BPF_JMP | BPF_JGE | BPF_X:
			case BPF_JMP | BPF_JEQ | BPF_X:
			case BPF_JMP | BPF_JNE | BPF_X:
				/* unsigned comparison */
				PPC_CMPLD(dst_reg, src_reg);
				break;
			case BPF_JMP | BPF_JSGT | BPF_X:
			case BPF_JMP | BPF_JSGE | BPF_X:
				/* signed comparison */
				PPC_CMPD(dst_reg, src_reg);
				break;
			case BPF_JMP | BPF_JSET | BPF_X:
				PPC_AND_DOT(b2p[TMP_REG_1], dst_reg, src_reg);
				break;
			case BPF_JMP | BPF_JNE | BPF_K:
			case BPF_JMP | BPF_JEQ | BPF_K:
			case BPF_JMP | BPF_JGT | BPF_K:
			case BPF_JMP | BPF_JGE | BPF_K:
				/*
				 * Need sign-extended load, so only positive
				 * values can be used as imm in cmpldi
				 */
				if (imm >= 0 && imm < 32768)
					PPC_CMPLDI(dst_reg, imm);
				else {
					/* sign-extending load */
					PPC_LI32(b2p[TMP_REG_1], imm);
					/* ... but unsigned comparison */
					PPC_CMPLD(dst_reg, b2p[TMP_REG_1]);
				}
				break;
			case BPF_JMP | BPF_JSGT | BPF_K:
			case BPF_JMP | BPF_JSGE | BPF_K:
				/*
				 * signed comparison, so any 16-bit value
				 * can be used in cmpdi
				 */
				if (imm >= -32768 && imm < 32768)
					PPC_CMPDI(dst_reg, imm);
				else {
					PPC_LI32(b2p[TMP_REG_1], imm);
					PPC_CMPD(dst_reg, b2p[TMP_REG_1]);
				}
				break;
			case BPF_JMP | BPF_JSET | BPF_K:
				/* andi does not sign-extend the immediate */
				if (imm >= 0 && imm < 32768)
					/* PPC_ANDI is _only/always_ dot-form */
					PPC_ANDI(b2p[TMP_REG_1], dst_reg, imm);
				else {
					PPC_LI32(b2p[TMP_REG_1], imm);
					PPC_AND_DOT(b2p[TMP_REG_1], dst_reg,
						    b2p[TMP_REG_1]);
				}
				break;
			}
			PPC_BCC(true_cond, addrs[i + 1 + off]);
			break;

		default:
			/*
			 * The filter contains something cruel & unusual.
			 * We don't handle it, but also there shouldn't be
			 * anything missing from our list.
			 */
			pr_err_ratelimited("eBPF filter opcode %04x (@%d) unsupported\n",
					code, i);
			return -ENOTSUPP;
		}
	}

	/* Set end-of-body-code address for exit. */
	addrs[i] = ctx->idx * 4;

	return 0;
}

void bpf_jit_compile(struct bpf_prog *fp) { }

void bpf_int_jit_compile(struct bpf_prog *fp)
{
	u32 proglen;
	u32 alloclen;
	u32 *image = NULL;
	u32 *code_base;
	u32 *addrs;
	struct codegen_context cgctx;
	int pass;
	int flen;

	if (!bpf_jit_enable)
		return;

	if (!fp || !fp->len)
		return;

	flen = fp->len;
	addrs = kzalloc((flen+1) * sizeof(*addrs), GFP_KERNEL);
	if (addrs == NULL)
		return;

	cgctx.idx = 0;
	cgctx.seen = 0;
	/* Scouting faux-generate pass 0 */
	if (bpf_jit_build_body(fp, 0, &cgctx, addrs))
		/* We hit something illegal or unsupported. */
		goto out;

	/*
	 * Pretend to build prologue, given the features we've seen.  This will
	 * update ctgtx.idx as it pretends to output instructions, then we can
	 * calculate total size from idx.
	 */
	bpf_jit_build_prologue(fp, 0, &cgctx);
	bpf_jit_build_epilogue(0, &cgctx);

	proglen = cgctx.idx * 4;
	alloclen = proglen + FUNCTION_DESCR_SIZE;
	image = module_alloc(alloclen);
	if (!image)
		goto out;

	code_base = image + (FUNCTION_DESCR_SIZE/4);

	/* Code generation passes 1-2 */
	for (pass = 1; pass < 3; pass++) {
		/* Now build the prologue, body code & epilogue for real. */
		cgctx.idx = 0;
		bpf_jit_build_prologue(fp, code_base, &cgctx);
		bpf_jit_build_body(fp, code_base, &cgctx, addrs);
		bpf_jit_build_epilogue(code_base, &cgctx);

		if (bpf_jit_enable > 1)
			pr_info("Pass %d: shrink = %d, seen = 0x%x\n", pass,
				proglen - (cgctx.idx * 4), cgctx.seen);
	}

	if (bpf_jit_enable > 1)
		/*
		 * Note that we output the base address of the code_base
		 * rather than image, since opcodes are in code_base.
		 */
		bpf_jit_dump(flen, proglen, pass, code_base);

	if (image) {
		flush_icache_range((unsigned long)code_base,
				(unsigned long)(code_base + (proglen/4)));
#if defined(CONFIG_PPC64) && (!defined(_CALL_ELF) || _CALL_ELF != 2)
		/* Function descriptor nastiness: Address + TOC */
		((u64 *)image)[0] = (u64)code_base;
		((u64 *)image)[1] = local_paca->kernel_toc;
#endif
		fp->bpf_func = (void *)image;
		fp->jited = 1;
	}
out:
	kfree(addrs);
}

void bpf_jit_free(struct bpf_prog *fp)
{
	if (fp->jited)
		module_memfree(fp->bpf_func);

	bpf_prog_unlock_free(fp);
}
