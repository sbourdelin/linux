/* SCTP kernel implementation
 * (C) Copyright IBM Corp. 2001, 2004
 * Copyright (c) 1999-2000 Cisco, Inc.
 * Copyright (c) 1999-2001 Motorola, Inc.
 * Copyright (c) 2001 Intel Corp.
 *
 * This file is part of the SCTP kernel implementation
 *
 * These functions manipulate sctp tsn mapping array.
 *
 * This SCTP implementation is free software;
 * you can redistribute it and/or modify it under the terms of
 * the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This SCTP implementation is distributed in the hope that it
 * will be useful, but WITHOUT ANY WARRANTY; without even the implied
 *                 ************************
 * warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with GNU CC; see the file COPYING.  If not, see
 * <http://www.gnu.org/licenses/>.
 *
 * Please send any bug reports or fixes you make to the
 * email address(es):
 *    lksctp developers <linux-sctp@vger.kernel.org>
 *
 * Written or modified by:
 *    Xin Long <lucien.xin@gmail.com>
 */

#include <net/sctp/sctp.h>
#include <net/sctp/sm.h>

static struct sctp_strreset_req *sctp_chunk_lookup_strreset_param(
			struct sctp_association *asoc, __u32 resp_seq)
{
	struct sctp_chunk *chunk = asoc->strreset_chunk;
	struct sctp_reconf_chunk *hdr;
	union sctp_params param;

	if (resp_seq != asoc->strreset_outseq || !chunk)
		return NULL;

	hdr = (struct sctp_reconf_chunk *)chunk->chunk_hdr;
	sctp_walk_params(param, hdr, params) {
		struct sctp_strreset_req *req = param.v;

		if (ntohl(req->request_seq) == resp_seq)
			return req;
	}

	return NULL;
}

struct sctp_chunk *sctp_process_strreset_outreq(
				struct sctp_association *asoc,
				union sctp_params param,
				struct sctp_ulpevent **evp)
{
	struct sctp_strreset_outreq *outreq = param.v;
	__u32 result = SCTP_STRRESET_DENIED;
	__u16 i, nums, *str_p, flags = 0;
	__u32 request_seq;

	request_seq = ntohl(outreq->request_seq);

	if (ntohl(outreq->send_reset_at_tsn) >
	    sctp_tsnmap_get_ctsn(&asoc->peer.tsn_map)) {
		result = SCTP_STRRESET_IN_PROGRESS;
		goto out;
	}

	if (request_seq > asoc->strreset_inseq) {
		result = SCTP_STRRESET_ERR_BAD_SEQNO;
		goto out;
	} else if (request_seq == asoc->strreset_inseq) {
		asoc->strreset_inseq++;
	}

	/* Check strreset_enable after inseq inc, as sender cannot tell
	 * the peer doesn't enable strreset after receiving response with
	 * result denied, as well as to keep consistent with bsd.
	 */
	if (!(asoc->strreset_enable & SCTP_ENABLE_RESET_STREAM_REQ))
		goto out;

	if (asoc->strreset_chunk) {
		struct sctp_strreset_req *inreq;
		struct sctp_transport *t;

		inreq = sctp_chunk_lookup_strreset_param(
					asoc, ntohl(outreq->response_seq));
		if (!inreq || inreq->param_hdr.type !=
					SCTP_PARAM_RESET_IN_REQUEST) {
			/* same process with outstanding isn't 0 */
			result = SCTP_STRRESET_ERR_IN_PROGRESS;
			goto out;
		}

		asoc->strreset_outstanding--;
		asoc->strreset_outseq++;

		if (!asoc->strreset_outstanding) {
			t = asoc->strreset_chunk->transport;
			if (del_timer(&t->reconf_timer))
				sctp_transport_put(t);

			sctp_chunk_put(asoc->strreset_chunk);
			asoc->strreset_chunk = NULL;
		}

		flags = SCTP_STREAM_RESET_INCOMING_SSN;
	}

	nums = (ntohs(param.p->length) - sizeof(*outreq)) / 2;
	if (nums) {
		str_p = outreq->list_of_streams;
		for (i = 0; i < nums; i++) {
			str_p[i] = ntohs(str_p[i]);
			if (str_p[i] >= asoc->streamincnt) {
				result = SCTP_STRRESET_ERR_WRONG_SSN;
				goto out;
			}
		}

		str_p = outreq->list_of_streams;
		for (i = 0; i < nums; i++, str_p++)
			asoc->streamin[*str_p].ssn = 0;
	} else {
		for (i = 0; i < asoc->streamincnt; i++)
			asoc->streamin[i].ssn = 0;
	}

	result = SCTP_STRRESET_PERFORMED;

	*evp = sctp_ulpevent_make_stream_reset_event(asoc,
		flags | SCTP_STREAM_RESET_OUTGOING_SSN, nums, str_p,
		GFP_ATOMIC);

out:
	return sctp_make_strreset_resp(asoc, result, request_seq);
}

struct sctp_chunk *sctp_process_strreset_inreq(
				struct sctp_association *asoc,
				union sctp_params param,
				struct sctp_ulpevent **evp)
{
	struct sctp_strreset_inreq *inreq = param.v;
	__u32 result = SCTP_STRRESET_DENIED;
	struct sctp_chunk *chunk = NULL;
	__u16 i, nums, *str_p;
	__u32 request_seq;

	request_seq = ntohl(inreq->request_seq);
	if (request_seq > asoc->strreset_inseq) {
		result = SCTP_STRRESET_ERR_BAD_SEQNO;
		goto out;
	} else if (request_seq == asoc->strreset_inseq) {
		asoc->strreset_inseq++;
	}

	if (!(asoc->strreset_enable & SCTP_ENABLE_RESET_STREAM_REQ))
		goto out;

	if (asoc->strreset_outstanding) {
		result = SCTP_STRRESET_ERR_IN_PROGRESS;
		goto out;
	}

	nums = (ntohs(param.p->length) - sizeof(*inreq)) / 2;
	str_p = inreq->list_of_streams;
	for (i = 0; i < nums; i++) {
		str_p[i] = ntohs(str_p[i]);
		if (str_p[i] >= asoc->streamoutcnt) {
			result = SCTP_STRRESET_ERR_WRONG_SSN;
			goto out;
		}
	}

	chunk = sctp_make_strreset_req(asoc, nums, str_p, 1, 0);
	if (!chunk)
		goto out;

	if (nums)
		for (i = 0; i < nums; i++)
			asoc->streamout[str_p[i]].state =
						   SCTP_STREAM_CLOSED;
	else
		for (i = 0; i < asoc->streamoutcnt; i++)
			asoc->streamout[i].state = SCTP_STREAM_CLOSED;

	asoc->strreset_chunk = chunk;
	asoc->strreset_outstanding = 1;
	sctp_chunk_hold(asoc->strreset_chunk);

	*evp = sctp_ulpevent_make_stream_reset_event(asoc,
		SCTP_STREAM_RESET_INCOMING_SSN, nums, str_p, GFP_ATOMIC);

out:
	if (!chunk)
		chunk =  sctp_make_strreset_resp(asoc, result, request_seq);

	return chunk;
}

struct sctp_chunk *sctp_process_strreset_tsnreq(
				struct sctp_association *asoc,
				union sctp_params param,
				struct sctp_ulpevent **evp)
{
	struct sctp_strreset_tsnreq *tsnreq = param.v;
	__u32 request_seq, initial_tsn, max_tsn_seen;
	__u32 result = SCTP_STRRESET_DENIED;
	__u16 i;

	request_seq = ntohl(tsnreq->request_seq);
	if (request_seq > asoc->strreset_inseq) {
		result = SCTP_STRRESET_ERR_BAD_SEQNO;
		goto out;
	} else if (request_seq == asoc->strreset_inseq) {
		asoc->strreset_inseq++;
	}

	if (!(asoc->strreset_enable & SCTP_ENABLE_RESET_ASSOC_REQ))
		goto out;

	if (asoc->strreset_outstanding) {
		result = SCTP_STRRESET_ERR_IN_PROGRESS;
		goto out;
	}

	/* G3: The same processing as though a SACK chunk with no gap report
	 *     and a cumulative TSN ACK of the Sender's Next TSN minus 1 were
	 *     received MUST be performed.
	 */
	max_tsn_seen = sctp_tsnmap_get_max_tsn_seen(&asoc->peer.tsn_map);
	sctp_ulpq_reasm_flushtsn(&asoc->ulpq, max_tsn_seen);
	sctp_ulpq_abort_pd(&asoc->ulpq, GFP_ATOMIC);

	/* G1: Compute an appropriate value for the Receiver's Next TSN -- the
	 *     TSN that the peer should use to send the next DATA chunk.  The
	 *     value SHOULD be the smallest TSN not acknowledged by the
	 *     receiver of the request plus 2^31.
	 */
	initial_tsn = sctp_tsnmap_get_ctsn(&asoc->peer.tsn_map) + (1 << 31);
	sctp_tsnmap_init(&asoc->peer.tsn_map, SCTP_TSN_MAP_INITIAL,
			 initial_tsn, GFP_ATOMIC);

	/* G4: The same processing as though a FWD-TSN chunk (as defined in
	 *     [RFC3758]) with all streams affected and a new cumulative TSN
	 *     ACK of the Receiver's Next TSN minus 1 were received MUST be
	 *     performed.
	 */
	sctp_outq_free(&asoc->outqueue);

	/* G2: Compute an appropriate value for the local endpoint's next TSN,
	 *     i.e., the next TSN assigned by the receiver of the SSN/TSN reset
	 *     chunk.  The value SHOULD be the highest TSN sent by the receiver
	 *     of the request plus 1.
	 */
	asoc->ctsn_ack_point = asoc->next_tsn - 1;
	asoc->adv_peer_ack_point = asoc->ctsn_ack_point;

	/* G5:  The next expected and outgoing SSNs MUST be reset to 0 for all
	 *      incoming and outgoing streams.
	 */
	for (i = 0; i < asoc->streamoutcnt; i++)
		asoc->streamout[i].ssn = 0;
	for (i = 0; i < asoc->streamincnt; i++)
		asoc->streamin[i].ssn = 0;

	result = SCTP_STRRESET_PERFORMED;

	*evp = sctp_ulpevent_make_assoc_reset_event(asoc,
		0, initial_tsn, asoc->next_tsn, GFP_ATOMIC);

out:
	return sctp_make_strreset_tsnresp(asoc, result, request_seq,
					  asoc->next_tsn, initial_tsn);
}

struct sctp_chunk *sctp_process_strreset_addstrm_out(
				struct sctp_association *asoc,
				union sctp_params param,
				struct sctp_ulpevent **evp)
{
	struct sctp_strreset_addstrm *addstrm = param.v;
	__u32 result = SCTP_STRRESET_DENIED;
	__u32 request_seq;
	__u16 nums;

	request_seq = ntohl(addstrm->request_seq);
	if (request_seq > asoc->strreset_inseq) {
		result = SCTP_STRRESET_ERR_BAD_SEQNO;
		goto out;
	} else if (request_seq == asoc->strreset_inseq) {
		asoc->strreset_inseq++;
	}

	if (!(asoc->strreset_enable & SCTP_ENABLE_CHANGE_ASSOC_REQ))
		goto out;

	if (asoc->strreset_outstanding) {
		result = SCTP_STRRESET_ERR_IN_PROGRESS;
		goto out;
	}

	nums = ntohs(addstrm->number_of_streams);
	if (!nums || nums + asoc->streamincnt > SCTP_MAX_STREAM)
		goto out;

	nums = nums + asoc->streamincnt;
	if (ksize(asoc->streamin) / sizeof(*asoc->streamin) < nums) {
		struct sctp_stream_in *streamin;

		streamin = kcalloc(nums, sizeof(*streamin), GFP_ATOMIC);
		if (!streamin)
			goto out;

		memcpy(streamin, asoc->streamin,
		       sizeof(*streamin) * asoc->streamincnt);

		kfree(asoc->streamin);
		asoc->streamin = streamin;
	}

	asoc->streamincnt = nums;

	result = SCTP_STRRESET_PERFORMED;

	*evp = sctp_ulpevent_make_stream_change_event(asoc,
		0, ntohs(addstrm->number_of_streams), 0, GFP_ATOMIC);

out:
	return sctp_make_strreset_resp(asoc, result, request_seq);
}

struct sctp_chunk *sctp_process_strreset_addstrm_in(
				struct sctp_association *asoc,
				union sctp_params param,
				struct sctp_ulpevent **evp)
{
	struct sctp_strreset_addstrm *addstrm = param.v;
	__u32 result = SCTP_STRRESET_DENIED;
	__u32 request_seq;
	__u16 i, nums;

	request_seq = ntohl(addstrm->request_seq);
	if (request_seq > asoc->strreset_inseq) {
		result = SCTP_STRRESET_ERR_BAD_SEQNO;
		goto out;
	} else if (request_seq == asoc->strreset_inseq) {
		asoc->strreset_inseq++;
	}

	if (!(asoc->strreset_enable & SCTP_ENABLE_CHANGE_ASSOC_REQ))
		goto out;

	if (asoc->strreset_outstanding) {
		result = SCTP_STRRESET_ERR_IN_PROGRESS;
		goto out;
	}

	nums = ntohs(addstrm->number_of_streams);
	if (!nums || nums + asoc->streamoutcnt > SCTP_MAX_STREAM)
		goto out;

	nums = nums + asoc->streamoutcnt;
	if (ksize(asoc->streamout) / sizeof(*asoc->streamout) < nums) {
		struct sctp_stream_out *streamout;

		streamout = kcalloc(nums, sizeof(*streamout), GFP_ATOMIC);
		if (!streamout)
			goto out;

		memcpy(streamout, asoc->streamout,
		       sizeof(*streamout) * asoc->streamoutcnt);

		kfree(asoc->streamout);
		asoc->streamout = streamout;
	}

	for (i = asoc->streamoutcnt; i < nums; i++)
		asoc->streamout[i].state = SCTP_STREAM_OPEN;

	asoc->streamoutcnt = nums;

	result = SCTP_STRRESET_PERFORMED;

	*evp = sctp_ulpevent_make_stream_change_event(asoc,
		0, 0, ntohs(addstrm->number_of_streams), GFP_ATOMIC);

out:
	return sctp_make_strreset_resp(asoc, result, request_seq);
}
