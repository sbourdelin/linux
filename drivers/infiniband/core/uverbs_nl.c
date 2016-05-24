/*
 * Copyright (c) 2016 Mellanox Technologies, LTD. All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
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

#include <net/netlink.h>
#include <rdma/ib_verbs.h>

static struct nlattr __user *ib_uverbs_nla_reserve(struct ib_udata *udata,
						   int attrtype, int attrlen)
{
	struct nlattr __user *nla;
	unsigned int i;

	if (nla_total_size(attrlen) > udata->outlen)
		return ERR_PTR(-ENOSPC);

	nla = (struct nlattr __user *)udata->outbuf;
	udata->outbuf += nla_total_size(attrlen);
	udata->outlen -= nla_total_size(attrlen);
	put_user(attrtype, &nla->nla_type);
	put_user(nla_attr_size(attrlen), &nla->nla_len);

	/* TODO: optimize */
	for (i = 0; i < nla_padlen(attrlen); i++)
		put_user(0,
			 (unsigned char __user *)nla + nla_attr_size(attrlen));

	return nla;
}

struct nlattr __user *ib_uverbs_nla_put(struct ib_udata *udata,
					int attrtype, int attrlen,
					const void *data)
{
	struct nlattr __user *nla;
	int ret;

	nla = ib_uverbs_nla_reserve(udata, attrtype, attrlen);
	if (IS_ERR(nla))
		return nla;

	ret = copy_to_user(nla_data(nla), data, attrlen);
	if (ret) {
		udata->outbuf -= nla_total_size(attrlen);
		udata->outlen += nla_total_size(attrlen);
		return ERR_PTR(ret);
	}

	return nla;
}

void ib_uverbs_nla_nest_end(struct ib_udata *udata,  struct nlattr __user *nla)
{
	put_user(udata->outbuf - (void __user *)nla, &nla->nla_len);
}

struct nlattr __user *ib_uverbs_nla_nest_start(struct ib_udata *udata,
					       uint16_t type)
{
	struct nlattr __user *nla = ib_uverbs_nla_put(udata, type, 0, NULL);

	return nla;
}

