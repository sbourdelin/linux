// SPDX-License-Identifier: GPL-2.0
#include <linux/filter.h>
#include <linux/ptrace.h>
#include <linux/version.h>
#include <uapi/linux/bpf.h>
#include <linux/rds.h>
#include "bpf_helpers.h"

#define PROG(F) SEC("socksg/"__stringify(F)) int bpf_func_##F

#define bpf_printk(fmt, ...)				\
({							\
	char ____fmt[] = fmt;				\
	bpf_trace_printk(____fmt, sizeof(____fmt),	\
			##__VA_ARGS__);			\
})

struct bpf_map_def SEC("maps") jmp_table = {
	.type = BPF_MAP_TYPE_PROG_ARRAY,
	.key_size = sizeof(u32),
	.value_size = sizeof(u32),
	.max_entries = 2,
};

#define SG1 1

static inline void dump_sg(struct sg_filter_md *sg)
{
	void *data = (void *)(long) sg->data;
	void *data_end = (void *)(long) sg->data_end;
	unsigned char *d;

	if (data + 8 > data_end)
		return;

	d = (unsigned char *)data;
	bpf_printk("%x %x %x\n", d[0], d[1], d[2]);
	bpf_printk("%x %x %x\n", d[3], d[4], d[5]);

	return;

}

static void sg_dispatcher(struct sg_filter_md *sg)
{
	int ret;

	ret = bpf_sg_next(sg);
	if (ret == -ENODATA) {
		bpf_printk("no more sg element\n");
		return;
	}

	/* We use same function to walk sg list */
	bpf_tail_call(sg, &jmp_table, 1);
}

/* walk sg list */
PROG(SG1)(struct sg_filter_md *sg)
{
	bpf_printk("next sg element:\n");
	dump_sg(sg);
	sg_dispatcher(sg);
	return 0;
}

SEC("socksg/0")
int main_prog(struct sg_filter_md *sg)
{
	bpf_printk("Print first 6 bytes from sg element\n");
	bpf_printk("First sg element:\n");
	dump_sg(sg);
	sg_dispatcher(sg);
	return 0;
}

char _license[] SEC("license") = "GPL";
u32 _version SEC("version") = LINUX_VERSION_CODE;
