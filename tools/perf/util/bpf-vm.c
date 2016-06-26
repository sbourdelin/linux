#include <linux/atomic.h>
#include "util.h"
#include "tools/be_byteshift.h"
#include "bpf-vm.h"

static inline void
bpf_vm_jmp_call_handler(u64 *regs __maybe_unused, void *ctx __maybe_unused,
			const struct bpf_insn *insn __maybe_unused) {}

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

#define UBPF_BUILD
#include <../../../kernel/bpf/vm.c>
