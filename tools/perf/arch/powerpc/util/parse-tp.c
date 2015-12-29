#include "../../util/evsel.h"
#include "../../util/trace-event.h"
#include "../../util/session.h"
#include "../../util/util.h"

#define KVMPPC_EXIT "kvm_hv:kvm_guest_exit"
#define HV_DECREMENTER 2432
#define HV_BIT 3
#define PR_BIT 49
#define PPC_MAX 63

static bool is_kvmppc_exit_event(struct perf_evsel *evsel)
{
	static unsigned int kvmppc_exit;

	if (evsel->attr.type != PERF_TYPE_TRACEPOINT)
		return false;

	if (unlikely(kvmppc_exit == 0)) {
		if (strcmp(KVMPPC_EXIT, evsel->name))
			return false;
		kvmppc_exit = evsel->attr.config;
	} else if (kvmppc_exit != evsel->attr.config) {
		return false;
	}

	return true;
}

static bool is_hv_dec_trap(struct perf_evsel *evsel, struct perf_sample *sample)
{
	int trap = perf_evsel__intval(evsel, sample, "trap");
	return trap == HV_DECREMENTER;
}

/*
 * Get the instruction pointer from the tracepoint data
 */
u64 arch__get_ip(struct perf_evsel *evsel, struct perf_sample *sample)
{
	if (perf_guest_only() &&
	    is_kvmppc_exit_event(evsel) &&
	    is_hv_dec_trap(evsel, sample))
		return perf_evsel__intval(evsel, sample, "pc");

	return sample->ip;
}

/*
 * Get the HV and PR bits and accordingly, determine the cpumode
 */
u8 arch__get_cpumode(const union perf_event *event, struct perf_evsel *evsel,
		     struct perf_sample *sample)
{
	unsigned long hv, pr, msr;
	u8 cpumode = event->header.misc & PERF_RECORD_MISC_CPUMODE_MASK;

	if (!perf_guest_only() || !is_kvmppc_exit_event(evsel))
		goto ret;

	if (sample->raw_data && is_hv_dec_trap(evsel, sample)) {
		msr = perf_evsel__intval(evsel, sample, "msr");
		hv = msr & ((unsigned long)1 << (PPC_MAX - HV_BIT));
		pr = msr & ((unsigned long)1 << (PPC_MAX - PR_BIT));

		if (!hv && pr)
			cpumode = PERF_RECORD_MISC_GUEST_USER;
		else
			cpumode = PERF_RECORD_MISC_GUEST_KERNEL;
	}

ret:
	return cpumode;
}
