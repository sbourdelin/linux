// SPDX-License-Identifier: GPL-2.0
#include "mem-events.h"

struct perf_mem_event perf_mem_events[PERF_MEM_EVENTS__MAX] = {
	PERF_MEM_EVENT("ldlat-loads", "cpu/mem-loads,ldlat=%u/P", "mem-loads"),
	PERF_MEM_EVENT("ldlat-stores", "cpu/mem-stores/P", "mem-stores"),
};

static char mem_loads_name[100];
static bool mem_loads_name__init;

char *perf_mem_events__name(int i)
{
	if (i == PERF_MEM_EVENTS__LOAD) {
		if (!mem_loads_name__init) {
			mem_loads_name__init = true;
			scnprintf(mem_loads_name, sizeof(mem_loads_name),
				  perf_mem_events[i].name,
				  perf_mem_events__loads_ldlat);
		}
		return mem_loads_name;
	}

	return (char *)perf_mem_events[i].name;
}
