#include <linux/atomic.h>
#include "util.h"
#include "tools/be_byteshift.h"
#include "bpf-vm.h"
#include "debug.h"
#include <linux/filter.h>
#include "bpf/libbpf.h"
#include <linux/bpf.h>

#define BPF_R0	regs[BPF_REG_0]
#define BPF_R1	regs[BPF_REG_1]
#define BPF_R2	regs[BPF_REG_2]
#define BPF_R3	regs[BPF_REG_3]
#define BPF_R4	regs[BPF_REG_4]
#define BPF_R5	regs[BPF_REG_5]

#define DST	regs[insn->dst_reg]
#define SRC	regs[insn->src_reg]

static inline void
bpf_vm_jmp_call_handler(u64 *regs, void *ctx __maybe_unused,
			const struct bpf_insn *insn)
{
	const void *xfunc;
	u64 (*func)(u64 r1, u64 r2, u64 r3, u64 r4, u64 r5);

	xfunc = libbpf_get_ubpf_func(insn->imm);
	if (xfunc) {
		func = xfunc;
		BPF_R0 = func(BPF_R1, BPF_R2, BPF_R3,
			      BPF_R4, BPF_R5);
	}
}

static inline int
bpf_vm_jmp_tail_call_handler(u64 *regs __maybe_unused,
			     u32 *p_tail_call_cnt __maybe_unused,
			     const struct bpf_insn **p_insn __maybe_unused) {
	return 0;
}

static inline void
bpf_vm_default_label_handler(void *ctx __maybe_unused,
			     const struct bpf_insn *insn __maybe_unused) {}

/* instructions for dealing with socket data use this function,
 * userspace bpf don't use it now so leave it blank here, which saves
 * us from including too much kernel structures.
 */
static inline void *bpf_load_pointer(const void *skb __maybe_unused,
				     int k __maybe_unused,
				     unsigned int size __maybe_unused,
				     void *buffer __maybe_unused)
{
	return NULL;
}

static bool
bounds_check(void *addr, int size, void *ctx, size_t ctx_len, void *stack)
{
	if (ctx && (addr >= ctx && (addr + size) <= (ctx + ctx_len))) {
		/* Context access */
		return true;
	} else if (addr >= stack && (addr + size) <= (stack + MAX_BPF_STACK)) {
		/* Stack access */
		return true;
	}

	pr_debug("bpf: bounds_check failed\n");
	return false;
}

#define BOUNDS_CHECK_LOAD(size)						\
	do {								\
		if (!bounds_check((void *)SRC + insn->off, size,	\
				  ctx, ctx_len, stack)) {		\
			return -1;					\
		}							\
	} while (0)
#define BOUNDS_CHECK_STORE(size)					\
	do {								\
		if (!bounds_check((void *)DST + insn->off, size,	\
				  ctx, ctx_len, stack)) {		\
			return -1;					\
		}							\
	} while (0)

#define UBPF_BUILD
#include <../../../kernel/bpf/vm.c>
