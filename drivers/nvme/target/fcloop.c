/*
 * Copyright (c) 2016 Avago Technologies.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful.
 * ALL EXPRESS OR IMPLIED CONDITIONS, REPRESENTATIONS AND WARRANTIES,
 * INCLUDING ANY IMPLIED WARRANTY OF MERCHANTABILITY, FITNESS FOR A
 * PARTICULAR PURPOSE, OR NON-INFRINGEMENT, ARE DISCLAIMED, EXCEPT TO
 * THE EXTENT THAT SUCH DISCLAIMERS ARE HELD TO BE LEGALLY INVALID.
 * See the GNU General Public License for more details, a copy of which
 * can be found in the file COPYING included with this package
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt
#include <linux/module.h>
#include <linux/parser.h>

#include "../host/nvme.h"
#include "../target/nvmet.h"
#include <linux/nvme-fc-driver.h>
#include <linux/nvme-fc.h>

#define is_end_of_list(pos, head, member) ((&pos->member) == (head))

enum {
	NVMF_OPT_ERR		= 0,
	NVMF_OPT_WWNN		= 1 << 0,
	NVMF_OPT_WWPN		= 1 << 1,
	NVMF_OPT_ROLES		= 1 << 2,
	NVMF_OPT_FCADDR		= 1 << 3,
	NVMF_OPT_FABRIC		= 1 << 5,
};

struct fcloop_ctrl_options {
	int			mask;
	u64			wwnn;
	u64			wwpn;
	u32			roles;
	u32			fcaddr;
	u64			fabric;
};

static const match_table_t opt_tokens = {
	{ NVMF_OPT_WWNN,	"wwnn=%s"	},
	{ NVMF_OPT_WWPN,	"wwpn=%s"	},
	{ NVMF_OPT_ROLES,	"roles=%d"	},
	{ NVMF_OPT_FCADDR,	"fcaddr=%x"	},
	{ NVMF_OPT_FABRIC,	"fabric=%s"	},
	{ NVMF_OPT_ERR,		NULL		}
};

static int
fcloop_parse_options(struct fcloop_ctrl_options *opts,
		const char *buf)
{
	substring_t args[MAX_OPT_ARGS];
	char *options, *o, *p;
	int token, ret = 0;
	u64 token64;

	options = o = kstrdup(buf, GFP_KERNEL);
	if (!options)
		return -ENOMEM;

	while ((p = strsep(&o, ",\n")) != NULL) {
		if (!*p)
			continue;

		token = match_token(p, opt_tokens, args);
		opts->mask |= token;
		switch (token) {
		case NVMF_OPT_WWNN:
			if (match_u64(args, &token64)) {
				ret = -EINVAL;
				goto out_free_options;
			}
			opts->wwnn = token64;
			break;
		case NVMF_OPT_WWPN:
			if (match_u64(args, &token64)) {
				ret = -EINVAL;
				goto out_free_options;
			}
			opts->wwpn = token64;
			break;
		case NVMF_OPT_ROLES:
			if (match_int(args, &token)) {
				ret = -EINVAL;
				goto out_free_options;
			}
			opts->roles = token;
			break;
		case NVMF_OPT_FCADDR:
			if (match_hex(args, &token)) {
				ret = -EINVAL;
				goto out_free_options;
			}
			opts->fcaddr = token;
			break;
		case NVMF_OPT_FABRIC:
			if (match_u64(args, &token64)) {
				ret = -EINVAL;
				goto out_free_options;
			}
			opts->fabric = token64;
			break;
		default:
			pr_warn("unknown parameter or missing value '%s'\n", p);
			ret = -EINVAL;
			goto out_free_options;
		}
	}

out_free_options:
	kfree(options);
	return ret;
}


static int
fcloop_parse_nm_options(struct device *dev, u64 *fname, u64 *pname,
		const char *buf)
{
	substring_t args[MAX_OPT_ARGS];
	char *options, *o, *p;
	int token, ret = 0;
	u64 token64;

	*fname = -1;
	*pname = -1;

	options = o = kstrdup(buf, GFP_KERNEL);
	if (!options)
		return -ENOMEM;

	while ((p = strsep(&o, ",\n")) != NULL) {
		if (!*p)
			continue;

		token = match_token(p, opt_tokens, args);
		switch (token) {
		case NVMF_OPT_FABRIC:
			if (match_u64(args, &token64)) {
				ret = -EINVAL;
				goto out_free_options;
			}
			*fname = token64;
			break;
		case NVMF_OPT_WWPN:
			if (match_u64(args, &token64)) {
				ret = -EINVAL;
				goto out_free_options;
			}
			*pname = token64;
			break;
		default:
			pr_warn("unknown parameter or missing value '%s'\n", p);
			ret = -EINVAL;
			goto out_free_options;
		}
	}

out_free_options:
	kfree(options);

	if (!ret) {
		if (*fname == -1)
			return -EINVAL;
		if (*pname == -1)
			return -EINVAL;
	}

	return ret;
}


#define LPORT_OPTS	(NVMF_OPT_WWNN | NVMF_OPT_WWPN | NVMF_OPT_ROLES | \
			 NVMF_OPT_FCADDR | NVMF_OPT_FABRIC)

#define RPORT_OPTS	(NVMF_OPT_WWNN | NVMF_OPT_WWPN | NVMF_OPT_ROLES | \
			 NVMF_OPT_FCADDR | NVMF_OPT_FABRIC)

#define TGTPORT_OPTS	(NVMF_OPT_WWNN | NVMF_OPT_WWPN | \
			 NVMF_OPT_FCADDR | NVMF_OPT_FABRIC)

#define ALL_OPTS	(NVMF_OPT_WWNN | NVMF_OPT_WWPN | NVMF_OPT_ROLES | \
			 NVMF_OPT_FCADDR | NVMF_OPT_FABRIC)


static LIST_HEAD(fcloop_lports);

struct fcloop_lport {
	struct nvme_fc_local_port *localport;
	struct list_head list;
	struct list_head rport_list;
};

struct fcloop_rport {
	struct nvme_fc_remote_port *remoteport;
	struct list_head list;
	struct nvmet_fc_target_port *targetport;
};

struct fcloop_tgtport {
	struct nvmet_fc_target_port *tgtport;
	struct fcloop_rport *rport;
	struct fcloop_lport *lport;
};


struct fcloop_lsreq {
	struct fcloop_tgtport		*tport;
	struct nvmefc_ls_req		*lsreq;
	struct work_struct		work;
	struct nvmefc_tgt_ls_req	tgt_ls_req;
};

struct fcloop_fcpreq {
	struct fcloop_tgtport		*tport;
	struct nvmefc_fcp_req		*fcpreq;
	u16				status;
	struct work_struct		work;
	struct nvmefc_tgt_fcp_req	tgt_fcp_req;
};

int
fcloop_create_queue(struct nvme_fc_local_port *localport,
			unsigned int qidx, u16 qsize,
			void **handle)
{
	*handle = localport;
	return 0;
}

void
fcloop_delete_queue(struct nvme_fc_local_port *localport,
			unsigned int idx, void *handle)
{
}


/*
 * Transmit of LS RSP done (e.g. buffers all set). call back up
 * initiator "done" flows.
 */
void
fcloop_tgt_lsrqst_done_work(struct work_struct *work)
{
	struct fcloop_lsreq *tls_req =
		container_of(work, struct fcloop_lsreq, work);
	struct nvmefc_ls_req *lsreq = tls_req->lsreq;

	lsreq->done(lsreq, 0);
}

int
fcloop_ls_req(struct nvme_fc_local_port *localport,
			struct nvme_fc_remote_port *remoteport,
			struct nvmefc_ls_req *lsreq)
{
	struct fcloop_lsreq *tls_req = lsreq->private;
	struct fcloop_rport *rport = remoteport->private;
	int ret = 0;

	tls_req->lsreq = lsreq;
	tls_req->tport = rport->targetport->private;
	INIT_WORK(&tls_req->work, fcloop_tgt_lsrqst_done_work);

	ret = nvmet_fc_rcv_ls_req(rport->targetport, &tls_req->tgt_ls_req,
				 lsreq->rqstaddr, lsreq->rqstlen);

	return ret;
}

int
fcloop_xmt_ls_rsp(struct nvmet_fc_target_port *tgtport,
			struct nvmefc_tgt_ls_req *tgt_lsreq)
{
	struct fcloop_lsreq *tls_req =
		container_of(tgt_lsreq, struct fcloop_lsreq, tgt_ls_req);
	struct nvmefc_ls_req *lsreq = tls_req->lsreq;

	memcpy(lsreq->rspaddr, tgt_lsreq->rspbuf,
		((lsreq->rsplen < tgt_lsreq->rsplen) ?
				lsreq->rsplen : tgt_lsreq->rsplen));
	(tgt_lsreq->done)(tgt_lsreq);

	schedule_work(&tls_req->work);

	return 0;
}

/*
 * FCP IO operation done. call back up initiator "done" flows.
 */
void
fcloop_tgt_fcprqst_done_work(struct work_struct *work)
{
	struct fcloop_fcpreq *tfcp_req =
		container_of(work, struct fcloop_fcpreq, work);
	struct nvmefc_fcp_req *fcpreq = tfcp_req->fcpreq;

	fcpreq->status = tfcp_req->status;
	fcpreq->done(fcpreq);
}


int
fcloop_fcp_req(struct nvme_fc_local_port *localport,
			struct nvme_fc_remote_port *remoteport,
			void *hw_queue_handle,
			struct nvmefc_fcp_req *fcpreq)
{
	struct fcloop_fcpreq *tfcp_req = fcpreq->private;
	struct fcloop_rport *rport = remoteport->private;
	int ret = 0;

	tfcp_req->fcpreq = fcpreq;
	tfcp_req->tport = rport->targetport->private;
	INIT_WORK(&tfcp_req->work, fcloop_tgt_fcprqst_done_work);

	ret = nvmet_fc_rcv_fcp_req(rport->targetport, &tfcp_req->tgt_fcp_req,
				 fcpreq->cmdaddr, fcpreq->cmdlen);

	return ret;
}

void
fcloop_fcp_copy_data(u8 op, struct scatterlist *data_sg,
			struct scatterlist *io_sg, u32 offset, u32 length)
{
	void *data_p, *io_p;
	u32 data_len, io_len, tlen;

	io_p = sg_virt(io_sg);
	io_len = io_sg->length;

	for ( ; offset; ) {
		tlen = min_t(u32, offset, io_len);
		offset -= tlen;
		io_len -= tlen;
		if (!io_len) {
			io_sg = sg_next(io_sg);
			io_p = sg_virt(io_sg);
			io_len = io_sg->length;
		} else
			io_p += tlen;
	}

	data_p = sg_virt(data_sg);
	data_len = data_sg->length;

	for ( ; length; ) {
		tlen = min_t(u32, io_len, data_len);
		tlen = min_t(u32, tlen, length);

		if (op == NVMET_FCOP_WRITEDATA)
			memcpy(data_p, io_p, tlen);
		else
			memcpy(io_p, data_p, tlen);

		length -= tlen;

		io_len -= tlen;
		if ((!io_len) && (length)) {
			io_sg = sg_next(io_sg);
			io_p = sg_virt(io_sg);
			io_len = io_sg->length;
		} else
			io_p += tlen;

		data_len -= tlen;
		if ((!data_len) && (length)) {
			data_sg = sg_next(data_sg);
			data_p = sg_virt(data_sg);
			data_len = data_sg->length;
		} else
			data_p += tlen;
	}
}

int
fcloop_fcp_op(struct nvmet_fc_target_port *tgtport,
			struct nvmefc_tgt_fcp_req *tgt_fcpreq)
{
	struct fcloop_fcpreq *tfcp_req =
		container_of(tgt_fcpreq, struct fcloop_fcpreq, tgt_fcp_req);
	struct nvmefc_fcp_req *fcpreq = tfcp_req->fcpreq;
	u32 rsplen = 0, xfrlen = 0;
	int fcp_err = 0;
	u8 op = tgt_fcpreq->op;

	switch (op) {
	case NVMET_FCOP_WRITEDATA:
		xfrlen = tgt_fcpreq->transfer_length;
		fcloop_fcp_copy_data(op, tgt_fcpreq->sg, fcpreq->first_sgl,
					tgt_fcpreq->offset, xfrlen);
		fcpreq->transferred_length += xfrlen;
		break;

	case NVMET_FCOP_READDATA:
	case NVMET_FCOP_READDATA_RSP:
		xfrlen = tgt_fcpreq->transfer_length;
		fcloop_fcp_copy_data(op, tgt_fcpreq->sg, fcpreq->first_sgl,
					tgt_fcpreq->offset, xfrlen);
		fcpreq->transferred_length += xfrlen;
		if (op == NVMET_FCOP_READDATA)
			break;

		/* Fall-Thru to RSP handling */

	case NVMET_FCOP_RSP:
		rsplen = ((fcpreq->rsplen < tgt_fcpreq->rsplen) ?
				fcpreq->rsplen : tgt_fcpreq->rsplen);
		memcpy(fcpreq->rspaddr, tgt_fcpreq->rspaddr, rsplen);
		if (rsplen < tgt_fcpreq->rsplen)
			fcp_err = -E2BIG;
		fcpreq->rcv_rsplen = rsplen;
		fcpreq->status = 0;
		tfcp_req->status = 0;
		break;

	case NVMET_FCOP_ABORT:
		tfcp_req->status = NVME_SC_FC_TRANSPORT_ABORTED;
		break;

	default:
		fcp_err = -EINVAL;
		break;
	}

	tgt_fcpreq->transferred_length = xfrlen;
	tgt_fcpreq->fcp_error = fcp_err;
	tgt_fcpreq->done(tgt_fcpreq);

	if ((!fcp_err) && (op == NVMET_FCOP_RSP ||
			op == NVMET_FCOP_READDATA_RSP ||
			op == NVMET_FCOP_ABORT))
		schedule_work(&tfcp_req->work);

	return 0;
}

void
fcloop_ls_abort(struct nvme_fc_local_port *localport,
			struct nvme_fc_remote_port *remoteport,
				struct nvmefc_ls_req *lsreq)
{
}

void
fcloop_fcp_abort(struct nvme_fc_local_port *localport,
			struct nvme_fc_remote_port *remoteport,
			void *hw_queue_handle,
			struct nvmefc_fcp_req *fcpreq)
{
}


struct nvme_fc_port_template fctemplate = {
	.create_queue	= fcloop_create_queue,
	.delete_queue	= fcloop_delete_queue,
	.ls_req		= fcloop_ls_req,
	.fcp_io		= fcloop_fcp_req,
	.ls_abort	= fcloop_ls_abort,
	.fcp_abort	= fcloop_fcp_abort,

	.max_hw_queues	= 1,
	.max_sgl_segments = 256,
	.max_dif_sgl_segments = 256,
	.dma_boundary = 0xFFFFFFFF,
	/* sizes of additional private data for data structures */
	.local_priv_sz	= sizeof(struct fcloop_lport),
	.remote_priv_sz	= sizeof(struct fcloop_rport),
	.lsrqst_priv_sz = sizeof(struct fcloop_lsreq),
	.fcprqst_priv_sz = sizeof(struct fcloop_fcpreq),
};

struct nvmet_fc_target_template tgttemplate = {
	.xmt_ls_rsp	= fcloop_xmt_ls_rsp,
	.fcp_op		= fcloop_fcp_op,

	.max_hw_queues	= 1,
	.max_sgl_segments = 256,
	.max_dif_sgl_segments = 256,
	.dma_boundary = 0xFFFFFFFF,

	/* optional features */
	.target_features = NVMET_FCTGTFEAT_READDATA_RSP,

	/* sizes of additional private data for data structures */
	.target_priv_sz = sizeof(struct fcloop_tgtport),
};

static ssize_t
fcloop_create_local_port(struct device *dev, struct device_attribute *attr,
		const char *buf, size_t count)
{
	struct nvme_fc_port_info pinfo;
	struct fcloop_ctrl_options *opts;
	struct nvme_fc_local_port *localport;
	struct fcloop_lport *lport;
	int ret;

	opts = kzalloc(sizeof(*opts), GFP_KERNEL);
	if (!opts)
		return -ENOMEM;

	ret = fcloop_parse_options(opts, buf);
	if (ret)
		goto out_free_opts;

	/* everything there ? */
	if ((opts->mask & LPORT_OPTS) != LPORT_OPTS) {
		ret = -EINVAL;
		goto out_free_opts;
	}

	pinfo.fabric_name = opts->fabric;
	pinfo.node_name = opts->wwnn;
	pinfo.port_name = opts->wwpn;
	pinfo.port_role = opts->roles;
	pinfo.port_id = opts->fcaddr;

	ret = nvme_fc_register_localport(&pinfo, &fctemplate, NULL, &localport);
	if (!ret) {
		/* success */
		lport = localport->private;
		lport->localport = localport;
		INIT_LIST_HEAD(&lport->list);
		INIT_LIST_HEAD(&lport->rport_list);
		list_add_tail(&lport->list, &fcloop_lports);

		/* mark all of the input buffer consumed */
		ret = count;
	}

out_free_opts:
	kfree(opts);
	return ret;
}

static int __delete_local_port(struct fcloop_lport *lport)
{
	int ret;

	if (!list_empty(&lport->rport_list))
		return -EBUSY;

	list_del(&lport->list);

	ret = nvme_fc_unregister_localport(lport->localport);

	return ret;
}

static ssize_t
fcloop_delete_local_port(struct device *dev, struct device_attribute *attr,
		const char *buf, size_t count)
{
	struct fcloop_lport *lport, *lnext;
	u64 fabric, portname;
	int ret;

	ret = fcloop_parse_nm_options(dev, &fabric, &portname, buf);
	if (ret)
		return ret;

	list_for_each_entry_safe(lport, lnext, &fcloop_lports, list) {
		if ((lport->localport->fabric_name == fabric) &&
		    (lport->localport->port_name == portname)) {
			break;
		}
	}
	if (is_end_of_list(lport, &fcloop_lports, list))
		return -ENOENT;

	ret = __delete_local_port(lport);

	if (!ret)
		return count;

	return ret;
}

static ssize_t
fcloop_create_remote_port(struct device *dev, struct device_attribute *attr,
		const char *buf, size_t count)
{
	struct fcloop_ctrl_options *opts;
	struct fcloop_lport *lport, *lnext;
	struct nvme_fc_remote_port *remoteport;
	struct fcloop_rport *rport;
	struct nvme_fc_port_info pinfo;
	struct nvmet_fc_port_info tinfo;
	int ret;

	opts = kzalloc(sizeof(*opts), GFP_KERNEL);
	if (!opts)
		return -ENOMEM;

	ret = fcloop_parse_options(opts, buf);
	if (ret)
		goto out_free_opts;

	/* everything there ? */
	if ((opts->mask & RPORT_OPTS) != RPORT_OPTS) {
		ret = -EINVAL;
		goto out_free_opts;
	}

	pinfo.fabric_name = tinfo.fabric_name = opts->fabric;
	pinfo.node_name = tinfo.node_name = opts->wwnn;
	pinfo.port_name = tinfo.port_name = opts->wwpn;
	pinfo.port_role = opts->roles;
	pinfo.port_id = tinfo.port_id = opts->fcaddr;

	list_for_each_entry_safe(lport, lnext, &fcloop_lports, list) {
		if (lport->localport->fabric_name == opts->fabric)
			break;
	}
	if (is_end_of_list(lport, &fcloop_lports, list)) {
		ret = -ENOENT;
		goto out_free_opts;
	}

	ret = nvme_fc_register_remoteport(lport->localport, &pinfo,
					&remoteport);
	if (ret)
		goto out_free_opts;

	/* success */
	rport = remoteport->private;
	rport->remoteport = remoteport;
	INIT_LIST_HEAD(&rport->list);
	list_add_tail(&rport->list, &lport->rport_list);

	/* tie into nvme target side */
	ret = nvmet_fc_register_targetport(&tinfo, &tgttemplate, NULL,
					&rport->targetport);
	if (ret) {
		list_del(&rport->list);
		(void)nvme_fc_unregister_remoteport(remoteport);
	} else {
		struct fcloop_tgtport *tport;

		tport = rport->targetport->private;
		tport->rport = rport;
		tport->lport = lport;
		tport->tgtport = rport->targetport;

		/* mark all of the input buffer consumed */
		ret = count;
	}

out_free_opts:
	kfree(opts);
	return ret;
}

static int __delete_remote_port(struct fcloop_rport *rport)
{
	int ret;

	ret = nvmet_fc_unregister_targetport(rport->targetport);
	if (ret)
		return ret;

	list_del(&rport->list);

	ret = nvme_fc_unregister_remoteport(rport->remoteport);

	return ret;
}

static ssize_t
fcloop_delete_remote_port(struct device *dev, struct device_attribute *attr,
		const char *buf, size_t count)
{
	struct fcloop_lport *lport, *lnext;
	struct fcloop_rport *rport, *rnext;
	u64 fabric, portname;
	int ret;

	ret = fcloop_parse_nm_options(dev, &fabric, &portname, buf);
	if (ret)
		return ret;

	list_for_each_entry_safe(lport, lnext, &fcloop_lports, list)
		if (lport->localport->fabric_name == fabric)
			break;
	if (is_end_of_list(lport, &fcloop_lports, list))
		return -ENOENT;

	list_for_each_entry_safe(rport, rnext, &lport->rport_list, list)
		if (rport->remoteport->port_name == portname)
			break;
	if (is_end_of_list(rport, &lport->rport_list, list))
		return -ENOENT;

	ret = __delete_remote_port(rport);
	if (!ret)
		return count;

	return ret;
}


static DEVICE_ATTR(add_local_port, S_IWUSR, NULL, fcloop_create_local_port);
static DEVICE_ATTR(del_local_port, S_IWUSR, NULL, fcloop_delete_local_port);
static DEVICE_ATTR(add_remote_port, S_IWUSR, NULL, fcloop_create_remote_port);
static DEVICE_ATTR(del_remote_port, S_IWUSR, NULL, fcloop_delete_remote_port);


static struct class *fcloop_class;
static struct device *fcloop_device;


static int __init fcloop_init(void)
{
	int ret;

	fcloop_class = class_create(THIS_MODULE, "fcloop");
	if (IS_ERR(fcloop_class)) {
		pr_err("couldn't register class fcloop\n");
		ret = PTR_ERR(fcloop_class);
		return ret;
	}

	fcloop_device =
		device_create(fcloop_class, NULL, MKDEV(0, 0), NULL, "ctl");
	if (IS_ERR(fcloop_device)) {
		pr_err("couldn't create ctl device!\n");
		ret = PTR_ERR(fcloop_device);
		goto out_destroy_class;
	}

	ret = device_create_file(fcloop_device, &dev_attr_add_local_port);
	if (ret) {
		pr_err("couldn't add device add_local_port attr.\n");
		goto out_destroy_device;
	}

	ret = device_create_file(fcloop_device, &dev_attr_del_local_port);
	if (ret) {
		pr_err("couldn't add device del_local_port attr.\n");
		goto out_destroy_device;
	}

	ret = device_create_file(fcloop_device, &dev_attr_add_remote_port);
	if (ret) {
		pr_err("couldn't add device add_remote_port attr.\n");
		goto out_destroy_device;
	}

	ret = device_create_file(fcloop_device, &dev_attr_del_remote_port);
	if (ret) {
		pr_err("couldn't add device del_remote_port attr.\n");
		goto out_destroy_device;
	}

	return 0;

out_destroy_device:
	device_destroy(fcloop_class, MKDEV(0, 0));
out_destroy_class:
	class_destroy(fcloop_class);
	return ret;
}

static void __exit fcloop_exit(void)
{
	struct fcloop_lport *lport, *lnext;
	struct fcloop_rport *rport, *rnext;
	int ret;

	list_for_each_entry_safe(lport, lnext, &fcloop_lports, list) {
		list_for_each_entry_safe(rport, rnext,
						&lport->rport_list, list) {
			ret = __delete_remote_port(rport);
			if (ret)
				pr_warn("%s: Failed deleting remote port\n",
						__func__);

		}
		ret = __delete_local_port(lport);
		if (ret)
			pr_warn("%s: Failed deleting local port\n", __func__);
	}

	device_destroy(fcloop_class, MKDEV(0, 0));
	class_destroy(fcloop_class);
}

module_init(fcloop_init);
module_exit(fcloop_exit);

MODULE_LICENSE("GPL v2");



