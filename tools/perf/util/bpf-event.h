/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __PERF_BPF_EVENT_H
#define __PERF_BPF_EVENT_H

#include "machine.h"

int machine__process_bpf_event(struct machine *machine,
			       union perf_event *event,
			       struct perf_sample *sample);

#endif
