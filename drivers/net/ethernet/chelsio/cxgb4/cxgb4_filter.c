/*
 * This file is part of the Chelsio T4 Ethernet driver for Linux.
 *
 * Copyright (c) 2003-2016 Chelsio Communications, Inc. All rights reserved.
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

#include "cxgb4.h"
#include "t4_regs.h"
#include "t4_values.h"
#include "l2t.h"
#include "t4fw_api.h"
#include "cxgb4_filter.h"

/* Validate filter spec against configuration done on the card.
 */
static int validate_filter(struct net_device *dev,
			   struct ch_filter_specification *fs)
{
	struct adapter *adapter = netdev2adap(dev);
	u32 fconf, iconf;

	/* Check for unconfigured fields being used. */
	fconf = adapter->params.tp.vlan_pri_map;
	iconf = adapter->params.tp.ingress_config;

	#define S(_field) \
		(fs->val._field || fs->mask._field)
	#define U(_mask, _field) \
		(!(fconf & (_mask)) && S(_field))

	if (U(FCOE_F, fcoe) || U(PORT_F, iport) || U(TOS_F, tos) ||
	    U(ETHERTYPE_F, ethtype) || U(MACMATCH_F, macidx) ||
	    U(MPSHITTYPE_F, matchtype) || U(FRAGMENTATION_F, frag) ||
	    U(PROTOCOL_F, proto) ||
	    U(VNIC_ID_F, pfvf_vld) ||
	    U(VNIC_ID_F, ovlan_vld) ||
	    U(VLAN_F, ivlan_vld))
		return -EOPNOTSUPP;

	/* T4 inconveniently uses the same FT_VNIC_ID_W bits for both the Outer
	 * VLAN Tag and PF/VF/VFvld fields based on VNIC_F being set
	 * in TP_INGRESS_CONFIG.  Hense the somewhat crazy checks
	 * below.  Additionally, since the T4 firmware interface also
	 * carries that overlap, we need to translate any PF/VF
	 * specification into that internal format below.
	 */
	if (S(pfvf_vld) && S(ovlan_vld))
		return -EOPNOTSUPP;
	if ((S(pfvf_vld) && !(iconf & VNIC_F)) ||
	    (S(ovlan_vld) && (iconf & VNIC_F)))
		return -EOPNOTSUPP;
	if (fs->val.pf > 0x7 || fs->val.vf > 0x7f)
		return -ERANGE;
	fs->mask.pf &= 0x7;
	fs->mask.vf &= 0x7f;

	#undef S
	#undef U

	/* If the user is requesting that the filter action loop
	 * matching packets back out one of our ports, make sure that
	 * the egress port is in range.
	 */
	if (fs->action == FILTER_SWITCH &&
	    fs->eport >= adapter->params.nports)
		return -ERANGE;

	/* Don't allow various trivially obvious bogus out-of-range values... */
	if (fs->val.iport >= adapter->params.nports)
		return -ERANGE;

	/* T4 doesn't support removing VLAN Tags for loop back filters. */
	if (is_t4(adapter->params.chip) &&
	    fs->action == FILTER_SWITCH &&
	    (fs->newvlan == VLAN_REMOVE ||
	     fs->newvlan == VLAN_REWRITE))
		return -EOPNOTSUPP;

	return 0;
}

static unsigned int get_filter_steerq(struct net_device *dev,
				      struct ch_filter_specification *fs)
{
	struct adapter *adapter = netdev2adap(dev);
	unsigned int iq;

	/* If the user has requested steering matching Ingress Packets
	 * to a specific Queue Set, we need to make sure it's in range
	 * for the port and map that into the Absolute Queue ID of the
	 * Queue Set's Response Queue.
	 */
	if (!fs->dirsteer) {
		if (fs->iq)
			return -EINVAL;
		iq = 0;
	} else {
		struct port_info *pi = netdev_priv(dev);

		/* If the iq id is greater than the number of qsets,
		 * then assume it is an absolute qid.
		 */
		if (fs->iq < pi->nqsets)
			iq = adapter->sge.ethrxq[pi->first_qset +
						 fs->iq].rspq.abs_id;
		else
			iq = fs->iq;
	}

	return iq;
}

static int cxgb4_set_ftid(struct tid_info *t, int fidx, int family)
{
	spin_lock_bh(&t->ftid_lock);

	if (test_bit(fidx, t->ftid_bmap)) {
		spin_unlock_bh(&t->ftid_lock);
		return -EBUSY;
	}

	if (family == PF_INET)
		__set_bit(fidx, t->ftid_bmap);
	else
		bitmap_allocate_region(t->ftid_bmap, fidx, 2);

	spin_unlock_bh(&t->ftid_lock);
	return 0;
}

static void cxgb4_clear_ftid(struct tid_info *t, int fidx, int family)
{
	spin_lock_bh(&t->ftid_lock);
	if (family == PF_INET)
		__clear_bit(fidx, t->ftid_bmap);
	else
		bitmap_release_region(t->ftid_bmap, fidx, 2);
	spin_unlock_bh(&t->ftid_lock);
}

/* Delete the filter at a specified index.
 */
static int del_filter_wr(struct adapter *adapter, int fidx)
{
	struct filter_entry *f = &adapter->tids.ftid_tab[fidx];
	struct sk_buff *skb;
	struct fw_filter_wr *fwr;
	unsigned int len;

	len = sizeof(*fwr);

	skb = alloc_skb(len, GFP_KERNEL);
	if (!skb)
		return -ENOMEM;

	fwr = (struct fw_filter_wr *)__skb_put(skb, len);
	t4_mk_filtdelwr(f->tid, fwr, adapter->sge.fw_evtq.abs_id);

	/* Mark the filter as "pending" and ship off the Filter Work Request.
	 * When we get the Work Request Reply we'll clear the pending status.
	 */
	f->pending = 1;
	t4_mgmt_tx(adapter, skb);
	return 0;
}

/* Send a Work Request to write the filter at a specified index.  We construct
 * a Firmware Filter Work Request to have the work done and put the indicated
 * filter into "pending" mode which will prevent any further actions against
 * it till we get a reply from the firmware on the completion status of the
 * request.
 */
int set_filter_wr(struct adapter *adapter, int fidx)
{
	struct filter_entry *f = &adapter->tids.ftid_tab[fidx];
	struct sk_buff *skb;
	struct fw_filter_wr *fwr;

	skb = alloc_skb(sizeof(*fwr), GFP_KERNEL);
	if (!skb)
		return -ENOMEM;

	/* If the new filter requires loopback Destination MAC and/or VLAN
	 * rewriting then we need to allocate a Layer 2 Table (L2T) entry for
	 * the filter.
	 */
	if (f->fs.newdmac || f->fs.newvlan) {
		/* allocate L2T entry for new filter */
		f->l2t = t4_l2t_alloc_switching(adapter, f->fs.vlan,
						f->fs.eport, f->fs.dmac);
		if (!f->l2t) {
			kfree_skb(skb);
			return -ENOMEM;
		}
	}

	fwr = (struct fw_filter_wr *)__skb_put(skb, sizeof(*fwr));
	memset(fwr, 0, sizeof(*fwr));

	/* It would be nice to put most of the following in t4_hw.c but most
	 * of the work is translating the cxgbtool ch_filter_specification
	 * into the Work Request and the definition of that structure is
	 * currently in cxgbtool.h which isn't appropriate to pull into the
	 * common code.  We may eventually try to come up with a more neutral
	 * filter specification structure but for now it's easiest to simply
	 * put this fairly direct code in line ...
	 */
	fwr->op_pkd = htonl(FW_WR_OP_V(FW_FILTER_WR));
	fwr->len16_pkd = htonl(FW_WR_LEN16_V(sizeof(*fwr) / 16));
	fwr->tid_to_iq =
		htonl(FW_FILTER_WR_TID_V(f->tid) |
		      FW_FILTER_WR_RQTYPE_V(f->fs.type) |
		      FW_FILTER_WR_NOREPLY_V(0) |
		      FW_FILTER_WR_IQ_V(f->fs.iq));
	fwr->del_filter_to_l2tix =
		htonl(FW_FILTER_WR_RPTTID_V(f->fs.rpttid) |
		      FW_FILTER_WR_DROP_V(f->fs.action == FILTER_DROP) |
		      FW_FILTER_WR_DIRSTEER_V(f->fs.dirsteer) |
		      FW_FILTER_WR_MASKHASH_V(f->fs.maskhash) |
		      FW_FILTER_WR_DIRSTEERHASH_V(f->fs.dirsteerhash) |
		      FW_FILTER_WR_LPBK_V(f->fs.action == FILTER_SWITCH) |
		      FW_FILTER_WR_DMAC_V(f->fs.newdmac) |
		      FW_FILTER_WR_SMAC_V(f->fs.newsmac) |
		      FW_FILTER_WR_INSVLAN_V(f->fs.newvlan == VLAN_INSERT ||
					     f->fs.newvlan == VLAN_REWRITE) |
		      FW_FILTER_WR_RMVLAN_V(f->fs.newvlan == VLAN_REMOVE ||
					    f->fs.newvlan == VLAN_REWRITE) |
		      FW_FILTER_WR_HITCNTS_V(f->fs.hitcnts) |
		      FW_FILTER_WR_TXCHAN_V(f->fs.eport) |
		      FW_FILTER_WR_PRIO_V(f->fs.prio) |
		      FW_FILTER_WR_L2TIX_V(f->l2t ? f->l2t->idx : 0));
	fwr->ethtype = htons(f->fs.val.ethtype);
	fwr->ethtypem = htons(f->fs.mask.ethtype);
	fwr->frag_to_ovlan_vldm =
		(FW_FILTER_WR_FRAG_V(f->fs.val.frag) |
		 FW_FILTER_WR_FRAGM_V(f->fs.mask.frag) |
		 FW_FILTER_WR_IVLAN_VLD_V(f->fs.val.ivlan_vld) |
		 FW_FILTER_WR_OVLAN_VLD_V(f->fs.val.ovlan_vld) |
		 FW_FILTER_WR_IVLAN_VLDM_V(f->fs.mask.ivlan_vld) |
		 FW_FILTER_WR_OVLAN_VLDM_V(f->fs.mask.ovlan_vld));
	fwr->smac_sel = 0;
	fwr->rx_chan_rx_rpl_iq =
		htons(FW_FILTER_WR_RX_CHAN_V(0) |
		      FW_FILTER_WR_RX_RPL_IQ_V(adapter->sge.fw_evtq.abs_id));
	fwr->maci_to_matchtypem =
		htonl(FW_FILTER_WR_MACI_V(f->fs.val.macidx) |
		      FW_FILTER_WR_MACIM_V(f->fs.mask.macidx) |
		      FW_FILTER_WR_FCOE_V(f->fs.val.fcoe) |
		      FW_FILTER_WR_FCOEM_V(f->fs.mask.fcoe) |
		      FW_FILTER_WR_PORT_V(f->fs.val.iport) |
		      FW_FILTER_WR_PORTM_V(f->fs.mask.iport) |
		      FW_FILTER_WR_MATCHTYPE_V(f->fs.val.matchtype) |
		      FW_FILTER_WR_MATCHTYPEM_V(f->fs.mask.matchtype));
	fwr->ptcl = f->fs.val.proto;
	fwr->ptclm = f->fs.mask.proto;
	fwr->ttyp = f->fs.val.tos;
	fwr->ttypm = f->fs.mask.tos;
	fwr->ivlan = htons(f->fs.val.ivlan);
	fwr->ivlanm = htons(f->fs.mask.ivlan);
	fwr->ovlan = htons(f->fs.val.ovlan);
	fwr->ovlanm = htons(f->fs.mask.ovlan);
	memcpy(fwr->lip, f->fs.val.lip, sizeof(fwr->lip));
	memcpy(fwr->lipm, f->fs.mask.lip, sizeof(fwr->lipm));
	memcpy(fwr->fip, f->fs.val.fip, sizeof(fwr->fip));
	memcpy(fwr->fipm, f->fs.mask.fip, sizeof(fwr->fipm));
	fwr->lp = htons(f->fs.val.lport);
	fwr->lpm = htons(f->fs.mask.lport);
	fwr->fp = htons(f->fs.val.fport);
	fwr->fpm = htons(f->fs.mask.fport);
	if (f->fs.newsmac)
		memcpy(fwr->sma, f->fs.smac, sizeof(fwr->sma));

	/* Mark the filter as "pending" and ship off the Filter Work Request.
	 * When we get the Work Request Reply we'll clear the pending status.
	 */
	f->pending = 1;
	set_wr_txq(skb, CPL_PRIORITY_CONTROL, f->fs.val.iport & 0x3);
	t4_ofld_send(adapter, skb);
	return 0;
}

/* Return an error number if the indicated filter isn't writable ...
 */
int writable_filter(struct filter_entry *f)
{
	if (f->locked)
		return -EPERM;
	if (f->pending)
		return -EBUSY;

	return 0;
}

/* Delete the filter at the specified index (if valid).  The checks for all
 * the common problems with doing this like the filter being locked, currently
 * pending in another operation, etc.
 */
int delete_filter(struct adapter *adapter, unsigned int fidx)
{
	struct filter_entry *f;
	int ret;

	if (fidx >= adapter->tids.nftids + adapter->tids.nsftids)
		return -EINVAL;

	f = &adapter->tids.ftid_tab[fidx];
	ret = writable_filter(f);
	if (ret)
		return ret;
	if (f->valid)
		return del_filter_wr(adapter, fidx);

	return 0;
}

/* Clear a filter and release any of its resources that we own.  This also
 * clears the filter's "pending" status.
 */
void clear_filter(struct adapter *adap, struct filter_entry *f)
{
	/* If the new or old filter have loopback rewriteing rules then we'll
	 * need to free any existing Layer Two Table (L2T) entries of the old
	 * filter rule.  The firmware will handle freeing up any Source MAC
	 * Table (SMT) entries used for rewriting Source MAC Addresses in
	 * loopback rules.
	 */
	if (f->l2t)
		cxgb4_l2t_release(f->l2t);

	/* The zeroing of the filter rule below clears the filter valid,
	 * pending, locked flags, l2t pointer, etc. so it's all we need for
	 * this operation.
	 */
	memset(f, 0, sizeof(*f));
}

void clear_all_filters(struct adapter *adapter)
{
	unsigned int i;

	if (adapter->tids.ftid_tab) {
		struct filter_entry *f = &adapter->tids.ftid_tab[0];
		unsigned int max_ftid = adapter->tids.nftids +
					adapter->tids.nsftids;

		for (i = 0; i < max_ftid; i++, f++)
			if (f->valid || f->pending)
				clear_filter(adapter, f);
	}
}

/* Fill up default masks for set match fields. */
static void fill_default_mask(struct ch_filter_specification *fs)
{
	unsigned int i;
	unsigned int lip = 0, lip_mask = 0;
	unsigned int fip = 0, fip_mask = 0;

	if (fs->val.iport && !fs->mask.iport)
		fs->mask.iport |= ~0;
	if (fs->val.fcoe && !fs->mask.fcoe)
		fs->mask.fcoe |= ~0;
	if (fs->val.matchtype && !fs->mask.matchtype)
		fs->mask.matchtype |= ~0;
	if (fs->val.macidx && !fs->mask.macidx)
		fs->mask.macidx |= ~0;
	if (fs->val.ethtype && !fs->mask.ethtype)
		fs->mask.ethtype |= ~0;
	if (fs->val.ivlan && !fs->mask.ivlan)
		fs->mask.ivlan |= ~0;
	if (fs->val.ovlan && !fs->mask.ovlan)
		fs->mask.ovlan |= ~0;
	if (fs->val.frag && !fs->mask.frag)
		fs->mask.frag |= ~0;
	if (fs->val.tos && !fs->mask.tos)
		fs->mask.tos |= ~0;
	if (fs->val.proto && !fs->mask.proto)
		fs->mask.proto |= ~0;

	for (i = 0; i < ARRAY_SIZE(fs->val.lip); i++) {
		lip |= fs->val.lip[i];
		lip_mask |= fs->mask.lip[i];
		fip |= fs->val.fip[i];
		fip_mask |= fs->mask.fip[i];
	}

	if (lip && !lip_mask)
		memset(fs->mask.lip, ~0, sizeof(fs->mask.lip));

	if (fip && !fip_mask)
		memset(fs->mask.fip, ~0, sizeof(fs->mask.lip));

	if (fs->val.lport && !fs->mask.lport)
		fs->mask.lport = ~0;
	if (fs->val.fport && !fs->mask.fport)
		fs->mask.fport = ~0;
}

/* Check a Chelsio Filter Request for validity, convert it into our internal
 * format and send it to the hardware.  Return 0 on success, an error number
 * otherwise.  We attach any provided filter operation context to the internal
 * filter specification in order to facilitate signaling completion of the
 * operation.
 */
int cxgb4_set_filter(struct net_device *dev, int filter_id,
		     struct ch_filter_specification *fs,
		     struct filter_ctx *ctx)
{
	struct adapter *adapter = netdev2adap(dev);
	struct filter_entry *f;
	u32 iconf;
	unsigned int fidx, iq;
	unsigned int max_fidx;
	int ret;

	max_fidx = adapter->tids.nftids;
	if ((filter_id != (max_fidx + adapter->tids.nsftids - 1)) &&
	    (filter_id >= max_fidx))
		return -E2BIG;

	fill_default_mask(fs);

	ret = validate_filter(dev, fs);
	if (ret)
		return ret;

	iq = get_filter_steerq(dev, fs);
	if (iq < 0)
		return iq;

	/* IPv6 filters occupy four slots and must be aligned on
	 * four-slot boundaries.  IPv4 filters only occupy a single
	 * slot and have no alignment requirements but writing a new
	 * IPv4 filter into the middle of an existing IPv6 filter
	 * requires clearing the old IPv6 filter and hence we prevent
	 * insertion.
	 */
	if (fs->type == 0) { /* IPv4 */
		/* If our IPv4 filter isn't being written to a
		 * multiple of four filter index and there's an IPv6
		 * filter at the multiple of 4 base slot, then we
		 * prevent insertion.
		 */
		fidx = filter_id & ~0x3;
		if (fidx != filter_id &&
		    adapter->tids.ftid_tab[fidx].fs.type) {
			f = &adapter->tids.ftid_tab[fidx];
			if (f->valid) {
				dev_err(adapter->pdev_dev,
					"Invalid location. IPv6 requires 4 slots and is occupying slots %u to %u\n",
					fidx, fidx + 3);
				return -EINVAL;
			}
		}
	} else { /* IPv6 */
		/* Ensure that the IPv6 filter is aligned on a
		 * multiple of 4 boundary.
		 */
		if (filter_id & 0x3) {
			dev_err(adapter->pdev_dev,
				"Invalid location. IPv6 must be aligned on a 4-slot boundary\n");
			return -EINVAL;
		}

		/* Check all except the base overlapping IPv4 filter slots. */
		for (fidx = filter_id + 1; fidx < filter_id + 4; fidx++) {
			f = &adapter->tids.ftid_tab[fidx];
			if (f->valid) {
				dev_err(adapter->pdev_dev,
					"Invalid location.  IPv6 requires 4 slots and an IPv4 filter exists at %u\n",
					fidx);
				return -EINVAL;
			}
		}
	}

	/* Check to make sure that provided filter index is not
	 * already in use by someone else
	 */
	f = &adapter->tids.ftid_tab[filter_id];
	if (f->valid)
		return -EBUSY;

	fidx = filter_id + adapter->tids.ftid_base;
	ret = cxgb4_set_ftid(&adapter->tids, filter_id,
			     fs->type ? PF_INET6 : PF_INET);
	if (ret)
		return ret;

	/* Check to make sure the filter requested is writable ... */
	ret = writable_filter(f);
	if (ret) {
		/* Clear the bits we have set above */
		cxgb4_clear_ftid(&adapter->tids, filter_id,
				 fs->type ? PF_INET6 : PF_INET);
		return ret;
	}

	/* Clear out any old resources being used by the filter before
	 * we start constructing the new filter.
	 */
	if (f->valid)
		clear_filter(adapter, f);

	/* Convert the filter specification into our internal format.
	 * We copy the PF/VF specification into the Outer VLAN field
	 * here so the rest of the code -- including the interface to
	 * the firmware -- doesn't have to constantly do these checks.
	 */
	f->fs = *fs;
	f->fs.iq = iq;
	f->dev = dev;

	iconf = adapter->params.tp.ingress_config;
	if (iconf & VNIC_F) {
		f->fs.val.ovlan = (fs->val.pf << 13) | fs->val.vf;
		f->fs.mask.ovlan = (fs->mask.pf << 13) | fs->mask.vf;
		f->fs.val.ovlan_vld = fs->val.pfvf_vld;
		f->fs.mask.ovlan_vld = fs->mask.pfvf_vld;
	}

	/* Attempt to set the filter.  If we don't succeed, we clear
	 * it and return the failure.
	 */
	f->ctx = ctx;
	f->tid = fidx; /* Save the actual tid */
	ret = set_filter_wr(adapter, filter_id);
	if (ret) {
		cxgb4_clear_ftid(&adapter->tids, filter_id,
				 fs->type ? PF_INET6 : PF_INET);
		clear_filter(adapter, f);
	}

	return ret;
}

/* Check a delete filter request for validity and send it to the hardware.
 * Return 0 on success, an error number otherwise.  We attach any provided
 * filter operation context to the internal filter specification in order to
 * facilitate signaling completion of the operation.
 */
int cxgb4_del_filter(struct net_device *dev, int filter_id,
		     struct filter_ctx *ctx)
{
	struct adapter *adapter = netdev2adap(dev);
	struct filter_entry *f;
	unsigned int max_fidx;
	int ret = 0;

	max_fidx = adapter->tids.nftids;
	if ((filter_id != (max_fidx + adapter->tids.nsftids - 1)) &&
	    (filter_id >= max_fidx))
		return -E2BIG;

	f = &adapter->tids.ftid_tab[filter_id];
	ret = writable_filter(f);
	if (ret)
		return ret;

	if (f->valid) {
		f->ctx = ctx;
		cxgb4_clear_ftid(&adapter->tids, filter_id,
				 f->fs.type ? PF_INET6 : PF_INET);
		return del_filter_wr(adapter, filter_id);
	}

	/* If the caller has passed in a Completion Context then we need to
	 * mark it as a successful completion so they don't stall waiting
	 * for it.
	 */
	if (ctx) {
		ctx->result = 0;
		complete(&ctx->completion);
	}
	return ret;
}

/* Handle a filter write/deletion reply.
 */
void filter_rpl(struct adapter *adap, const struct cpl_set_tcb_rpl *rpl)
{
	unsigned int tid = GET_TID(rpl);
	struct filter_entry *f = NULL;
	unsigned int max_fidx;
	int idx;

	max_fidx = adap->tids.nftids + adap->tids.nsftids;
	/* Get the corresponding filter entry for this tid */
	if (adap->tids.ftid_tab) {
		/* Check this in normal filter region */
		idx = tid - adap->tids.ftid_base;
		if (idx >= max_fidx)
			return;
		f = &adap->tids.ftid_tab[idx];
		if (f->tid != tid)
			return;
	}

	/* We found the filter entry for this tid */
	if (f) {
		unsigned int ret = TCB_COOKIE_G(rpl->cookie);
		struct filter_ctx *ctx;

		/* Pull off any filter operation context attached to the
		 * filter.
		 */
		ctx = f->ctx;
		f->ctx = NULL;

		if (ret == FW_FILTER_WR_FLT_DELETED) {
			/* Clear the filter when we get confirmation from the
			 * hardware that the filter has been deleted.
			 */
			clear_filter(adap, f);
			if (ctx)
				ctx->result = 0;
		} else if (ret == FW_FILTER_WR_SMT_TBL_FULL) {
			dev_err(adap->pdev_dev, "filter %u setup failed due to full SMT\n",
				idx);
			clear_filter(adap, f);
			if (ctx)
				ctx->result = -ENOMEM;
		} else if (ret == FW_FILTER_WR_FLT_ADDED) {
			f->smtidx = (be64_to_cpu(rpl->oldval) >> 24) & 0xff;
			f->pending = 0;  /* asynchronous setup completed */
			f->valid = 1;
			if (ctx) {
				ctx->result = 0;
				ctx->tid = idx;
			}
		} else {
			/* Something went wrong.  Issue a warning about the
			 * problem and clear everything out.
			 */
			dev_err(adap->pdev_dev, "filter %u setup failed with error %u\n",
				idx, ret);
			clear_filter(adap, f);
			if (ctx)
				ctx->result = -EINVAL;
		}
		if (ctx)
			complete(&ctx->completion);
	}
}

/* Retrieve the packet count for the specified filter. */
int cxgb4_get_filter_count(struct adapter *adapter, unsigned int fidx,
			   u64 *c, int hash, bool get_byte)
{
	struct filter_entry *f;
	unsigned int tcb_base, tcbaddr;
	unsigned int max_ftids;
	int ret;

	tcb_base = t4_read_reg(adapter, TP_CMM_TCB_BASE_A);
	max_ftids = adapter->tids.nftids;
	if ((fidx != (max_ftids + adapter->tids.nsftids - 1)) &&
	    (fidx >= max_ftids))
		return -E2BIG;

	f = &adapter->tids.ftid_tab[fidx];
	if (!f->valid)
		return -EINVAL;

	tcbaddr = tcb_base + f->tid * TCB_SIZE;

	if (is_t4(adapter->params.chip)) {
		/* For T4, the Filter Packet Hit Count is maintained as a
		 * 64-bit Big Endian value in the TCB fields
		 * {t_rtt_ts_recent_age, t_rtseq_recent} ... The format in
		 * memory is swizzled/mapped in a manner such that instead
		 * of having this 64-bit counter show up at offset 24
		 * ((TCB_T_RTT_TS_RECENT_AGE_W == 6) * sizeof(u32)), it
		 * actually shows up at offset 16. Hence the constant "4"
		 * below instead of TCB_T_RTT_TS_RECENT_AGE_W.
		 */
		if (get_byte) {
			unsigned int word_offset = 4;
			__be64 be64_byte_count;

			spin_lock(&adapter->win0_lock);
			ret = t4_memory_rw(adapter, MEMWIN_NIC, MEM_EDC0,
					   tcbaddr +
					   (word_offset * sizeof(__be32)),
					   sizeof(be64_byte_count),
					   &be64_byte_count,
					   T4_MEMORY_READ);
			spin_unlock(&adapter->win0_lock);
			if (ret < 0)
				return ret;
			*c = be64_to_cpu(be64_byte_count);
		} else {
			unsigned int word_offset = 4;
			__be64 be64_count;

			spin_lock(&adapter->win0_lock);
			ret = t4_memory_rw(adapter, MEMWIN_NIC, MEM_EDC0,
					   tcbaddr +
					   (word_offset * sizeof(__be32)),
					   sizeof(be64_count),
					   (__be32 *)&be64_count,
					   T4_MEMORY_READ);
			spin_unlock(&adapter->win0_lock);
			if (ret < 0)
				return ret;
			*c = be64_to_cpu(be64_count);
		}
	} else {
		/* For T5, the Filter Packet Hit Count is maintained as a
		 * 32-bit Big Endian value in the TCB field {timestamp}.
		 * Instead of the filter hit count showing up at offset 20
		 * ((TCB_TIMESTAMP_W == 5) * sizeof(u32)), it actually shows
		 * up at offset 24.  Hence the constant "6" below.
		 */
		if (get_byte) {
			unsigned int word_offset = 4;
			__be64 be64_byte_count;

			spin_lock(&adapter->win0_lock);
			ret = t4_memory_rw(adapter, MEMWIN_NIC, MEM_EDC0,
					   tcbaddr +
					   (word_offset * sizeof(__be32)),
					   sizeof(be64_byte_count),
					   &be64_byte_count,
					   T4_MEMORY_READ);
			spin_unlock(&adapter->win0_lock);
			if (ret < 0)
				return ret;
			*c = be64_to_cpu(be64_byte_count);
		} else {
			unsigned int word_offset = 6;
			__be32 be32_count;

			spin_lock(&adapter->win0_lock);
			ret = t4_memory_rw(adapter, MEMWIN_NIC, MEM_EDC0,
					   tcbaddr +
					   (word_offset * sizeof(__be32)),
					   sizeof(be32_count), &be32_count,
					   T4_MEMORY_READ);
			spin_unlock(&adapter->win0_lock);
			if (ret < 0)
				return ret;
			*c = (u64)be32_to_cpu(be32_count);
		}
	}

	return 0;
}

/* Filter Table. */
static void filters_show_ipaddr(struct seq_file *seq,
				int type, u8 *addr, u8 *addrm)
{
	int noctets, octet;

	seq_puts(seq, " ");
	if (type == 0) {
		noctets = 4;
		seq_printf(seq, "%48s", " ");
	} else {
		noctets = 16;
	}

	for (octet = 0; octet < noctets; octet++)
		seq_printf(seq, "%02x", addr[octet]);
	seq_puts(seq, "/");
	for (octet = 0; octet < noctets; octet++)
		seq_printf(seq, "%02x", addrm[octet]);
}

static void filters_display(struct seq_file *seq, unsigned int fidx,
			    struct filter_entry *f, int hash)
{
	struct adapter *adapter = seq->private;
	u32 fconf = adapter->params.tp.vlan_pri_map;
	u32 tpiconf = adapter->params.tp.ingress_config;
	int i;

	/* Filter index */
	seq_printf(seq, "%4d%c%c", fidx,
		   (!f->locked  ? ' ' : '!'),
		   (!f->pending ? ' ' : (!f->valid ? '+' : '-')));

	if (f->fs.hitcnts) {
		u64 hitcnt;
		int ret;

		ret = cxgb4_get_filter_count(adapter, fidx, &hitcnt,
					     hash, false);
		if (ret)
			seq_printf(seq, " %20s", "hits={ERROR}");
		else
			seq_printf(seq, " %20llu", hitcnt);
	} else {
		seq_printf(seq, " %20s", "Disabled");
	}

	/* Compressed header portion of filter. */
	for (i = FT_FIRST_S; i <= FT_LAST_S; i++) {
		switch (fconf & (1 << i)) {
		case 0:
			/* compressed filter field not enabled */
			break;

		case FCOE_F:
			seq_printf(seq, "  %1d/%1d",
				   f->fs.val.fcoe, f->fs.mask.fcoe);
			break;

		case PORT_F:
			seq_printf(seq, "  %1d/%1d",
				   f->fs.val.iport, f->fs.mask.iport);
			break;

		case VNIC_ID_F:
			if ((tpiconf & VNIC_F) == 0)
				seq_printf(seq, " %1d:%04x/%1d:%04x",
					   f->fs.val.ovlan_vld,
					   f->fs.val.ovlan,
					   f->fs.mask.ovlan_vld,
					   f->fs.mask.ovlan);
			else
				seq_printf(seq, " %1d:%1x:%02x/%1d:%1x:%02x",
					   f->fs.val.ovlan_vld,
					   (f->fs.val.ovlan >> 13) & 0x7,
					   f->fs.val.ovlan & 0x7f,
					   f->fs.mask.ovlan_vld,
					   (f->fs.mask.ovlan >> 13) & 0x7,
					   f->fs.mask.ovlan & 0x7f);
			break;

		case VLAN_F:
			seq_printf(seq, " %1d:%04x/%1d:%04x",
				   f->fs.val.ivlan_vld,
				   f->fs.val.ivlan,
				   f->fs.mask.ivlan_vld,
				   f->fs.mask.ivlan);
			break;

		case TOS_F:
			seq_printf(seq, " %02x/%02x",
				   f->fs.val.tos, f->fs.mask.tos);
			break;

		case PROTOCOL_F:
			seq_printf(seq, " %02x/%02x",
				   f->fs.val.proto, f->fs.mask.proto);
			break;

		case ETHERTYPE_F:
			seq_printf(seq, " %04x/%04x",
				   f->fs.val.ethtype, f->fs.mask.ethtype);
			break;

		case MACMATCH_F:
			seq_printf(seq, " %03x/%03x",
				   f->fs.val.macidx, f->fs.mask.macidx);
			break;

		case MPSHITTYPE_F:
			seq_printf(seq, " %1x/%1x",
				   f->fs.val.matchtype,
				   f->fs.mask.matchtype);
			break;

		case FRAGMENTATION_F:
			seq_printf(seq, "  %1d/%1d",
				   f->fs.val.frag, f->fs.mask.frag);
			break;
		}
	}

	/* Fixed portion of filter. */
	filters_show_ipaddr(seq, f->fs.type,
			    f->fs.val.lip, f->fs.mask.lip);
	filters_show_ipaddr(seq, f->fs.type,
			    f->fs.val.fip, f->fs.mask.fip);
	seq_printf(seq, " %04x/%04x %04x/%04x",
		   f->fs.val.lport, f->fs.mask.lport,
		   f->fs.val.fport, f->fs.mask.fport);

	/* Variable length filter action. */
	if (f->fs.action == FILTER_DROP) {
		seq_puts(seq, " Drop");
	} else if (f->fs.action == FILTER_SWITCH) {
		seq_printf(seq, " Switch: port=%d", f->fs.eport);
		if (f->fs.newdmac)
			seq_printf(seq,
				   ", dmac=%02x:%02x:%02x:%02x:%02x:%02x, l2tidx=%d",
				   f->fs.dmac[0], f->fs.dmac[1],
				   f->fs.dmac[2], f->fs.dmac[3],
				   f->fs.dmac[4], f->fs.dmac[5],
				   f->l2t->idx);
		if (f->fs.newsmac)
			seq_printf(seq,
				   ", smac=%02x:%02x:%02x:%02x:%02x:%02x, smtidx=%d",
				   f->fs.smac[0], f->fs.smac[1],
				   f->fs.smac[2], f->fs.smac[3],
				   f->fs.smac[4], f->fs.smac[5],
				   f->smtidx);
		if (f->fs.newvlan == VLAN_REMOVE)
			seq_puts(seq, ", vlan=none");
		else if (f->fs.newvlan == VLAN_INSERT)
			seq_printf(seq, ", vlan=insert(%x)",
				   f->fs.vlan);
		else if (f->fs.newvlan == VLAN_REWRITE)
			seq_printf(seq, ", vlan=rewrite(%x)",
				   f->fs.vlan);
	} else {
		seq_puts(seq, " Pass: Q=");
		if (f->fs.dirsteer == 0) {
			seq_puts(seq, "RSS");
			if (f->fs.maskhash)
				seq_puts(seq, "(TCB=hash)");
		} else {
			seq_printf(seq, "%d", f->fs.iq);
			if (f->fs.dirsteerhash == 0)
				seq_puts(seq, "(QID)");
			else
				seq_puts(seq, "(hash)");
		}
	}
	if (f->fs.prio)
		seq_puts(seq, " Prio");
	if (f->fs.rpttid)
		seq_puts(seq, " RptTID");
	seq_puts(seq, "\n");
}

static int filters_show(struct seq_file *seq, void *v)
{
	struct adapter *adapter = seq->private;
	u32 fconf = adapter->params.tp.vlan_pri_map;
	u32 tpiconf = adapter->params.tp.ingress_config;
	int i;

	if (v == SEQ_START_TOKEN) {
		seq_puts(seq, "[[Legend: '!' => locked; '+' => pending set; '-' => pending clear]]\n");
		seq_puts(seq, " Idx                   Hits");
		for (i = FT_FIRST_S; i <= FT_LAST_S; i++) {
			switch (fconf & (1 << i)) {
			case 0:
				/* compressed filter field not enabled */
				break;

			case FCOE_F:
				seq_puts(seq, " FCoE");
				break;

			case PORT_F:
				seq_puts(seq, " Port");
				break;

			case VNIC_ID_F:
				if ((tpiconf & VNIC_F) == 0)
					seq_puts(seq, "     vld:oVLAN");
				else
					seq_puts(seq, "   VFvld:PF:VF");
				break;

			case VLAN_F:
				seq_puts(seq, "     vld:iVLAN");
				break;

			case TOS_F:
				seq_puts(seq, "   TOS");
				break;

			case PROTOCOL_F:
				seq_puts(seq, "  Prot");
				break;

			case ETHERTYPE_F:
				seq_puts(seq, "   EthType");
				break;

			case MACMATCH_F:
				seq_puts(seq, "  MACIdx");
				break;

			case MPSHITTYPE_F:
				seq_puts(seq, " MPS");
				break;

			case FRAGMENTATION_F:
				seq_puts(seq, " Frag");
				break;
			}
		}
		seq_printf(seq, " %65s %65s %9s %9s %s\n",
			   "LIP", "FIP", "LPORT", "FPORT", "Action");
	} else {
		int fidx = (uintptr_t)v - 2;
		struct filter_entry *f = &adapter->tids.ftid_tab[fidx];

		/* if this entry isn't filled in just return */
		if (!f->valid && !f->pending)
			return 0;

		filters_display(seq, fidx, f, 0);
	}
	return 0;
}

static inline void *filters_get_idx(struct adapter *adapter, loff_t pos)
{
	if (pos > (adapter->tids.nftids + adapter->tids.nsftids))
		return NULL;

	return (void *)(uintptr_t)(pos + 1);
}

static void *filters_start(struct seq_file *seq, loff_t *pos)
{
	struct adapter *adapter = seq->private;

	return *pos ? filters_get_idx(adapter, *pos) : SEQ_START_TOKEN;
}

static void *filters_next(struct seq_file *seq, void *v, loff_t *pos)
{
	struct adapter *adapter = seq->private;

	(*pos)++;
	return filters_get_idx(adapter, *pos);
}

static void filters_stop(struct seq_file *seq, void *v)
{
}

static const struct seq_operations filters_seq_ops = {
	.start = filters_start,
	.next  = filters_next,
	.stop  = filters_stop,
	.show  = filters_show
};

int filters_open(struct inode *inode, struct file *file)
{
	struct adapter *adapter = inode->i_private;
	int res;

	res = seq_open(file, &filters_seq_ops);
	if (!res) {
		struct seq_file *seq = file->private_data;

		seq->private = adapter;
	}
	return res;
}

const struct file_operations filters_debugfs_fops = {
	.owner   = THIS_MODULE,
	.open    = filters_open,
	.read    = seq_read,
	.llseek  = seq_lseek,
};
