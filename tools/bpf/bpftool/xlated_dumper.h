/*
 * Copyright (C) 2018 Netronome Systems, Inc.
 *
 * This software is dual licensed under the GNU General License Version 2,
 * June 1991 as shown in the file COPYING in the top-level directory of this
 * source tree or the BSD 2-Clause License provided below.  You have the
 * option to license this software under the complete terms of either license.
 *
 * The BSD 2-Clause License:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      1. Redistributions of source code must retain the above
 *         copyright notice, this list of conditions and the following
 *         disclaimer.
 *
 *      2. Redistributions in binary form must reproduce the above
 *         copyright notice, this list of conditions and the following
 *         disclaimer in the documentation and/or other materials
 *         provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#ifndef __BPF_TOOL_XLATED_DUMPER_H
#define __BPF_TOOL_XLATED_DUMPER_H

#define SYM_MAX_NAME	256

struct kernel_sym {
	unsigned long address;
	char name[SYM_MAX_NAME];
};

struct dump_data {
	unsigned long address_call_base;
	struct kernel_sym *sym_mapping;
	__u32 sym_count;
	char scratch_buff[SYM_MAX_NAME];
};

void kernel_syms_load(struct dump_data *dd);
void kernel_syms_destroy(struct dump_data *dd);
void dump_xlated_json(struct dump_data *dd, void *buf, unsigned int len,
		      bool opcodes);
void dump_xlated_plain(struct dump_data *dd, void *buf, unsigned int len,
		       bool opcodes);
void dump_xlated_for_graph(struct dump_data *dd, void *buf, void *buf_end,
			   unsigned int start_index);

#endif
