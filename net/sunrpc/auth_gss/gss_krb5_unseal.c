/*
 *  linux/net/sunrpc/gss_krb5_unseal.c
 *
 *  Adapted from MIT Kerberos 5-1.2.1 lib/gssapi/krb5/k5unseal.c
 *
 *  Copyright (c) 2000-2008 The Regents of the University of Michigan.
 *  All rights reserved.
 *
 *  Andy Adamson   <andros@umich.edu>
 */

/*
 * Copyright 1993 by OpenVision Technologies, Inc.
 *
 * Permission to use, copy, modify, distribute, and sell this software
 * and its documentation for any purpose is hereby granted without fee,
 * provided that the above copyright notice appears in all copies and
 * that both that copyright notice and this permission notice appear in
 * supporting documentation, and that the name of OpenVision not be used
 * in advertising or publicity pertaining to distribution of the software
 * without specific, written prior permission. OpenVision makes no
 * representations about the suitability of this software for any
 * purpose.  It is provided "as is" without express or implied warranty.
 *
 * OPENVISION DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO
 * EVENT SHALL OPENVISION BE LIABLE FOR ANY SPECIAL, INDIRECT OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF
 * USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR
 * OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

/*
 * Copyright (C) 1998 by the FundsXpress, INC.
 *
 * All rights reserved.
 *
 * Export of this software from the United States of America may require
 * a specific license from the United States Government.  It is the
 * responsibility of any person or organization contemplating export to
 * obtain such a license before exporting.
 *
 * WITHIN THAT CONSTRAINT, permission to use, copy, modify, and
 * distribute this software and its documentation for any purpose and
 * without fee is hereby granted, provided that the above copyright
 * notice appear in all copies and that both that copyright notice and
 * this permission notice appear in supporting documentation, and that
 * the name of FundsXpress. not be used in advertising or publicity pertaining
 * to distribution of the software without specific, written prior
 * permission.  FundsXpress makes no representations about the suitability of
 * this software for any purpose.  It is provided "as is" without express
 * or implied warranty.
 *
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND WITHOUT ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, WITHOUT LIMITATION, THE IMPLIED
 * WARRANTIES OF MERCHANTIBILITY AND FITNESS FOR A PARTICULAR PURPOSE.
 */

#include <linux/types.h>
#include <linux/jiffies.h>
#include <linux/sunrpc/gss_krb5.h>
#include <linux/crypto.h>

#if IS_ENABLED(CONFIG_SUNRPC_DEBUG)
# define RPCDBG_FACILITY        RPCDBG_AUTH
#endif


/* read_token is a mic token, and message_buffer is the data that the mic was
 * supposedly taken over. */

static u32
gss_verify_mic_v2(struct krb5_ctx *ctx,
		struct xdr_buf *message_buffer, struct xdr_netobj *read_token)
{
	char cksumdata[GSS_KRB5_MAX_CKSUM_LEN];
	struct xdr_netobj cksumobj = {.len = sizeof(cksumdata),
				      .data = cksumdata};
	s32 now;
	u8 *ptr = read_token->data;
	u8 *cksumkey;
	u8 flags;
	int i;
	unsigned int cksum_usage;
	__be16 be16_ptr;

	dprintk("RPC:       %s\n", __func__);

	memcpy(&be16_ptr, (char *) ptr, 2);
	if (be16_to_cpu(be16_ptr) != KG2_TOK_MIC)
		return GSS_S_DEFECTIVE_TOKEN;

	flags = ptr[2];
	if ((!ctx->initiate && (flags & KG2_TOKEN_FLAG_SENTBYACCEPTOR)) ||
	    (ctx->initiate && !(flags & KG2_TOKEN_FLAG_SENTBYACCEPTOR)))
		return GSS_S_BAD_SIG;

	if (flags & KG2_TOKEN_FLAG_SEALED) {
		dprintk("%s: token has unexpected sealed flag\n", __func__);
		return GSS_S_FAILURE;
	}

	for (i = 3; i < 8; i++)
		if (ptr[i] != 0xff)
			return GSS_S_DEFECTIVE_TOKEN;

	if (ctx->initiate) {
		cksumkey = ctx->acceptor_sign;
		cksum_usage = KG_USAGE_ACCEPTOR_SIGN;
	} else {
		cksumkey = ctx->initiator_sign;
		cksum_usage = KG_USAGE_INITIATOR_SIGN;
	}

	if (make_checksum_v2(ctx, ptr, GSS_KRB5_TOK_HDR_LEN, message_buffer, 0,
			     cksumkey, cksum_usage, &cksumobj))
		return GSS_S_FAILURE;

	if (memcmp(cksumobj.data, ptr + GSS_KRB5_TOK_HDR_LEN,
				ctx->gk5e->cksumlength))
		return GSS_S_BAD_SIG;

	/* it got through unscathed.  Make sure the context is unexpired */
	now = get_seconds();
	if (now > ctx->endtime)
		return GSS_S_CONTEXT_EXPIRED;

	/*
	 * NOTE: the sequence number at ptr + 8 is skipped, rpcsec_gss
	 * doesn't want it checked; see page 6 of rfc 2203.
	 */

	return GSS_S_COMPLETE;
}

u32
gss_verify_mic_kerberos(struct gss_ctx *gss_ctx,
			struct xdr_buf *message_buffer,
			struct xdr_netobj *read_token)
{
	struct krb5_ctx *ctx = gss_ctx->internal_ctx_id;

	switch (ctx->enctype) {
	case ENCTYPE_AES128_CTS_HMAC_SHA1_96:
	case ENCTYPE_AES256_CTS_HMAC_SHA1_96:
		return gss_verify_mic_v2(ctx, message_buffer, read_token);
	default:
		return GSS_S_FAILURE;
	}
}
