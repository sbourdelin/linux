// SPDX-License-Identifier: GPL-2.0
#include <opencsd/c_api/opencsd_c_api.h>

int main(void)
{
	/*
	 * Requires ocsd_generic_trace_elem.num_instr_range introduced in
	 * OpenCSD 0.9
	 */
	ocsd_generic_trace_elem elem;
	(void)elem.num_instr_range;

	(void)ocsd_get_version();
	return 0;
}
