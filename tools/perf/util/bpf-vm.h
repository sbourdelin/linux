#ifndef _BPF_VM_H
#define _BPF_VM_H

struct bpf_insn;
unsigned int __bpf_prog_run(void *ctx, const struct bpf_insn *insn,
			    size_t ctx_len);

#endif /* _BPF_VM_H */
