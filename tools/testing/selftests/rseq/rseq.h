#ifndef RSEQ_TEST_H
#define RSEQ_TEST_H

int sys_rseq(int op, int flags, void *val1, void *val2, void *val3);
/* RSEQ provided thread-local current_cpu */

void rseq_configure_cpu_pointer(void);

void rseq_configure_region(void *rseq_text_start, void *rseq_text_end,
	void *rseq_text_restart);

extern __thread volatile const int __rseq_current_cpu;
static inline int rseq_current_cpu(void) { return __rseq_current_cpu; }

void run_tests(void);

#endif
