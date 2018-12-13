/* SPDX-License-Identifier: (GPL-2.0-only OR BSD-2-Clause) */
#ifndef __PROBE_HELPERS_H
#define __PROBE_HELPERS_H

#include <linux/bpf.h>

bool bpf_prog_type_supported(enum bpf_prog_type prog_type);
bool bpf_map_type_supported(enum bpf_map_type map_type);

#endif
