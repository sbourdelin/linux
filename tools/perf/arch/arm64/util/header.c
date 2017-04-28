#include <stdio.h>
#include <stdlib.h>
#include "header.h"

#define LINUX_SYS_CPU_DIRECTORY "/sys/devices/system/cpu/"
#define MIDR "/regs/identification/midr_el1"

char *get_cpuid_str(struct perf_pmu *pmu)
{
	char *buf = malloc(128);
	FILE *file;
	char *ret = NULL;
	int cpu;
	char sys_file[256];
	struct cpu_map *cpus;

	if (!pmu->cpus)
		return NULL;

	/* read midr from list of cpus mapped to this pmu */
	cpus = cpu_map__get(pmu->cpus);

	for (cpu = 0; cpu < cpus->nr; cpu++) {
		sprintf(sys_file, LINUX_SYS_CPU_DIRECTORY"cpu%d"MIDR,
				cpus->map[cpu]);
		file = fopen(sys_file, "r");
		if (file) {
			ret = fgets(buf, 128, file);
			if (ret) {
				buf[strlen(buf)-1] = '\0';
				fclose(file);
				cpu_map__put(cpus);
				return buf;
			}
			fclose(file);
		}
	}
	cpu_map__put(cpus);
	free(buf);
	return ret;
}
