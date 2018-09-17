// SPDX-License-Identifier: GPL-2.0
/*
 * bsg endpoint that supports UPIUs
 *
 * Copyright (C) 2018 Western Digital Corporation
 */
#include "ufs_bsg.h"


static inline struct ufs_hba *dev_to_ufs_hba(struct device *d)
{
	struct Scsi_Host *shost = dev_to_shost(d->parent);

	return shost_priv(shost);
}

static int ufs_bsg_get_query_desc_size(struct ufs_hba *hba,
				       struct utp_upiu_query *qr,
				       int *desc_len)
{
	int desc_size = be16_to_cpu(qr->length);
	int desc_id = qr->idn;
	int ret = 0;

	if (qr->opcode != UPIU_QUERY_OPCODE_WRITE_DESC || desc_size <= 0)
		return -EINVAL;

	ret = ufshcd_map_desc_id_to_length(hba, desc_id, desc_len);

	if (ret || !*desc_len)
		return -EINVAL;

	*desc_len = min_t(int, *desc_len, desc_size);

	return ret;
}

static int ufs_bsg_verify_query_size(unsigned int request_len,
				     unsigned int reply_len,
				     int rw, int desc_len)
{
	int min_req_len = sizeof(struct ufs_bsg_request);
	int min_rsp_len = sizeof(struct ufs_bsg_reply);

	if (rw == WRITE)
		min_req_len += desc_len;

	if (min_req_len > request_len)
		return -EINVAL;

	if (min_rsp_len > reply_len)
		return -EINVAL;

	return 0;
}

static int ufs_bsg_exec_uic_cmd(struct ufs_hba *hba, struct uic_command *uc)
{
	u32 attr_sel = uc->argument1;
	u8 attr_set = (uc->argument2 >> 16) & 0xff;
	u32 mib_val = uc->argument3;
	int cmd = uc->command;
	int ret = 0;

	switch (cmd) {
	case UIC_CMD_DME_GET:
		ret = ufshcd_dme_get_attr(hba, attr_sel, &mib_val, DME_LOCAL);
		break;
	case UIC_CMD_DME_SET:
		ret = ufshcd_dme_set_attr(hba, attr_sel, attr_set, mib_val,
					  DME_LOCAL);
		break;
	case UIC_CMD_DME_PEER_GET:
		ret = ufshcd_dme_get_attr(hba, attr_sel, &mib_val, DME_PEER);
		break;
	case UIC_CMD_DME_PEER_SET:
		ret = ufshcd_dme_set_attr(hba, attr_sel, attr_set, mib_val,
					  DME_PEER);
		break;
	case UIC_CMD_DME_POWERON:
	case UIC_CMD_DME_POWEROFF:
	case UIC_CMD_DME_ENABLE:
	case UIC_CMD_DME_RESET:
	case UIC_CMD_DME_END_PT_RST:
	case UIC_CMD_DME_LINK_STARTUP:
	case UIC_CMD_DME_HIBER_ENTER:
	case UIC_CMD_DME_HIBER_EXIT:
	case UIC_CMD_DME_TEST_MODE:
		ret = -ENOTSUPP;
		pr_err("%s unsupported command 0x%x\n", __func__, cmd);
		break;
	default:
		ret = -EINVAL;
	}

	if (ret)
		pr_err("%s error in command 0x%x\n", __func__, cmd);

	uc->argument3 = mib_val;

	return ret;
}

static int ufs_bsg_request(struct bsg_job *job)
{
	struct ufs_bsg_request *bsg_request = job->request;
	struct ufs_bsg_reply *bsg_reply = job->reply;
	struct ufs_hba *hba = dev_to_ufs_hba(job->dev);
	unsigned int request_len = job->request_len;
	unsigned int reply_len = job->reply_len;
	struct utp_upiu_query *qr;
	struct uic_command uc = {};
	struct utp_upiu_req *req_upiu = NULL;
	struct utp_upiu_req *rsp_upiu = NULL;
	int msgcode;
	uint8_t *desc_buff = NULL;
	int desc_len = 0;
	int rw = UFS_BSG_NOP;
	int ret;

	ret = ufs_bsg_verify_query_size(request_len, reply_len, rw, desc_len);
	if (ret) {
		dev_err(job->dev, "not enough space assigned\n");
		goto out;
	}
	bsg_reply->reply_payload_rcv_len = 0;

	msgcode = bsg_request->msgcode;
	switch (msgcode) {
	case UPIU_TRANSACTION_QUERY_REQ:
		qr = &bsg_request->tsf.qr;
		if (qr->opcode == UPIU_QUERY_OPCODE_READ_DESC)
			goto not_supported;

		if (ufs_bsg_get_query_desc_size(hba, qr, &desc_len))
			goto null_desc_buff;

		if (qr->opcode == UPIU_QUERY_OPCODE_WRITE_DESC) {
			rw = WRITE;
			desc_buff = ((uint8_t *)bsg_request) +
				    sizeof(struct ufs_bsg_request);
		}

null_desc_buff:
		/* fall through */
	case UPIU_TRANSACTION_NOP_OUT:
	case UPIU_TRANSACTION_TASK_REQ:
		/* Now that we know if its a read or write, verify again */
		if (rw != UFS_BSG_NOP || desc_len) {
			ret = ufs_bsg_verify_query_size(request_len, reply_len,
							rw, desc_len);
			if (ret) {
				dev_err(job->dev,
					"not enough space assigned\n");
				goto out;
			}
		}

		req_upiu = (struct utp_upiu_req *)&bsg_request->header;
		rsp_upiu = (struct utp_upiu_req *)&bsg_reply->header;
		ret = ufshcd_exec_raw_upiu_cmd(hba, req_upiu, rsp_upiu,
					       msgcode, desc_buff, &desc_len,
					       rw);

		break;
	case UPIU_TRANSACTION_UIC_CMD:
		memcpy(&uc, &bsg_request->tsf.uc, UIC_CMD_SIZE);
		ret = ufs_bsg_exec_uic_cmd(hba, &uc);
		memcpy(&bsg_reply->tsf.uc, &uc, UIC_CMD_SIZE);

		break;
	case UPIU_TRANSACTION_COMMAND:
	case UPIU_TRANSACTION_DATA_OUT:
not_supported:
		/* for the time being, we do not support data transfer upiu */
		ret = -ENOTSUPP;
		dev_err(job->dev, "unsupported msgcode 0x%x\n", msgcode);

		break;
	default:
		ret = -EINVAL;

		break;
	}

out:
	bsg_reply->result = ret;
	job->reply_len = sizeof(struct ufs_bsg_reply) +
			 bsg_reply->reply_payload_rcv_len;

	bsg_job_done(job, ret, bsg_reply->reply_payload_rcv_len);

	return ret;
}

/**
 * ufs_bsg_remove - detach and remove the added ufs-bsg node
 *
 * Should be called when unloading the driver.
 */
void ufs_bsg_remove(struct ufs_hba *hba)
{
	struct device *bsg_dev = &hba->bsg_dev;

	if (!hba->bsg_queue)
		return;

	bsg_unregister_queue(hba->bsg_queue);

	device_del(bsg_dev);
	put_device(bsg_dev);
}

static inline void ufs_bsg_node_release(struct device *dev)
{
	put_device(dev->parent);
}

/**
 * ufs_bsg_probe - Add ufs bsg device node
 * @hba: per adapter object
 *
 * Called during initial loading of the driver, and before scsi_scan_host.
 */
int ufs_bsg_probe(struct ufs_hba *hba)
{
	struct device *bsg_dev = &hba->bsg_dev;
	struct Scsi_Host *shost = hba->host;
	struct device *parent = &shost->shost_gendev;
	struct request_queue *q;
	int ret;

	device_initialize(bsg_dev);

	bsg_dev->parent = get_device(parent);
	bsg_dev->release = ufs_bsg_node_release;

	dev_set_name(bsg_dev, "ufs-bsg-%d:0", shost->host_no);

	ret = device_add(bsg_dev);
	if (ret)
		goto out;

	q = bsg_setup_queue(bsg_dev, dev_name(bsg_dev), ufs_bsg_request, 0);
	if (IS_ERR(q)) {
		ret = PTR_ERR(q);
		goto out;
	}

	hba->bsg_queue = q;

	return 0;

out:
	dev_err(bsg_dev, "fail to initialize a bsg dev %d\n", shost->host_no);
	put_device(bsg_dev);
	return ret;
}
