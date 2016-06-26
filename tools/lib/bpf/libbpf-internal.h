#ifndef _LIBBPF_INTERNAL_H
#define _LIBBPF_INTERNAL_H

#include "libbpf.h"

#define __printf(a, b)	__attribute__((format(printf, a, b)))

#define __pr(func, fmt, ...)	\
do {				\
	if ((func))		\
		(func)("libbpf: " fmt, ##__VA_ARGS__); \
} while (0)

extern libbpf_print_fn_t __pr_bpf_warning;
extern libbpf_print_fn_t __pr_bpf_info;
extern libbpf_print_fn_t __pr_bpf_debug;

#define pr_warning(fmt, ...)	__pr(__pr_bpf_warning, fmt, ##__VA_ARGS__)
#define pr_info(fmt, ...)	__pr(__pr_bpf_info, fmt, ##__VA_ARGS__)
#define pr_debug(fmt, ...)	__pr(__pr_bpf_debug, fmt, ##__VA_ARGS__)

/* Copied from tools/perf/util/util.h */
#ifndef zfree
# define zfree(ptr) ({ free(*ptr); *ptr = NULL; })
#endif

#ifndef zclose
# define zclose(fd) ({			\
	int ___err = 0;			\
	if ((fd) >= 0)			\
		___err = close((fd));	\
	fd = -1;			\
	___err; })
#endif

/*
 * bpf_prog should be a better name but it has been used in
 * linux/filter.h.
 */
struct bpf_program {
	/* Index in elf obj file, for relocation use. */
	int idx;
	char *section_name;
	struct bpf_insn *insns;
	size_t insns_cnt;

	struct {
		int insn_idx;
		int map_idx;
	} *reloc_desc;
	int nr_reloc;

	struct bpf_engine *engine;
	struct {
		int nr;
		void *entries;
	} instances;
	bpf_program_prep_t preprocessor;

	struct bpf_object *obj;
	void *priv;
	bpf_program_clear_priv_t clear_priv;
};

struct bpf_engine {
	int (*init)(struct bpf_program *prog);
	int (*load)(struct bpf_program *prog, struct bpf_insn *insns,
		    int insns_cnt, char *license,
		    u32 kern_version, int index);
	void (*unload)(struct bpf_program *prog, int index);
	int (*get_nth)(struct bpf_program *prog, int index, void *ret);
};

extern struct bpf_engine kengine;

#endif /* _LIBBPF_INTERNAL_H */
