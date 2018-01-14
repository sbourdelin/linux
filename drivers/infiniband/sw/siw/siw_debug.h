/*
 * Software iWARP device driver
 *
 * Authors: Fredy Neeser <nfd@zurich.ibm.com>
 *          Bernard Metzler <bmt@zurich.ibm.com>
 *
 * Copyright (c) 2008-2017, IBM Corporation
 *
 * This software is available to you under a choice of one of two
 * licenses. You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * BSD license below:
 *
 *   Redistribution and use in source and binary forms, with or
 *   without modification, are permitted provided that the following
 *   conditions are met:
 *
 *   - Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *
 *   - Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *
 *   - Neither the name of IBM nor the names of its contributors may be
 *     used to endorse or promote products derived from this software without
 *     specific prior written permission.
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

#ifndef _SIW_DEBUG_H
#define _SIW_DEBUG_H

#include <linux/uaccess.h>
#include <linux/hardirq.h>	/* in_interrupt() */

struct siw_device;
struct siw_iwarp_rx;
union iwarp_hdr;

extern void siw_debug_init(void);
extern void siw_debugfs_add_device(struct siw_device *dev);
extern void siw_debugfs_del_device(struct siw_device *dev);
extern void siw_debugfs_delete(void);

extern void siw_print_hdr(union iwarp_hdr *hdr, int id, char *msg);
extern void siw_print_qp_attr_mask(enum ib_qp_attr_mask mask, char *msg);
extern char ib_qp_state_to_string[IB_QPS_ERR+1][sizeof "RESET"];

#ifndef refcount_read
#define refcount_read(x)	atomic_read(x.refcount.refs)
#endif

#define siw_dbg(ddev, fmt, ...) \
	dev_dbg(&(ddev)->base_dev.dev, "cpu%2d %s: " fmt, smp_processor_id(),\
			__func__, ##__VA_ARGS__)

#define siw_dbg_qp(qp, fmt, ...) \
	siw_dbg(qp->hdr.sdev, "[QP %d]: " fmt, QP_ID(qp), ##__VA_ARGS__)

#define siw_dbg_cep(cep, fmt, ...) \
	siw_dbg(cep->sdev, "[CEP 0x%p]: " fmt, cep, ##__VA_ARGS__)

#define siw_dbg_obj(obj, fmt, ...) \
	siw_dbg(obj->hdr.sdev, "[OBJ ID %d]: " fmt, obj->hdr.id, ##__VA_ARGS__)

#ifdef DEBUG

#define siw_dprint_hdr(h, i, m)	siw_print_hdr(h, i, m)

#else

#define dprint(dbgcat, fmt, args...)	do { } while (0)
#define siw_dprint_hdr(h, i, m)	do { } while (0)

#endif

#endif
