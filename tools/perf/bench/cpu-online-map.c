#include <sched.h>
#include <unistd.h>
#include <err.h>
#include <stdlib.h>
#include "cpu-online-map.h"

void compute_cpu_online_map(int *cpu_online_map)
{
	long ncpus_conf = sysconf(_SC_NPROCESSORS_CONF);
	cpu_set_t set;
	size_t j = 0;

	CPU_ZERO(&set);

	if (sched_getaffinity(0, sizeof(set), &set))
		err(EXIT_FAILURE, "sched_getaffinity");

	for (int i = 0; i < ncpus_conf; i++)
		if (CPU_ISSET(i, &set))
			cpu_online_map[j++] = i;
}
