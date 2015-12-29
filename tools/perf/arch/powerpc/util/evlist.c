#include <linux/err.h>
#include "../../util/evsel.h"
#include "../../util/evlist.h"

/*
 * To sample for only guest, record kvm_hv:kvm_guest_exit.
 * Otherwise go via normal way(cycles).
 */
int perf_evlist__arch_add_default(struct perf_evlist *evlist)
{
	struct perf_evsel *evsel;

	if (!perf_guest_only())
		return -1;

	evsel = perf_evsel__newtp_idx("kvm_hv", "kvm_guest_exit", 0);
	if (IS_ERR(evsel))
		return PTR_ERR(evsel);

	perf_evlist__add(evlist, evsel);
	return 0;
}
