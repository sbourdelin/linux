/*
 *
 *  Realtek Bluetooth Profile profiling driver
 *
 *  Copyright (C) 2015 Realtek Semiconductor Corporation
 *
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/sched.h>
#include <linux/errno.h>
#include <linux/skbuff.h>
#include <linux/usb.h>
#include <linux/dcache.h>
#include <linux/version.h>
#include <linux/skbuff.h>
#include <net/bluetooth/bluetooth.h>
#include <net/bluetooth/hci_core.h>
#include <net/bluetooth/l2cap.h>
#include <net/bluetooth/hci_mon.h>
#include "rtl_btpf.h"

#define VERSION "0.1"

#define BTPF_CMD_MAXLEN		64


static struct rtl_btpf *rtl_btpf;

static int psm_to_profile(u16 psm)
{
	switch (psm) {
	case PSM_AVCTP:
	case PSM_SDP:
		return -1; /* ignore */

	case PSM_HID:
	case PSM_HID_INT:
		return PROFILE_HID;

	case PSM_AVDTP:
		return PROFILE_A2DP;

	case PSM_PAN:
	case PSM_OPP:
	case PSM_FTP:
	case PSM_BIP:
	case PSM_RFCOMM:
		return PROFILE_PAN;

	default:
		return PROFILE_PAN;
	}
}

static struct rtl_hci_conn *rtl_hci_conn_lookup(struct rtl_btpf *btpf,
						u16 handle)
{
	struct list_head *head = &btpf->conn_list;
	struct list_head *p, *n;
	struct rtl_hci_conn *conn;

	list_for_each_safe(p, n, head) {
		conn = list_entry(p, struct rtl_hci_conn, list);
		if ((handle & 0xfff) == conn->handle)
			return conn;
	}

	return NULL;
}

static void rtl_hci_conn_list_purge(struct rtl_btpf *btpf)
{
	struct list_head *head = &btpf->conn_list;
	struct list_head *p, *n;
	struct rtl_hci_conn *conn;

	list_for_each_safe(p, n, head) {
		conn = list_entry(p, struct rtl_hci_conn, list);
		if (conn) {
			list_del(&conn->list);
			kfree(conn);
		}
	}
}

static struct rtl_profile *profile_alloc(u16 handle, u16 psm, u8 idx,
					 u16 dcid, u16 scid)
{
	struct rtl_profile *pf;

	pf = kzalloc(sizeof(struct rtl_profile), GFP_KERNEL);
	if (!pf)
		return NULL;

	pf->handle = handle;
	pf->psm = psm;
	pf->scid = scid;
	pf->dcid = dcid;
	pf->idx = idx;
	INIT_LIST_HEAD(&pf->list);

	return pf;
}

static void rtl_profile_list_purge(struct rtl_btpf *btpf)
{
	struct list_head *head = &btpf->pf_list;
	struct list_head *p, *n;
	struct rtl_profile *pf;

	list_for_each_safe(p, n, head) {
		pf = list_entry(p, struct rtl_profile, list);
		list_del(&pf->list);
		kfree(pf);
	}
}

static struct rtl_profile *rtl_profile_lookup(struct rtl_btpf *btpf,
					      struct rtl_profile_id *id)
{
	struct list_head *head = &btpf->pf_list;
	struct list_head *p, *n;
	struct rtl_profile *tmp;
	u16 handle = id->handle;

	if (!id->match_flags) {
		rtlbt_warn("%s: no match flags", __func__);
		return NULL;
	}

	list_for_each_safe(p, n, head) {
		tmp = list_entry(p, struct rtl_profile, list);

		if ((id->match_flags & RTL_PROFILE_MATCH_HANDLE) &&
		    (handle & 0xfff) != tmp->handle)
			continue;

		if ((id->match_flags & RTL_PROFILE_MATCH_SCID) &&
		    id->scid != tmp->scid)
			continue;

		if ((id->match_flags & RTL_PROFILE_MATCH_DCID) &&
		    id->dcid != tmp->dcid)
			continue;

		return tmp;
	}

	return NULL;
}

static int hci_cmd_send_to_fw(struct rtl_btpf *btpf, u16 opcode, u8 dlen,
			      u8 *data)
{
	int n = 1 + 3 + dlen;
	u8 buff[BTPF_CMD_MAXLEN];
	struct kvec iv = { buff, n };
	struct msghdr msg;
	int ret;

	if (!test_bit(BTPF_HCI_SOCK, &btpf->flags) ||
	    !test_bit(BTPF_CID_RTL, &btpf->flags))
		return -1;

	rtlbt_info("%s: opcode 0x%04x", __func__, opcode);
	if (n > BTPF_CMD_MAXLEN) {
		rtlbt_err("vendor cmd too large");
		return -1;
	}

	buff[0] = HCI_COMMAND_PKT;
	buff[1] = opcode & 0xff;
	buff[2] = (opcode >> 8) & 0xff;
	buff[3] = dlen;
	memcpy(buff + 4, data, dlen);

	memset(&msg, 0, sizeof(msg));

	ret = kernel_sendmsg(btpf->hci_sock, &msg, &iv, 1, n);
	if (ret < 0) {
		rtlbt_err("sendmsg failed: %d", ret);
		return -EAGAIN;
	}

	return 0;
}

static void btpf_update_to_controller(struct rtl_btpf *btpf)
{
	struct list_head *head, *pos, *q;
	struct rtl_hci_conn *conn;
	u8 handle_num;
	u32 buff_sz;
	u8 *buff;
	u8 *p;

	if (!test_bit(BTPF_CID_RTL, &btpf->flags))
		return;

	head = &btpf->conn_list;
	handle_num = 0;
	list_for_each_safe(pos, q, head) {
		conn = list_entry(pos, struct rtl_hci_conn, list);
		if (conn && conn->pf_bits)
			handle_num++;
	}

	buff_sz = 1 + handle_num * 3 + 1;

	rtlbt_info("%s: buff_sz %u, handle_num %u", __func__, buff_sz,
		   handle_num);

	buff = kzalloc(buff_sz, GFP_ATOMIC);
	if (!buff)
		return;

	p = buff;
	*p++ = handle_num;
	head = &btpf->conn_list;
	list_for_each(pos, head) {
		conn = list_entry(pos, struct rtl_hci_conn, list);
		if (conn && conn->pf_bits) {
			put_unaligned_le16(conn->handle, p);
			p += 2;
			rtlbt_info("%s: handle 0x%04x, pf_bits 0x%02x",
				   __func__, conn->handle, conn->pf_bits);
			*p++ = conn->pf_bits;
			handle_num--;
		}
		if (!handle_num)
			break;
	}
	*p++ = btpf->pf_state;

	rtlbt_info("%s: pf_state 0x%02x", __func__, btpf->pf_state);

	hci_cmd_send_to_fw(btpf, HCI_VENDOR_SET_PF_REPORT_CMD, buff_sz, buff);

	kfree(buff);
}

static void update_profile_state(struct rtl_btpf *btpf, u8 idx, u8 busy)
{
	u8 update = 0;

	if (!(btpf->pf_bits & BIT(idx))) {
		rtlbt_err("%s: profile(%x) not exist", __func__, idx);
		return;
	}

	if (busy) {
		if (!(btpf->pf_state & BIT(idx))) {
			update = 1;
			btpf->pf_state |= BIT(idx);
		}
	} else {
		if (btpf->pf_state & BIT(idx)) {
			update = 1;
			btpf->pf_state &= ~BIT(idx);
		}
	}

	if (update) {
		rtlbt_info("%s: pf_bits 0x%02x", __func__, btpf->pf_bits);
		rtlbt_info("%s: pf_state 0x%02x", __func__, btpf->pf_state);
		btpf_update_to_controller(btpf);
	}
}

static void a2dp_do_poll(unsigned long data)
{
	struct rtl_btpf *btpf = (struct rtl_btpf *)data;

	rtlbt_dbg("%s: icount.a2dp %d", __func__, btpf->icount.a2dp);

	if (!btpf->icount.a2dp) {
		if (btpf->pf_state & BIT(PROFILE_A2DP)) {
			rtlbt_info("%s: a2dp state, busy to idle", __func__);
			update_profile_state(btpf, PROFILE_A2DP, 0);
		}
	}

	btpf->icount.a2dp = 0;
	mod_timer(&btpf->a2dp_timer, jiffies + msecs_to_jiffies(1000));
}

static void pan_do_poll(unsigned long data)
{
	struct rtl_btpf *btpf = (struct rtl_btpf *)data;

	rtlbt_dbg("%s: icount.pan %d", __func__, btpf->icount.pan);

	if (btpf->icount.pan < PAN_PACKET_COUNT) {
		if (btpf->pf_state & BIT(PROFILE_PAN)) {
			rtlbt_info("%s: pan state, busy to idle", __func__);
			update_profile_state(btpf, PROFILE_PAN, 0);
		}
	} else {
		if (!(btpf->pf_state & BIT(PROFILE_PAN))) {
			rtlbt_info("%s: pan state, idle to busy", __func__);
			update_profile_state(btpf, PROFILE_PAN, 1);
		}
	}

	btpf->icount.pan = 0;
	mod_timer(&btpf->pan_timer, jiffies + msecs_to_jiffies(1000));
}

static void setup_monitor_timer(struct rtl_btpf *btpf, u8 idx)
{
	switch (idx) {
	case PROFILE_A2DP:
		btpf->icount.a2dp = 0;
		setup_timer(&btpf->a2dp_timer, a2dp_do_poll,
			    (unsigned long)btpf);
		btpf->a2dp_timer.expires = jiffies + msecs_to_jiffies(1000);
		add_timer(&btpf->a2dp_timer);
		break;
	case PROFILE_PAN:
		btpf->icount.pan = 0;
		setup_timer(&btpf->pan_timer, pan_do_poll, (unsigned long)btpf);
		btpf->pan_timer.expires = jiffies + msecs_to_jiffies(1000);
		add_timer(&(btpf->pan_timer));
		break;
	default:
		break;
	}
}

static void del_monitor_timer(struct rtl_btpf *btpf, u8 idx)
{
	switch (idx) {
	case PROFILE_A2DP:
		btpf->icount.a2dp = 0;
		del_timer_sync(&btpf->a2dp_timer);
		break;
	case PROFILE_PAN:
		btpf->icount.pan = 0;
		del_timer_sync(&btpf->pan_timer);
		break;
	default:
		break;
	}
}

static int profile_conn_get(struct rtl_btpf *btpf, struct rtl_hci_conn *conn,
			    u8 idx)
{
	int update = 0;
	u8 i;

	rtlbt_dbg("%s: idx %u", __func__, idx);

	if (!conn || idx >= PROFILE_MAX)
		return -EINVAL;

	if (!btpf->pf_refs[idx]) {
		update = 1;
		btpf->pf_bits |= BIT(idx);

		/* SCO is always busy */
		if (idx == PROFILE_SCO)
			btpf->pf_state |= BIT(idx);

		setup_monitor_timer(btpf, idx);
	}
	btpf->pf_refs[idx]++;

	if (!conn->pf_refs[idx]) {
		update = 1;
		conn->pf_bits |= BIT(idx);
	}
	conn->pf_refs[idx]++;

	rtlbt_info("%s: btpf->pf_bits 0x%02x", __func__, btpf->pf_bits);
	for (i = 0; i < MAX_PROFILE_NUM; i++)
		rtlbt_info("%s: btpf->pf_refs[%u] %d", __func__, i,
			   btpf->pf_refs[i]);

	if (update)
		btpf_update_to_controller(btpf);

	return 0;
}

static int profile_conn_put(struct rtl_btpf *btpf, struct rtl_hci_conn *conn,
			    u8 idx)
{
	int need_update = 0;
	u8 i;

	rtlbt_dbg("%s: idx %u", __func__, idx);

	if (!conn || idx >= PROFILE_MAX)
		return -EINVAL;

	btpf->pf_refs[idx]--;
	if (!btpf->pf_refs[idx]) {
		need_update = 1;
		btpf->pf_bits &= ~BIT(idx);
		btpf->pf_state &= ~BIT(idx);
		del_monitor_timer(btpf, idx);
	}

	conn->pf_refs[idx]--;
	if (!conn->pf_refs[idx]) {
		need_update = 1;
		conn->pf_bits &= ~BIT(idx);

		/* Clear hid interval if needed */
		if (idx == PROFILE_HID &&
		    (conn->pf_bits & BIT(PROFILE_HID2))) {
			conn->pf_bits &= ~BIT(PROFILE_HID2);
			btpf->pf_refs[PROFILE_HID2]--;
		}
	}

	rtlbt_info("%s: btpf->pf_refs[%u] %d", __func__, idx,
		   btpf->pf_refs[idx]);
	rtlbt_info("%s: pf_bits 0x%02x", __func__, btpf->pf_bits);
	for (i = 0; i < MAX_PROFILE_NUM; i++)
		rtlbt_info("%s: btpf->pf_refs[%u] %d", __func__, i,
			   btpf->pf_refs[i]);

	if (need_update)
		btpf_update_to_controller(btpf);

	return 0;
}

static void hid_state_update(struct rtl_btpf *btpf, u16 handle,
			     u16 interval)
{
	u8 update = 0;
	struct rtl_hci_conn *conn;

	conn = rtl_hci_conn_lookup(btpf, handle);
	if (!conn)
		return;

	rtlbt_info("%s: handle 0x%04x, interval 0x%x", __func__, handle,
		   interval);
	if (!(conn->pf_bits & BIT(PROFILE_HID))) {
		rtlbt_dbg("hid not connected in the handle");
		return;
	}

	if (interval < 60) {
		if (!(conn->pf_bits & BIT(PROFILE_HID2))) {
			update = 1;
			conn->pf_bits |= BIT(PROFILE_HID2);

			btpf->pf_refs[PROFILE_HID2]++;
			if (btpf->pf_refs[PROFILE_HID2] == 1)
				btpf->pf_state |= BIT(PROFILE_HID);
		}
	} else {
		if (conn->pf_bits & BIT(PROFILE_HID2)) {
			update = 1;
			conn->pf_bits &= ~BIT(PROFILE_HID2);

			btpf->pf_refs[PROFILE_HID2]--;
			if (!btpf->pf_refs[PROFILE_HID2])
				btpf->pf_state &= ~BIT(PROFILE_HID);
		}
	}

	if (update)
		btpf_update_to_controller(btpf);
}

static int handle_l2cap_conn_req(struct rtl_btpf *btpf, u16 handle, u16 psm,
				 u16 cid, u8 dir)
{
	struct rtl_profile *pf;
	int idx = psm_to_profile(psm);
	struct rtl_profile_id id;

	if (idx < 0) {
		rtlbt_info("no need to parse psm %04x", psm);
		return 0;
	}

	memset(&id, 0, sizeof(id));
	id.match_flags = RTL_PROFILE_MATCH_HANDLE;
	id.handle = handle;

	if (dir == RTL_TO_REMOTE) {
		id.match_flags |= RTL_PROFILE_MATCH_SCID;
		id.scid = cid;
	} else {
		id.match_flags |= RTL_PROFILE_MATCH_DCID;
		id.dcid = cid;
	}

	pf = rtl_profile_lookup(btpf, &id);

	if (pf) {
		rtlbt_warn("%s: profile already exists", __func__);
		return -1;
	}

	if (dir == RTL_TO_REMOTE)
		pf = profile_alloc(handle, psm, (u8)idx, 0, cid);
	else
		pf = profile_alloc(handle, psm, (u8)idx, cid, 0);

	if (!pf) {
		rtlbt_err("%s: allocate profile failed", __func__);
		return -1;
	}

	list_add_tail(&pf->list, &btpf->pf_list);

	return 0;
}

/* dcid is the cid on the device sending this resp packet.
 * scid is the cid on the device receiving the resp packet.
 */
static u8 handle_l2cap_conn_rsp(struct rtl_btpf *btpf,
		u16 handle, u16 dcid,
		u16 scid, u8 dir, u8 result)
{
	struct rtl_profile *pf;
	struct rtl_hci_conn *conn;
	struct rtl_profile_id id = {
		.match_flags = RTL_PROFILE_MATCH_HANDLE,
		.handle = handle,
	};

	if (dir == RTL_FROM_REMOTE) {
		id.match_flags |= RTL_PROFILE_MATCH_SCID;
		id.scid = scid;
		pf = rtl_profile_lookup(btpf, &id);
	} else {
		id.match_flags |= RTL_PROFILE_MATCH_DCID;
		id.dcid = scid;
		pf = rtl_profile_lookup(btpf, &id);
	}

	if (!pf) {
		rtlbt_err("%s: profile not found", __func__);
		return -1;
	}

	if (!result) {
		rtlbt_info("l2cap connection success");
		if (dir == RTL_FROM_REMOTE)
			pf->dcid = dcid;
		else
			pf->scid = dcid;

		conn = rtl_hci_conn_lookup(btpf, handle);
		if (conn)
			profile_conn_get(btpf, conn, pf->idx);
	}

	return 0;
}

static int handle_l2cap_disconn_req(struct rtl_btpf *btpf,
		u16 handle, u16 dcid,
		u16 scid, u8 dir)
{
	struct rtl_profile *pf;
	struct rtl_hci_conn *conn;
	int err = 0;
	struct rtl_profile_id id = {
		.match_flags = RTL_PROFILE_MATCH_HANDLE |
			       RTL_PROFILE_MATCH_SCID |
			       RTL_PROFILE_MATCH_DCID,
		.handle = handle,
		.scid   = scid,
		.dcid   = dcid,
	};

	if (dir == RTL_FROM_REMOTE) {
		id.scid = dcid;
		id.dcid = scid;
		pf = rtl_profile_lookup(btpf, &id);
	} else {
		pf = rtl_profile_lookup(btpf, &id);
	}

	if (!pf) {
		rtlbt_err("%s: no profile", __func__);
		err = -1;
		goto done;
	}

	conn = rtl_hci_conn_lookup(btpf, handle);
	if (!conn) {
		rtlbt_err("%s: no connection", __func__);
		err = -1;
		goto done;
	}

	profile_conn_put(btpf, conn, pf->idx);
	list_del(&pf->list);
	kfree(pf);

done:
	rtlbt_info("%s: handle %04x, dcid %04x, scid %04x, dir %x",
		   __func__, handle, dcid, scid, dir);

	return 0;
}

static const char sample_freqs[4][8] = {
	"16", "32", "44.1", "48"
};

static const u8 sbc_blocks[4] = { 4, 8, 12, 16 };

static const char chan_modes[4][16] = {
	"MONO", "DUAL_CHANNEL", "STEREO", "JOINT_STEREO"
};

static const char alloc_methods[2][12] = {
	"LOUDNESS", "SNR"
};

static const u8 subbands[2] = { 4, 8 };

static void pr_sbc_hdr(struct sbc_frame_hdr *hdr)
{
	rtlbt_info("syncword: %02x", hdr->syncword);
	rtlbt_info("freq %skHz", sample_freqs[hdr->sampling_frequency]);
	rtlbt_info("blocks %u", sbc_blocks[hdr->blocks]);
	rtlbt_info("channel mode %s", chan_modes[hdr->channel_mode]);
	rtlbt_info("allocation method %s",
		   alloc_methods[hdr->allocation_method]);
	rtlbt_info("subbands %u", subbands[hdr->subbands]);
}

static void packet_increment(struct rtl_btpf *btpf, u16 handle,
		u16 ch_id, u16 length, u8 *payload, u8 dir)
{
	struct rtl_profile *pf;
	struct rtl_hci_conn *conn;
	struct rtl_profile_id id;

	conn = rtl_hci_conn_lookup(btpf, handle);
	if (!conn)
		goto done;

	if (conn->type != ACL_CONN)
		return;

	memset(&id, 0, sizeof(id));
	id.match_flags = RTL_PROFILE_MATCH_HANDLE;
	id.handle = handle;
	if (dir == RTL_FROM_REMOTE) {
		id.match_flags |= RTL_PROFILE_MATCH_SCID;
		id.scid = ch_id;
	} else {
		id.match_flags |= RTL_PROFILE_MATCH_DCID;
		id.dcid = ch_id;
	}
	pf = rtl_profile_lookup(btpf, &id);
	if (!pf)
		goto done;

	if (pf->idx == PROFILE_A2DP && length > 100) {
		/* avdtp media data */
		if (!(btpf->pf_state & BIT(PROFILE_A2DP))) {
			struct sbc_frame_hdr *sbc_hdr;
			struct rtp_header *rtp_hdr;
			u8 bitpool;

			update_profile_state(btpf, PROFILE_A2DP, 1);
			rtp_hdr = (struct rtp_header *)payload;

			rtlbt_info("rtp: v %u, cc %u, pt %u", rtp_hdr->v,
				   rtp_hdr->cc, rtp_hdr->pt);

			payload += sizeof(*rtp_hdr) + rtp_hdr->cc * 4 + 1;

			sbc_hdr = (struct sbc_frame_hdr *)payload;

			rtlbt_info("bitpool %u", sbc_hdr->bitpool);

			pr_sbc_hdr(sbc_hdr);

			bitpool = sbc_hdr->bitpool;
			hci_cmd_send_to_fw(btpf, HCI_VENDOR_SET_BITPOOL_CMD, 1,
					   &bitpool);
		}
		btpf->icount.a2dp++;

	}

	if (pf->idx == PROFILE_PAN)
		btpf->icount.pan++;

done:
	return;
}

static void hci_cmd_complete_evt(struct rtl_btpf *btpf, u8 total_len, u8 *p)
{
	u16 opcode;
	struct hci_ev_cmd_complete *cmdcp;

	cmdcp = (struct hci_ev_cmd_complete *)p;
	opcode = le16_to_cpu(cmdcp->opcode);

	switch (opcode) {
	case HCI_OP_READ_LOCAL_VERSION: {
		struct hci_rp_read_local_version *v =
			(struct hci_rp_read_local_version *)(p +
					sizeof(*cmdcp));
		if (v->status)
			break;

		btpf->hci_rev = le16_to_cpu(v->hci_rev);
		btpf->lmp_subver = le16_to_cpu(v->lmp_subver);
		rtlbt_info("HCI Rev 0x%04x, LMP Subver 0x%04x", btpf->hci_rev,
			   btpf->lmp_subver);

		if (le16_to_cpu(v->manufacturer) == 0x005d) {
			rtlbt_info("Realtek Semiconductor Corporation");
			set_bit(BTPF_CID_RTL, &btpf->flags);
		} else {
			clear_bit(BTPF_CID_RTL, &btpf->flags);
		}

		break;
	}
	default:
		break;
	}
}

static void hci_conn_complete_evt(struct rtl_btpf *btpf, u8 *p)
{
	struct hci_ev_conn_complete *ev = (void *)p;
	u16 handle;
	struct rtl_hci_conn *conn;

	handle = __le16_to_cpu(ev->handle);

	conn = rtl_hci_conn_lookup(btpf, handle);
	if (!conn) {
		conn = kzalloc(sizeof(struct rtl_hci_conn), GFP_KERNEL);
		if (conn) {
			conn->handle = handle;
			list_add_tail(&conn->list, &btpf->conn_list);
			conn->pf_bits = 0;
			memset(conn->pf_refs, 0, MAX_PROFILE_NUM);
			/* sco or esco */
			if (ev->link_type == 0 || ev->link_type == 2) {
				conn->type = SYNC_CONN;
				profile_conn_get(btpf, conn, PROFILE_SCO);
			} else {
				conn->type = ACL_CONN;
			}
		} else {
			rtlbt_err("%s: hci conn allocate fail.", __func__);
			return;
		}
	} else {
		/* If the connection has already existed, reset connection
		 * information
		 */
		rtlbt_warn("%s: hci conn handle(0x%x) already existed",
			   __func__, handle);
		conn->pf_bits = 0;
		memset(conn->pf_refs, 0, MAX_PROFILE_NUM);
		/* sco or esco */
		if (ev->link_type == 0 || ev->link_type == 2) {
			conn->type = SYNC_CONN;
			profile_conn_get(btpf, conn, PROFILE_SCO);
		} else {
			conn->type = ACL_CONN;
		}
	}
}

static int hci_disconn_complete_evt(struct rtl_btpf *btpf, u8 *p)
{
	struct hci_ev_disconn_complete *ev = (void *)p;
	u16 handle;
	struct rtl_hci_conn *conn;
	struct list_head *pos, *temp;
	struct rtl_profile *pf;

	handle = le16_to_cpu(ev->handle);

	rtlbt_info("%s: status %u, handle %04x, reason 0x%x", __func__,
		   ev->status, handle, ev->reason);

	if (ev->status)
		return -1;

	conn = rtl_hci_conn_lookup(btpf, handle);
	if (!conn) {
		rtlbt_err("hci conn handle(0x%x) not found", handle);
		return -1;
	}

	switch (conn->type) {
	case ACL_CONN:
		list_for_each_safe(pos, temp, &btpf->pf_list) {
			pf = list_entry(pos, struct rtl_profile, list);
			if (pf->handle == handle && pf->scid && pf->dcid) {
				rtlbt_info(
				  "%s: hndl %04x psm %04x dcid %04x scid %04x",
				  __func__, pf->handle, pf->psm, pf->dcid,
				  pf->scid);
				/* If both scid and dcid are bigger than zero,
				 * L2cap connection exists.
				 */
				profile_conn_put(btpf, conn, pf->idx);
				list_del(&pf->list);
				kfree(pf);
			}
		}
		break;

	case SYNC_CONN:
		profile_conn_put(btpf, conn, PROFILE_SCO);
		break;

	case LE_CONN:
		profile_conn_put(btpf, conn, PROFILE_HID);
		break;

	default:
		break;
	}

	list_del(&conn->list);
	kfree(conn);

	return 0;
}

static void hci_mode_change_evt(struct rtl_btpf *btpf, u8 *p)
{
	struct hci_ev_mode_change *ev = (void *)p;

	hid_state_update(btpf, le16_to_cpu(ev->handle),
			 le16_to_cpu(ev->interval));
}

static void rtl_le_conn_compl_evt(struct rtl_btpf *btpf, u8 *p)
{
	struct hci_ev_le_conn_complete *ev = (void *)p;
	u16 handle, interval;
	struct rtl_hci_conn *conn;

	handle = le16_to_cpu(ev->handle);
	interval = le16_to_cpu(ev->interval);

	conn = rtl_hci_conn_lookup(btpf, handle);
	if (!conn) {
		conn = kzalloc(sizeof(struct rtl_hci_conn), GFP_ATOMIC);
		if (conn) {
			conn->handle = handle;
			list_add_tail(&conn->list, &btpf->conn_list);
			conn->pf_bits = 0;
			memset(conn->pf_refs, 0, MAX_PROFILE_NUM);
			conn->type = LE_CONN;
			/* We consider le is the same as hid */
			profile_conn_get(btpf, conn, PROFILE_HID);
			hid_state_update(btpf, handle, interval);
		} else {
			rtlbt_err("%s: hci conn allocate fail.", __func__);
		}
	} else {
		rtlbt_warn("%s: hci conn handle(%x) already existed.", __func__,
			   handle);
		conn->pf_bits = 0;
		memset(conn->pf_refs, 0, MAX_PROFILE_NUM);
		conn->type = LE_CONN;
		profile_conn_get(btpf, conn, PROFILE_HID);
		hid_state_update(btpf, handle, interval);
	}
}

static void hci_le_conn_complete_evt(struct rtl_btpf *btpf, u8 *p)
{
	struct hci_ev_le_conn_update_complete *ev = (void *)p;
	u16 handle, interval;

	handle = le16_to_cpu(ev->handle);
	interval = le16_to_cpu(ev->interval);
	hid_state_update(btpf, handle, interval);
}

static void hci_le_meta_evt(struct rtl_btpf *btpf, u8 *p)
{
	struct hci_ev_le_meta *le_ev = (void *)p;

	p += sizeof(struct hci_ev_le_meta);

	switch (le_ev->subevent) {
	case HCI_EV_LE_CONN_COMPLETE:
		rtl_le_conn_compl_evt(btpf, p);
		break;

	case HCI_EV_LE_CONN_UPDATE_COMPLETE:
		hci_le_conn_complete_evt(btpf, p);
		break;

	default:
		break;
	}
}

static void hci_process_evt(struct rtl_btpf *btpf, u8 *p, u16 len)
{
	struct hci_event_hdr *hdr = (struct hci_event_hdr *)p;

	(void)&len;

	p += sizeof(struct hci_event_hdr);

	switch (hdr->evt) {
	case HCI_EV_CMD_COMPLETE:
		hci_cmd_complete_evt(btpf, hdr->plen, p);
		break;
	case HCI_EV_CONN_COMPLETE:
	case HCI_EV_SYNC_CONN_COMPLETE:
		hci_conn_complete_evt(btpf, p);
		break;
	case HCI_EV_DISCONN_COMPLETE:
		hci_disconn_complete_evt(btpf, p);
		break;
	case HCI_EV_MODE_CHANGE:
		hci_mode_change_evt(btpf, p);
		break;
	case HCI_EV_LE_META:
		hci_le_meta_evt(btpf, p);
		break;
	default:
		break;
	}
}

static const char l2_dir_str[][4] = {
	"RX", "TX",
};

static void l2_process_frame(struct rtl_btpf *btpf, u8 *data, u16 len,
			     u8 out)
{
	u16 handle;
	u16 flags;
	u16 chann_id;
	u16 psm, scid, dcid, result;
	struct hci_acl_hdr *acl_hdr = (void *)data;
	struct l2cap_cmd_hdr *cmd;
	struct l2cap_hdr *hdr;
	struct l2cap_conn_req *conn_req;
	struct l2cap_conn_rsp *conn_rsp;
	struct l2cap_disconn_req *disc_req;

	handle = __le16_to_cpu(acl_hdr->handle);
	flags  = hci_flags(handle);
	handle = hci_handle(handle);

	if (flags == ACL_CONT)
		return;

	data += sizeof(*acl_hdr);

	hdr = (void *)data;
	chann_id = le16_to_cpu(hdr->cid);

	if (chann_id != 0x0001) {
		if (btpf->pf_bits & BIT(PROFILE_A2DP) ||
		    btpf->pf_bits & BIT(PROFILE_PAN))
			packet_increment(btpf, handle, chann_id,
					 le16_to_cpu(hdr->len), data + 4, out);
		return;
	}

	data += sizeof(*hdr);

	cmd = (void *)data;
	data += sizeof(*cmd);

	switch (cmd->code) {
	case L2CAP_CONN_REQ:
		conn_req = (void *)data;
		psm = le16_to_cpu(conn_req->psm);
		scid = le16_to_cpu(conn_req->scid);
		rtlbt_info(
		    "%s l2cap conn req: hndl %04x psm %04x scid %04x",
		    l2_dir_str[out], handle, psm, scid);
		handle_l2cap_conn_req(btpf, handle, psm, scid, out);
		break;

	case L2CAP_CONN_RSP:
		conn_rsp = (void *)data;
		dcid = le16_to_cpu(conn_rsp->dcid);
		scid = le16_to_cpu(conn_rsp->scid);
		result = le16_to_cpu(conn_rsp->result);
		rtlbt_info(
		    "%s l2cap conn rsp: hndl %04x dcid %04x scid %04x res %x",
		    l2_dir_str[out], handle, dcid, scid, result);
		handle_l2cap_conn_rsp(btpf, handle, dcid, scid, out, result);
		break;

	case L2CAP_DISCONN_REQ:
		disc_req = (void *)data;
		dcid = le16_to_cpu(disc_req->dcid);
		scid = le16_to_cpu(disc_req->scid);
		rtlbt_info(
		    "%s l2cap disc req: hndl %04x dcid %04x scid %04x",
		    l2_dir_str[out], handle, dcid, scid);
		handle_l2cap_disconn_req(btpf, handle, dcid, scid, out);
		break;
	case L2CAP_DISCONN_RSP:
		break;
	default:
		rtlbt_dbg("undesired l2 command code 0x%02x", cmd->code);
		break;
	}
}

static void btpf_process_frame(struct rtl_btpf *btpf, struct sk_buff *skb)
{
	u8 pkt_type = skb->data[0];

	skb_pull(skb, 1);

	if (!test_bit(BTPF_CID_RTL, &btpf->flags)) {
		if (pkt_type == HCI_EVENT_PKT) {
			struct hci_event_hdr *hdr = (void *)skb->data;

			if (hdr->evt == HCI_EV_CMD_COMPLETE) {
				skb_pull(skb, sizeof(*hdr));
				hci_cmd_complete_evt(btpf, hdr->plen,
						     skb->data);
			}
		}
		return;
	}

	switch (pkt_type) {
	case HCI_EVENT_PKT:
		hci_process_evt(btpf, skb->data, skb->len);
		break;
	case HCI_ACLDATA_PKT:
		if (bt_cb(skb)->incoming)
			l2_process_frame(btpf, skb->data, skb->len, 0);
		else
			l2_process_frame(btpf, skb->data, skb->len, 1);
		break;
	default:
		break;
	}
}

static void btpf_process_work(struct work_struct *work)
{
	struct rtl_btpf *btpf;
	struct sock *sk;
	struct sk_buff *skb;

	btpf = container_of(work, struct rtl_btpf, hci_work);
	sk = btpf->hci_sock->sk;

	/* Get data directly from socket receive queue without copying it. */
	while ((skb = skb_dequeue(&sk->sk_receive_queue))) {
		skb_orphan(skb);
		btpf_process_frame(btpf, skb);
		kfree_skb(skb);
	}
}

static void btpf_raw_data_ready(struct sock *sk)
{
	struct rtl_btpf *btpf;

	/* rtlbt_dbg("qlen %d", skb_queue_len(&sk->sk_receive_queue)); */

	btpf = sk->sk_user_data;
	queue_work(btpf->workq, &btpf->hci_work);
}

static void btpf_raw_error_report(struct sock *sk)
{
}

static int btpf_open_socket(struct rtl_btpf *btpf)
{
	int ret;
	struct sockaddr_hci addr;
	struct sock *sk;
	struct hci_filter flt;

	ret = sock_create_kern(&init_net, PF_BLUETOOTH, SOCK_RAW, BTPROTO_HCI,
			       &btpf->hci_sock);
	if (ret < 0) {
		rtlbt_err("Create hci sock error %d", ret);
		goto err_1;
	}

	memset(&addr, 0, sizeof(addr));
	addr.hci_family = AF_BLUETOOTH;
	/* Assume Realtek BT controller index is 0. */
	addr.hci_dev = 0;
	addr.hci_channel = HCI_CHANNEL_RAW;
	ret = kernel_bind(btpf->hci_sock, (struct sockaddr *)&addr,
			  sizeof(addr));
	if (ret < 0) {
		rtlbt_err("Bind hci sock error");
		goto err_2;
	}

	memset(&flt, 0, sizeof(flt));
	/* flt.type_mask = 0; */
	flt.type_mask = (1 << HCI_EVENT_PKT | 1 << HCI_ACLDATA_PKT);
	flt.event_mask[0] = 0xffffffff;
	flt.event_mask[1] = 0xffffffff;

	ret = kernel_setsockopt(btpf->hci_sock, SOL_HCI, HCI_FILTER,
				(char *)&flt, sizeof(flt));
	if (ret < 0) {
		rtlbt_err("Set hci sock filter error %d", ret);
		goto err_2;
	}

	sk = btpf->hci_sock->sk;
	sk->sk_user_data	= btpf;
	sk->sk_data_ready	= btpf_raw_data_ready;
	sk->sk_error_report	= btpf_raw_error_report;

	set_bit(BTPF_HCI_SOCK, &btpf->flags);

	return 0;
err_2:
	sock_release(btpf->hci_sock);
err_1:
	return ret;
}

static void btpf_close_socket(struct rtl_btpf *btpf)
{
	struct socket *socket = btpf->hci_sock;

	if (socket) {
		btpf->hci_sock = NULL;
		kernel_sock_shutdown(socket, SHUT_RDWR);
		socket->sk->sk_user_data = NULL;
		sock_release(socket);
	}

	clear_bit(BTPF_HCI_SOCK, &btpf->flags);
}

int rtl_btpf_init(void)
{
	int i;
	struct rtl_btpf *btpf;
	int ret = 0;

	btpf = kzalloc(sizeof(struct rtl_btpf), GFP_KERNEL);
	if (!btpf)
		return -ENOMEM;

	INIT_LIST_HEAD(&btpf->conn_list);
	INIT_LIST_HEAD(&btpf->pf_list);

	btpf->pf_bits = 0;
	btpf->pf_state = 0;
	for (i = 0; i < MAX_PROFILE_NUM; i++)
		btpf->pf_refs[i] = 0;

	INIT_WORK(&btpf->hci_work, btpf_process_work);

	btpf->workq = create_workqueue("rtl_btpf_workq");
	if (!btpf->workq) {
		ret = -ENOMEM;
		goto err_1;
	}

	/* init sock */
	ret = btpf_open_socket(btpf);
	if (ret < 0) {
		rtlbt_err("Failed to open sock to monitor tx/rx");
		goto err_2;
	}

	rtl_btpf = btpf;

	rtlbt_info("rtl btpf initialized");

	return 0;
err_2:
	flush_workqueue(btpf->workq);
	destroy_workqueue(btpf->workq);
err_1:
	kfree(btpf);
	return ret;
}
EXPORT_SYMBOL_GPL(rtl_btpf_init);

void rtl_btpf_deinit(void)
{
	struct rtl_btpf *btpf = rtl_btpf;

	rtlbt_info("rtl btpf de-initialize");

	rtl_btpf = NULL;

	if (!btpf)
		return;

	flush_workqueue(btpf->workq);
	destroy_workqueue(btpf->workq);

	del_timer_sync(&btpf->a2dp_timer);
	del_timer_sync(&btpf->pan_timer);

	rtl_hci_conn_list_purge(btpf);
	rtl_profile_list_purge(btpf);

	btpf_close_socket(btpf);

	kfree(btpf);
}
EXPORT_SYMBOL_GPL(rtl_btpf_deinit);

MODULE_AUTHOR("Alex Lu <alex_lu@realsil.com.cn>");
MODULE_DESCRIPTION("Bluetooth profiling for Realtek devices ver " VERSION);
MODULE_VERSION(VERSION);
MODULE_LICENSE("GPL");
