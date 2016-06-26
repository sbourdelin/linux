#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <linux/bpf.h>

#include "bpf.h"
#include "libbpf-internal.h"

static int engine__init(struct bpf_program *prog)
{
	int *instances_entries;
	int nr_instances = prog->instances.nr;

	instances_entries = malloc(sizeof(int) * nr_instances);
	if (!instances_entries) {
		pr_warning("alloc memory failed for instances\n");
		return -ENOMEM;
	}

	/* fill all fd with -1 */
	memset(instances_entries, -1, sizeof(int) * nr_instances);

	prog->instances.entries = instances_entries;

	return 0;
}

static int engine__get_nth(struct bpf_program *prog, int n, void *ret)
{
	int *pfd = (int *)ret;

	if (n >= prog->instances.nr || n < 0) {
		pr_warning("Can't get the %dth fd from program %s: only %d instances\n",
			   n, prog->section_name, prog->instances.nr);
		return -EINVAL;
	}

	*pfd = ((int *)prog->instances.entries)[n];
	if (*pfd < 0) {
		pr_warning("%dth instance of program '%s' is invalid\n",
			   n, prog->section_name);
		return -ENOENT;
	}

	return 0;
}

static void engine__unload(struct bpf_program *prog, int index)
{
	zclose(((int *)prog->instances.entries)[index]);
}

static int
load_program(struct bpf_insn *insns, int insns_cnt,
	     char *license, u32 kern_version, int *pfd)
{
	int ret;
	char *log_buf;

	if (!insns || !insns_cnt)
		return -EINVAL;

	log_buf = malloc(BPF_LOG_BUF_SIZE);
	if (!log_buf)
		pr_warning("Alloc log buffer for bpf loader error, continue without log\n");

	ret = bpf_load_program(BPF_PROG_TYPE_KPROBE, insns,
			       insns_cnt, license, kern_version,
			       log_buf, BPF_LOG_BUF_SIZE);

	if (ret >= 0) {
		*pfd = ret;
		ret = 0;
		goto out;
	}

	ret = -LIBBPF_ERRNO__LOAD;
	pr_warning("load bpf program failed: %s\n", strerror(errno));

	if (log_buf && log_buf[0] != '\0') {
		ret = -LIBBPF_ERRNO__VERIFY;
		pr_warning("-- BEGIN DUMP LOG ---\n");
		pr_warning("\n%s\n", log_buf);
		pr_warning("-- END LOG --\n");
	} else {
		if (insns_cnt >= BPF_MAXINSNS) {
			pr_warning("Program too large (%d insns), at most %d insns\n",
				   insns_cnt, BPF_MAXINSNS);
			ret = -LIBBPF_ERRNO__PROG2BIG;
		} else if (log_buf) {
			pr_warning("log buffer is empty\n");
			ret = -LIBBPF_ERRNO__KVER;
		}
	}

out:
	free(log_buf);
	return ret;
}

static int engine__load(struct bpf_program *prog, struct bpf_insn *insns,
			int insns_cnt, char *license,
			u32 kern_version, int index)
{
	int err = 0;
	int fd;

	if (!insns || !insns_cnt) {
		((int *)prog->instances.entries)[index] = -1;
		pr_debug("Skip loading the %dth instance of program '%s'\n",
			 index, prog->section_name);
		return err;
	}

	err = load_program(insns, insns_cnt, license, kern_version, &fd);
	if (!err)
		((int *)prog->instances.entries)[index] = fd;
	else
		pr_warning("Loading the %dth instance of program '%s' failed\n",
			   index, prog->section_name);

	return err;
}

struct bpf_engine kengine = {
	.init		= engine__init,
	.load		= engine__load,
	.unload		= engine__unload,
	.get_nth	= engine__get_nth,
};
