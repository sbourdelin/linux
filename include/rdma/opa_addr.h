/*
 * Copyright (c) 2016 Intel Corporation.  All rights reserved.
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

#if !defined(OPA_ADDR_H)
#define OPA_ADDR_H

#include <rdma/ib_verbs.h>

#define OPA_TO_IB_UCAST_LID(x)	(((x) >= be16_to_cpu(IB_MULTICAST_LID_BASE)) \
				 ? 0 : x)
#define	OPA_SPECIAL_OUI		(0x00066AULL)
#define OPA_MAKE_ID(x)          (cpu_to_be64(OPA_SPECIAL_OUI << 40 | (x)))

/**
 * ib_is_opa_gid: Returns true if the top 24 bits of the gid
 * contains the OPA_STL_OUI identifier. This identifies that
 * the provided gid is a special purpose GID meant to carry
 * extended LID information.
 *
 * @gid: The Global identifier
 */
static inline bool ib_is_opa_gid(union ib_gid *gid)
{
	return ((be64_to_cpu(gid->global.interface_id) >> 40) ==
		OPA_SPECIAL_OUI);
}

/**
 * opa_get_lid_from_gid: Returns the last 32 bits of the gid.
 * OPA devices use one of the gids in the gid table to also
 * store the lid.
 *
 * @gid: The Global identifier
 */
static inline u32 opa_get_lid_from_gid(union ib_gid *gid)
{
	return be64_to_cpu(gid->global.interface_id) & 0xFFFFFFFF;
}
#endif /* OPA_ADDR_H */
