#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <linux/bpf.h>

#include "bpf.h"
#include "libbpf-internal.h"

static struct {
	const char *name;
	void *func;
} ubpf_funcs[MAX_UBPF_FUNC];

const void *libbpf_get_ubpf_func(unsigned int func_id)
{
	if (func_id >= MAX_UBPF_FUNC)
		return NULL;

	return ubpf_funcs[func_id].func;
}

int libbpf_set_ubpf_func(unsigned int idx, const char *name, void *func)
{
	if (idx >= MAX_UBPF_FUNC)
		return -E2BIG;
	if (!func)
		return -EINVAL;

	ubpf_funcs[idx].name = name;
	ubpf_funcs[idx].func = func;
	return 0;
}

static int engine__init(struct bpf_program *prog)
{
	struct ubpf_entry *instances_entries;
	int nr_instances = prog->instances.nr;

	instances_entries = malloc(sizeof(struct ubpf_entry) * nr_instances);
	if (!instances_entries) {
		pr_warning("alloc memory failed for instances\n");
		return -ENOMEM;
	}

	/* fill all entries with NULL */
	memset(instances_entries, 0,
	       sizeof(instances_entries[0]) * nr_instances);

	prog->instances.entries = instances_entries;

	return 0;
}

static int engine__get_nth(struct bpf_program *prog, int n, void *ret)
{
	struct ubpf_entry **p_vm = (struct ubpf_entry **)ret;

	if (n >= prog->instances.nr || n < 0) {
		pr_warning("Can't get the %dth vm from program %s: only %d instances\n",
			   n, prog->section_name, prog->instances.nr);
		return -EINVAL;
	}

	*p_vm = &((struct ubpf_entry *)prog->instances.entries)[n];

	return 0;
}

static void engine__unload(struct bpf_program *prog, int index)
{
	struct ubpf_entry *v =
		&((struct ubpf_entry *)prog->instances.entries)[index];

	free(v->insns);
}

static int
load_ubpf_program(struct bpf_insn *insns, int insns_cnt,
		  struct ubpf_entry *entry)
{
	entry->insns = malloc(insns_cnt * sizeof(struct bpf_insn));
	if (!entry->insns) {
		pr_warning("Failed to create ubpf entry\n");
		return -LIBBPF_ERRNO__LOADUBPF;
	}

	memcpy(entry->insns, &insns->code, insns_cnt * sizeof(struct bpf_insn));

	return 0;
}

static int engine__load(struct bpf_program *prog, struct bpf_insn *insns,
			int insns_cnt, char *license __maybe_unused,
			u32 kern_version __maybe_unused, int index)
{
	int err = 0;
	struct ubpf_entry entry;

	if (!insns || !insns_cnt) {
		((struct ubpf_entry **)prog->instances.entries)[index] = NULL;
		pr_debug("Skip loading the %dth instance of program '%s'\n",
			 index, prog->section_name);
		return err;
	}

	err = load_ubpf_program(insns, insns_cnt, &entry);
	if (!err)
		((struct ubpf_entry *)prog->instances.entries)[index] = entry;
	else
		pr_warning("Loading the %dth instance of program '%s' failed\n",
			   index, prog->section_name);

	return err;
}

struct bpf_engine uengine = {
	.init		= engine__init,
	.load		= engine__load,
	.unload		= engine__unload,
	.get_nth	= engine__get_nth,
};

int bpf_program__set_ubpf(struct bpf_program *prog)
{
	prog->engine = &uengine;

	return 0;
}

bool bpf_program__is_ubpf(struct bpf_program *prog)
{
	return prog->engine == &uengine;
}
