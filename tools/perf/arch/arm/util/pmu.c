#include <string.h>

#include <linux/coresight-pmu.h>
#include <linux/perf_event.h>

#include "../../util/pmu.h"

struct perf_event_attr
*perf_pmu__get_default_config(struct perf_pmu *pmu __maybe_unused)
{
#ifdef HAVE_AUXTRACE_SUPPORT_ARM
	if (!strcmp(pmu->name, CORESIGHT_ETM_PMU_NAME)) {
		/* add ETM default config here */
		pmu->selectable = true;
	}
#endif
	return NULL;
}
