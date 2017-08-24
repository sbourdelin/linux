#include <stdio.h>
#include <stdlib.h>
#include <api/fs/fs.h>
#include "header.h"

#define MIDR "/regs/identification/midr_el1"
#define MIDR_SIZE 19

char *get_cpuid_str(struct perf_pmu *pmu)
{
	char *buf = NULL;
	char path[PATH_MAX];
	const char *sysfs = sysfs__mountpoint();
	int cpu;
	u64 midr = 0;
	struct cpu_map *cpus;
	FILE *file;

	if (!sysfs || !pmu->cpus)
		return NULL;

	buf = malloc(MIDR_SIZE);
	if (!buf)
		return NULL;

	/* read midr from list of cpus mapped to this pmu */
	cpus = cpu_map__get(pmu->cpus);
	for (cpu = 0; cpu < cpus->nr; cpu++) {
		scnprintf(path, PATH_MAX, "%s/devices/system/cpu/cpu%d"MIDR,
				sysfs, cpus->map[cpu]);

		file = fopen(path, "r");
		if (!file) {
			pr_debug("fopen failed for file %s\n", path);
			continue;
		}

		if (!fgets(buf, MIDR_SIZE, file))
			continue;
		fclose(file);

		/* Ignore/clear Variant[23:20] and
		 * Revision[3:0] of MIDR
		 */
		midr = strtoul(buf, NULL, 16);
		midr &= (~(0xf << 20 | 0xf));
		scnprintf(buf, MIDR_SIZE, "0x%016lx", midr);
		/* got midr break loop */
		break;
	}

	if (!midr) {
		pr_err("failed to get cpuid string\n");
		free(buf);
		buf = NULL;
	}

	cpu_map__put(cpus);
	return buf;
}
