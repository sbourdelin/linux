#include <stdio.h>
#include <stdlib.h>
#include <linux/bpf.h>
#include <unistd.h>
#include "libbpf.h"
#include "bpf_load.h"

int main(int ac, char **argv)
{
	FILE *f;
	char filename[256];
	int ret;

	snprintf(filename, sizeof(filename), "%s_kern.o", argv[0]);

	if (load_bpf_file(filename)) {
		printf("%s", bpf_log_buf);
		return 1;
	}

	ret = system(
		"echo ip_rcv > /sys/kernel/debug/tracing/set_graph_function");
	if (ret != 0) {
		printf("set_graph_function failed\n");
		return 1;
	}
	ret = system(
		"echo function_graph > /sys/kernel/debug/tracing/current_tracer");
	if (ret != 0) {
		printf("set current_tracer faield\n");
		return 1;
	}
	ret = system(
		"echo 1 > /sys/kernel/debug/tracing/tracing_on");
	if (ret != 0) {
		printf("tracing_on failed\n");
		return 1;
	}
	f = popen("nc localhost 9001", "r");
	(void) f;

	read_trace_pipe();

	return 0;
}
