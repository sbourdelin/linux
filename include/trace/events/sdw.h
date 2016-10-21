/*
 * sdw.h - SDW message transfer tracepoints.
 *
 * Author: Hardik Shah <hardik.t.shah@intel.com>
 *
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 *
 * This file is provided under a dual BSD/GPLv2 license.  When using or
 * redistributing this file, you may do so under either license.
 *
 * GPL LICENSE SUMMARY
 *
 * Copyright(c) 2016 Intel Corporation.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 *
 * BSD LICENSE
 *
 * Copyright(c) 2016 Intel Corporation.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in
 *     the documentation and/or other materials provided with the
 *     distribution.
 *   * Neither the name of Intel Corporation nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 *
 */

#undef TRACE_SYSTEM
#define TRACE_SYSTEM sdw

#if !defined(_TRACE_SDW_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_SDW_H

#include <linux/mod_devicetable.h>
#include <sound/sdw_bus.h>
#include <linux/tracepoint.h>

/*
 * __sdw_transfer() write request
 */
TRACE_EVENT_FN(sdw_write,
	       TP_PROTO(const struct sdw_master *mstr,
			const struct sdw_msg *msg,
			int num),
	       TP_ARGS(mstr, msg, num),
	       TP_STRUCT__entry(
		       __field(int,	master_nr)
		       __field(__u16,	msg_nr)
		       __field(__u8,	addr_page1)
		       __field(__u8,	addr_page2)
		       __field(__u16,	addr)
		       __field(__u16,	flag)
		       __field(__u16,	len)
		       __dynamic_array(__u8, buf, msg->len)),
	       TP_fast_assign(
		       __entry->master_nr = mstr->nr;
		       __entry->msg_nr = num;
		       __entry->addr = msg->addr;
		       __entry->flag = msg->r_w_flag;
		       __entry->len = msg->len;
		       __entry->addr_page1 = msg->addr_page1;
		       __entry->addr_page2 = msg->addr_page2;
		       memcpy(__get_dynamic_array(buf), msg->buf, msg->len);
			      ),
	       TP_printk("sdw-%d #%u a=%03x addr_page1=%04x addr_page2=%04x f=%04x l=%u [%*phD]",
			 __entry->master_nr,
			 __entry->msg_nr,
			 __entry->addr,
			 __entry->addr_page1,
			 __entry->addr_page2,
			 __entry->flag,
			 __entry->len,
			 __entry->len, __get_dynamic_array(buf)
			 ),
	       sdw_transfer_trace_reg,
	       sdw_transfer_trace_unreg);

/*
 * __sdw_transfer() read request
 */
TRACE_EVENT_FN(sdw_read,
	       TP_PROTO(const struct sdw_master *mstr, const struct sdw_msg *msg,
			int num),
	       TP_ARGS(mstr, msg, num),
	       TP_STRUCT__entry(
		       __field(int,	master_nr)
		       __field(__u16,	msg_nr)
		       __field(__u8,	addr_page1)
		       __field(__u8,	addr_page2)
		       __field(__u16,	addr)
		       __field(__u16,	flag)
		       __field(__u16,	len)
		       __dynamic_array(__u8, buf, msg->len)),
	       TP_fast_assign(
		       __entry->master_nr = mstr->nr;
		       __entry->msg_nr = num;
		       __entry->addr = msg->addr;
		       __entry->flag = msg->r_w_flag;
		       __entry->len = msg->len;
		       __entry->addr_page1 = msg->addr_page1;
		       __entry->addr_page2 = msg->addr_page2;
		       memcpy(__get_dynamic_array(buf), msg->buf, msg->len);
			      ),
	       TP_printk("sdw-%d #%u a=%03x addr_page1=%04x addr_page2=%04x f=%04x l=%u [%*phD]",
			 __entry->master_nr,
			 __entry->msg_nr,
			 __entry->addr,
			 __entry->addr_page1,
			 __entry->addr_page2,
			 __entry->flag,
			 __entry->len,
			 __entry->len, __get_dynamic_array(buf)
			 ),
	       sdw_transfer_trace_reg,
	sdw_transfer_trace_unreg);

/*
 * __sdw_transfer() read reply
 */
TRACE_EVENT_FN(sdw_reply,
	       TP_PROTO(const struct sdw_master *mstr,
			const struct sdw_msg *msg,
			int num),
	       TP_ARGS(mstr, msg, num),
	       TP_STRUCT__entry(
		       __field(int,	master_nr)
		       __field(__u16,	msg_nr)
		       __field(__u16,	addr)
		       __field(__u16,	flag)
		       __field(__u16,	len)
		       __dynamic_array(__u8, buf, msg->len)),
	       TP_fast_assign(
		       __entry->master_nr = mstr->nr;
		       __entry->msg_nr = num;
		       __entry->addr = msg->addr;
		       __entry->flag = msg->r_w_flag;
		       __entry->len = msg->len;
		       memcpy(__get_dynamic_array(buf), msg->buf, msg->len);
			      ),
	       TP_printk("sdw-%d #%u a=%03x f=%04x l=%u [%*phD]",
			 __entry->master_nr,
			 __entry->msg_nr,
			 __entry->addr,
			 __entry->flag,
			 __entry->len,
			 __entry->len, __get_dynamic_array(buf)
			 ),
	       sdw_transfer_trace_reg,
	       sdw_transfer_trace_unreg);

/*
 * __sdw_transfer() result
 */
TRACE_EVENT_FN(sdw_result,
	       TP_PROTO(const struct sdw_master *mstr, int num, int ret),
	       TP_ARGS(mstr, num, ret),
	       TP_STRUCT__entry(
		       __field(int,	master_nr)
		       __field(__u16,	nr_msgs)
		       __field(__s16,	ret)
				),
	       TP_fast_assign(
		       __entry->master_nr = mstr->nr;
		       __entry->nr_msgs = num;
		       __entry->ret = ret;
			      ),
	       TP_printk("sdw-%d n=%u ret=%d",
			 __entry->master_nr,
			 __entry->nr_msgs,
			 __entry->ret
			 ),
	       sdw_transfer_trace_reg,
	       sdw_transfer_trace_unreg);

#endif /* _TRACE_SDW_H */

/* This part must be outside protection */
#include <trace/define_trace.h>
