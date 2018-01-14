/*
 * Software iWARP device driver
 *
 * Authors: Bernard Metzler <bmt@zurich.ibm.com>
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

#include <linux/errno.h>
#include <linux/types.h>
#include <linux/net.h>
#include <linux/scatterlist.h>
#include <linux/highmem.h>
#include <net/sock.h>
#include <net/tcp_states.h>
#include <net/tcp.h>

#include <rdma/iw_cm.h>
#include <rdma/ib_verbs.h>
#include <rdma/ib_smi.h>
#include <rdma/ib_user_verbs.h>

#include "siw.h"
#include "siw_obj.h"
#include "siw_cm.h"

void siw_qp_event(struct siw_qp *qp, enum ib_event_type etype)
{
	struct ib_event event;
	struct ib_qp	*base_qp = &qp->base_qp;

	/*
	 * Do not report asynchronous errors on QP which gets
	 * destroyed via verbs interface (siw_destroy_qp())
	 */
	if (qp->attrs.flags & SIW_QP_IN_DESTROY)
		return;

	event.event = etype;
	event.device = base_qp->device;
	event.element.qp = base_qp;

	if (base_qp->event_handler) {
		siw_dbg_qp(qp, "reporting event %d\n", etype);
		(*base_qp->event_handler)(&event, base_qp->qp_context);
	}
}

void siw_cq_event(struct siw_cq *cq, enum ib_event_type etype)
{
	struct ib_event event;
	struct ib_cq	*base_cq = &cq->base_cq;

	event.event = etype;
	event.device = base_cq->device;
	event.element.cq = base_cq;

	if (base_cq->event_handler) {
		siw_dbg(cq->hdr.sdev, "reporting CQ event %d\n", etype);
		(*base_cq->event_handler)(&event, base_cq->cq_context);
	}
}

void siw_srq_event(struct siw_srq *srq, enum ib_event_type etype)
{
	struct ib_event event;
	struct ib_srq	*base_srq = &srq->base_srq;

	event.event = etype;
	event.device = base_srq->device;
	event.element.srq = base_srq;

	if (base_srq->event_handler) {
		siw_dbg(srq->pd->hdr.sdev, "reporting SRQ event %d\n", etype);
		(*base_srq->event_handler)(&event, base_srq->srq_context);
	}
}

void siw_port_event(struct siw_device *sdev, u8 port, enum ib_event_type etype)
{
	struct ib_event event;

	event.event = etype;
	event.device = &sdev->base_dev;
	event.element.port_num = port;

	siw_dbg(sdev, "reporting port event %d\n", etype);

	ib_dispatch_event(&event);
}
