/*
 * bpf-script-test-ubpf.c
 * Test user space BPF
 */

#ifndef LINUX_VERSION_CODE
# error Need LINUX_VERSION_CODE
# error Example: for 4.2 kernel, put 'clang-opt="-DLINUX_VERSION_CODE=0x40200" into llvm section of ~/.perfconfig'
#endif
#define BPF_ANY 0
#define BPF_MAP_TYPE_ARRAY 2
#define BPF_FUNC_map_lookup_elem 1
#define BPF_FUNC_map_update_elem 2

static void *(*bpf_map_lookup_elem)(void *map, void *key) =
	(void *) BPF_FUNC_map_lookup_elem;
static
void *(*bpf_map_update_elem)(void *map, void *key, void *value, int flags) =
	(void *) BPF_FUNC_map_update_elem;

struct bpf_map_def {
	unsigned int type;
	unsigned int key_size;
	unsigned int value_size;
	unsigned int max_entries;
};

#define SEC(NAME) __attribute__((section(NAME), used))
SEC("maps")
struct bpf_map_def counter = {
	.type = BPF_MAP_TYPE_ARRAY,
	.key_size = sizeof(int),
	.value_size = sizeof(int),
	.max_entries = 1,
};

SEC("func=sys_epoll_pwait")
int bpf_func__sys_epoll_pwait(void *ctx)
{
	int ind = 0;
	int *flag = bpf_map_lookup_elem(&counter, &ind);

	if (!flag)
		return 0;
	__sync_fetch_and_add(flag, 1);
	return 0;
}
char _license[] SEC("license") = "GPL";
int _version SEC("version") = LINUX_VERSION_CODE;

#define UBPF_FUNC_printf		4
#define UBPF_FUNC_map_lookup_elem	5
#define UBPF_FUNC_map_update_elem	6
#define UBPF_FUNC_test_report		63

static int (*ubpf_printf)(char *fmt, ...) = (void *)UBPF_FUNC_printf;
static void
(*ubpf_map_lookup_elem)(struct bpf_map_def *, void *, void *) =
	(void *)UBPF_FUNC_map_lookup_elem;
static void
(*ubpf_map_update_elem)(struct bpf_map_def *, void *, void *, int flags) =
	(void *)UBPF_FUNC_map_update_elem;
static void (*ubpf_test_report)(int) = (void *)UBPF_FUNC_test_report;

struct perf_record_end_ctx {
	int samples;
	int dummy;
};

SEC("UBPF;perf_record_start")
int perf_record_start(void)
{
	int idx = 0, val = 1000;

	ubpf_map_update_elem(&counter, &idx, &val, 0);
	return 0;
}

SEC("UBPF;perf_record_end")
int perf_record_end(struct perf_record_end_ctx *ctx)
{
	int idx = 0, val;

	ubpf_map_lookup_elem(&counter, &idx, &val);
	ubpf_test_report(val + ctx->samples);

	return 0;
}
