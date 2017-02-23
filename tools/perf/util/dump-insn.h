#ifndef DUMP_INSN_H
#define DUMP_INSN_H 1

#define MAXINSN 15

struct perf_insn {
	/* Initialized by callers: */
	struct thread *thread;
	u8 cpumode;
	int cpu;
	bool is64bit;
	/* Temporary */
	char out[256];
};

const char *dump_insn(struct perf_insn *x, uint64_t ip,
		      u8 *inbuf, int inlen, int *lenp);

#endif
