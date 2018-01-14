/*
 * Software iWARP device driver
 *
 * Authors: Bernard Metzler <bmt@zurich.ibm.com>
 *          Fredy Neeser <nfd@zurich.ibm.com>
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
#include <net/tcp.h>
#include <linux/list.h>
#include <linux/debugfs.h>

#include <rdma/iw_cm.h>
#include <rdma/ib_verbs.h>
#include <rdma/ib_smi.h>
#include <rdma/ib_user_verbs.h>

#include "siw.h"
#include "siw_cm.h"
#include "siw_obj.h"

#define FDENTRY(f) (f->f_path.dentry)

static struct dentry *siw_debugfs;

static ssize_t siw_show_qps(struct file *f, char __user *buf, size_t space,
			    loff_t *ppos)
{
	struct siw_device *sdev = FDENTRY(f)->d_inode->i_private;
	struct list_head *pos, *tmp;
	char *kbuf = NULL;
	int len = 0, n, num_qp;

	if (*ppos)
		goto out;

	kbuf = kmalloc(space, GFP_KERNEL);
	if (!kbuf)
		goto out;

	num_qp = atomic_read(&sdev->num_qp);
	if (!num_qp)
		goto out;

	len = snprintf(kbuf, space, "%s: %d QPs\n", sdev->base_dev.name,
		       num_qp);
	if (len > space) {
		len = space;
		goto out;
	}
	space -= len;
	n = snprintf(kbuf + len, space,
		     "%-15s%-6s%-6s%-5s%-5s%-5s%-5s%-5s%-20s%s\n",
		     "QP-ID", "State", "Ref's", "SQ", "RQ", "IRQ", "ORQ",
		     "s/r", "Sock", "CEP");

	if (n > space) {
		len += space;
		goto out;
	}
	len += n;
	space -= n;

	list_for_each_safe(pos, tmp, &sdev->qp_list) {
		struct siw_qp *qp = list_entry(pos, struct siw_qp, devq);

		n = snprintf(kbuf + len, space,
			"%-15d%-6d%-6d%-5d%-5d%-5d%-5d%d/%-3d0x%-18p0x%-18p\n",
			     QP_ID(qp),
			     qp->attrs.state,
			     refcount_read(&qp->hdr.ref),
			     qp->attrs.sq_size,
			     qp->attrs.rq_size,
			     qp->attrs.irq_size,
			     qp->attrs.orq_size,
			     tx_wqe(qp) ? 1 : 0,
			     rx_wqe(qp) ? 1 : 0,
			     qp->attrs.llp_stream_handle,
			     qp->cep);
		if (n < space) {
			len += n;
			space -= n;
		} else {
			len += space;
			break;
		}
	}
out:
	if (len)
		len = simple_read_from_buffer(buf, len, ppos, kbuf, len);

	kfree(kbuf);

	return len;
};

static ssize_t siw_show_mrs(struct file *f, char __user *buf, size_t space,
			    loff_t *ppos)
{
	struct siw_device *sdev = FDENTRY(f)->d_inode->i_private;
	struct list_head *pos, *tmp;
	char *kbuf = NULL;
	int len = 0, n, num_mr;

	if (*ppos)
		goto out;

	kbuf = kmalloc(space, GFP_KERNEL);
	if (!kbuf)
		goto out;

	num_mr = atomic_read(&sdev->num_mr);
	if (!num_mr)
		goto out;

	len = snprintf(kbuf, space, "%s: %d MRs\n", sdev->base_dev.name,
		       num_mr);
	if (len > space) {
		len = space;
		goto out;
	}
	space -= len;

	n = snprintf(kbuf + len, space,
		     "%-15s%-18s%-8s%-8s%-22s%-8s%-9s\n",
		     "MEM-ID", "PD", "STag", "Type", "size", "Ref's", "State");

	if (n > space) {
		len += space;
		goto out;
	}
	len += n;
	space -= n;

	list_for_each_safe(pos, tmp, &sdev->mr_list) {
		struct siw_mr *mr = list_entry(pos, struct siw_mr, devq);

		n = snprintf(kbuf + len, space,
			     "%-15d%-18p0x%-8x%-8s0x%-20llx%-8d%-9s\n",
			     OBJ_ID(&mr->mem), mr->pd, mr->mem.hdr.id,
			     mr->mem_obj ? mr->mem.is_pbl ? "PBL":"UMEM":"KVA",
			     mr->mem.len,
			     refcount_read(&mr->mem.hdr.ref),
			     mr->mem.stag_valid ? "valid" : "invalid");
		if (n < space) {
			len += n;
			space -= n;
		} else {
			len += space;
			break;
		}
	}
out:
	if (len)
		len = simple_read_from_buffer(buf, len, ppos, kbuf, len);

	kfree(kbuf);

	return len;
}

static ssize_t siw_show_ceps(struct file *f, char __user *buf, size_t space,
			     loff_t *ppos)
{
	struct siw_device *sdev = FDENTRY(f)->d_inode->i_private;
	struct list_head *pos, *tmp;
	char *kbuf = NULL;
	int len = 0, n, num_cep;

	if (*ppos)
		goto out;

	kbuf = kmalloc(space, GFP_KERNEL);
	if (!kbuf)
		goto out;

	num_cep = atomic_read(&sdev->num_cep);
	if (!num_cep)
		goto out;

	len = snprintf(kbuf, space, "%s: %d CEPs\n", sdev->base_dev.name,
		       num_cep);
	if (len > space) {
		len = space;
		goto out;
	}
	space -= len;

	n = snprintf(kbuf + len, space,
		     "%-20s%-6s%-6s%-9s%-5s%-3s%-4s%-21s%-9s\n",
		     "CEP", "State", "Ref's", "QP-ID", "LQ", "LC", "U", "Sock",
		     "CM-ID");

	if (n > space) {
		len += space;
		goto out;
	}
	len += n;
	space -= n;

	list_for_each_safe(pos, tmp, &sdev->cep_list) {
		struct siw_cep *cep = list_entry(pos, struct siw_cep, devq);

		n = snprintf(kbuf + len, space,
			     "0x%-18p%-6d%-6d%-9d%-5s%-3s%-4d0x%-18p 0x%-16p\n",
			     cep, cep->state,
			     refcount_read(&cep->ref),
			     cep->qp ? QP_ID(cep->qp) : -1,
			     list_empty(&cep->listenq) ? "n" : "y",
			     cep->listen_cep ? "y" : "n",
			     cep->in_use,
			     cep->llp.sock,
			     cep->cm_id);
		if (n < space) {
			len += n;
			space -= n;
		} else {
			len += space;
			break;
		}
	}
out:
	if (len)
		len = simple_read_from_buffer(buf, len, ppos, kbuf, len);

	kfree(kbuf);

	return len;
}

static ssize_t siw_show_stats(struct file *f, char __user *buf, size_t space,
			      loff_t *ppos)
{
	struct siw_device *sdev = FDENTRY(f)->d_inode->i_private;
	char *kbuf = NULL;
	int len = 0;

	if (*ppos)
		goto out;

	kbuf = kmalloc(space, GFP_KERNEL);
	if (!kbuf)
		goto out;

	len =  snprintf(kbuf, space, "Allocated SIW Objects:\n"
		"Device %s (%s):\t"
		"%s: %d, %s %d, %s: %d, %s: %d, %s: %d, %s: %d, %s: %d\n",
		sdev->base_dev.name,
		sdev->netdev->flags & IFF_UP ? "IFF_UP" : "IFF_DOWN",
		"CXs", atomic_read(&sdev->num_ctx),
		"PDs", atomic_read(&sdev->num_pd),
		"QPs", atomic_read(&sdev->num_qp),
		"CQs", atomic_read(&sdev->num_cq),
		"SRQs", atomic_read(&sdev->num_srq),
		"MRs", atomic_read(&sdev->num_mr),
		"CEPs", atomic_read(&sdev->num_cep));
	if (len > space)
		len = space;
out:
	if (len)
		len = simple_read_from_buffer(buf, len, ppos, kbuf, len);

	kfree(kbuf);
	return len;
}

static const struct file_operations siw_qp_debug_fops = {
	.owner	= THIS_MODULE,
	.read	= siw_show_qps
};

static const struct file_operations siw_mr_debug_fops = {
	.owner	= THIS_MODULE,
	.read	= siw_show_mrs
};

static const struct file_operations siw_cep_debug_fops = {
	.owner	= THIS_MODULE,
	.read	= siw_show_ceps
};

static const struct file_operations siw_stats_debug_fops = {
	.owner	= THIS_MODULE,
	.read	= siw_show_stats
};

void siw_debugfs_add_device(struct siw_device *sdev)
{
	struct dentry	*entry;

	if (!siw_debugfs)
		return;

	sdev->debugfs = debugfs_create_dir(sdev->base_dev.name, siw_debugfs);
	if (sdev->debugfs) {
		entry = debugfs_create_file("qp", 0400, sdev->debugfs,
					    (void *)sdev, &siw_qp_debug_fops);
		if (!entry)
			siw_dbg(sdev, "could not create 'qp' entry\n");

		entry = debugfs_create_file("cep", 0400, sdev->debugfs,
					    (void *)sdev, &siw_cep_debug_fops);
		if (!entry)
			siw_dbg(sdev, "could not create 'cep' entry\n");

		entry = debugfs_create_file("mr", 0400, sdev->debugfs,
					    (void *)sdev, &siw_mr_debug_fops);
		if (!entry)
			siw_dbg(sdev, "could not create 'qp' entry\n");

		entry = debugfs_create_file("stats", 0400, sdev->debugfs,
					    (void *)sdev,
					    &siw_stats_debug_fops);
		if (!entry)
			siw_dbg(sdev, "could not create 'stats' entry\n");
	}
}

void siw_debugfs_del_device(struct siw_device *sdev)
{
	debugfs_remove_recursive(sdev->debugfs);
	sdev->debugfs = NULL;
}

void siw_debug_init(void)
{
	siw_debugfs = debugfs_create_dir("siw", NULL);

	if (!siw_debugfs || siw_debugfs == ERR_PTR(-ENODEV)) {
		pr_warn("SIW: could not init debugfs\n");
		siw_debugfs = NULL;
	}
}

void siw_debugfs_delete(void)
{
	debugfs_remove_recursive(siw_debugfs);
	siw_debugfs = NULL;
}

void siw_print_hdr(union iwarp_hdr *hdr, int qp_id, char *msg)
{
	enum rdma_opcode op = __rdmap_opcode(&hdr->ctrl);
	u16 mpa_len = be16_to_cpu(hdr->ctrl.mpa_len);

	switch (op) {

	case RDMAP_RDMA_WRITE:
		pr_info("siw: [QP %d]: %s(WRITE, DDP len %d): %08x %016llx\n",
			qp_id, msg, ddp_data_len(op, mpa_len),
			hdr->rwrite.sink_stag, hdr->rwrite.sink_to);
		break;

	case RDMAP_RDMA_READ_REQ:
		pr_info("siw: [QP %d]: %s(RREQ, DDP len %d): %08x %08x %08x %08x %016llx %08x %08x %016llx\n",
			qp_id, msg,
			ddp_data_len(op, mpa_len),
			be32_to_cpu(hdr->rreq.ddp_qn),
			be32_to_cpu(hdr->rreq.ddp_msn),
			be32_to_cpu(hdr->rreq.ddp_mo),
			be32_to_cpu(hdr->rreq.sink_stag),
			be64_to_cpu(hdr->rreq.sink_to),
			be32_to_cpu(hdr->rreq.read_size),
			be32_to_cpu(hdr->rreq.source_stag),
			be64_to_cpu(hdr->rreq.source_to));

		break;

	case RDMAP_RDMA_READ_RESP:
		pr_info("siw: [QP %d]: %s(RRESP, DDP len %d): %08x %016llx\n",
			qp_id, msg, ddp_data_len(op, mpa_len),
			be32_to_cpu(hdr->rresp.sink_stag),
			be64_to_cpu(hdr->rresp.sink_to));
		break;

	case RDMAP_SEND:
		pr_info("siw: [QP %d]: %s(SEND, DDP len %d): %08x %08x %08x\n",
			qp_id, msg, ddp_data_len(op, mpa_len),
			be32_to_cpu(hdr->send.ddp_qn),
			be32_to_cpu(hdr->send.ddp_msn),
			be32_to_cpu(hdr->send.ddp_mo));
		break;

	case RDMAP_SEND_INVAL:
		pr_info("siw: [QP %d]: %s(S_INV, DDP len %d): %08x %08x %08x %08x\n",
			qp_id, msg, ddp_data_len(op, mpa_len),
			be32_to_cpu(hdr->send_inv.inval_stag),
			be32_to_cpu(hdr->send_inv.ddp_qn),
			be32_to_cpu(hdr->send_inv.ddp_msn),
			be32_to_cpu(hdr->send_inv.ddp_mo));
		break;

	case RDMAP_SEND_SE:
		pr_info("siw: [QP %d]: %s(S_SE, DDP len %d): %08x %08x %08x\n",
			qp_id, msg, ddp_data_len(op, mpa_len),
			be32_to_cpu(hdr->send.ddp_qn),
			be32_to_cpu(hdr->send.ddp_msn),
			be32_to_cpu(hdr->send.ddp_mo));
		break;

	case RDMAP_SEND_SE_INVAL:
		pr_info("siw: [QP %d]: %s(S_SE_INV, DDP len %d): %08x %08x %08x %08x\n",
			qp_id, msg, ddp_data_len(op, mpa_len),
			be32_to_cpu(hdr->send_inv.inval_stag),
			be32_to_cpu(hdr->send_inv.ddp_qn),
			be32_to_cpu(hdr->send_inv.ddp_msn),
			be32_to_cpu(hdr->send_inv.ddp_mo));
		break;

	case RDMAP_TERMINATE:
		pr_info("siw: [QP %d]: %s(TERM, DDP len %d):\n", qp_id, msg,
			ddp_data_len(op, mpa_len));
		break;

	default:
		pr_info("siw: [QP %d]: %s (undefined opcode %d)", qp_id, msg,
			op);
		break;
	}
}

char ib_qp_state_to_string[IB_QPS_ERR+1][sizeof "RESET"] = {
	[IB_QPS_RESET]	= "RESET",
	[IB_QPS_INIT]	= "INIT",
	[IB_QPS_RTR]	= "RTR",
	[IB_QPS_RTS]	= "RTS",
	[IB_QPS_SQD]	= "SQD",
	[IB_QPS_SQE]	= "SQE",
	[IB_QPS_ERR]	= "ERR"
};
