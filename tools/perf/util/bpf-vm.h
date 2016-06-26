#ifndef _BPF_VM_H
#define _BPF_VM_H

struct bpf_insn;
unsigned int __bpf_prog_run(void *ctx, const struct bpf_insn *insn);

#endif /* _BPF_VM_H */
