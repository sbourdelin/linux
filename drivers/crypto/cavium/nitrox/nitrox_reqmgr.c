#include <linux/gfp.h>

#include "nitrox_dev.h"
#include "nitrox_req.h"
#include "nitrox_csr.h"
#include "nitrox_req.h"

/* SLC_STORE_INFO */
#define MIN_UDD_LEN 16
/* PKT_IN_HDR + SLC_STORE_INFO */
#define FDATA_SIZE 32
/* Base destination port for the solicited requests */
#define SOLICIT_BASE_DPORT 256
#define DEFAULT_POLL_COUNT 512
#define PENDING_SIG	0xFFFFFFFFFFFFFFFFUL

/**
 * Response codes from SE microcode
 * 0x00 - Success
 *   Completion with no error
 * 0x43 - ERR_GC_DATA_LEN_INVALID
 *   Invalid Data length if Encryption Data length is
 *   less than 16 bytes for AES-XTS and AES-CTS.
 * 0x45 - ERR_GC_CTX_LEN_INVALID
 *   Invalid context length: CTXL != 23 words.
 * 0x4F - ERR_GC_DOCSIS_CIPHER_INVALID
 *   DOCSIS support is enabled with other than
 *   AES/DES-CBC mode encryption.
 * 0x50 - ERR_GC_DOCSIS_OFFSET_INVALID
 *   Authentication offset is other than 0 with
 *   Encryption IV source = 0.
 *   Authentication offset is other than 8 (DES)/16 (AES)
 *   with Encryption IV source = 1
 * 0x51 - ERR_GC_CRC32_INVALID_SELECTION
 *   CRC32 is enabled for other than DOCSIS encryption.
 * 0x52 - ERR_GC_AES_CCM_FLAG_INVALID
 *   Invalid flag options in AES-CCM IV.
 */

/**
 * dma_free_sglist - unmap and free the sg lists.
 * @ndev: N5 device
 * @sgtbl: SG table
 */
static void dma_free_sglist(struct nitrox_device *ndev,
			    struct dma_sgtable *sgtbl)
{
	struct device *dev = DEV(ndev);
	struct io_sglist *sglist = sgtbl->sglist;
	int i;

	if (sgtbl->len)
		dma_unmap_single(dev, sgtbl->dma, sgtbl->len, sgtbl->dir);

	if (sglist) {
		for (i = 0; i < sglist->cnt; i++) {
			dma_unmap_single(dev, sglist->bufs[i].dma,
					 sglist->bufs[i].len, sgtbl->dir);
		}
	}
	kfree(sglist);
	kfree(sgtbl->sgcomp);

	sgtbl->sgcomp = NULL;
	sgtbl->len = 0;
}

/**
 * create_sg_component - create SG componets for N5 device.
 * @sr: Request structure
 * @sgtbl: SG table
 * @nr_comp: total number of components required
 *
 * Component structure
 *
 *   63     48 47     32 31    16 15      0
 *   --------------------------------------
 *   |   LEN0  |  LEN1  |  LEN2  |  LEN3  |
 *   |-------------------------------------
 *   |               PTR0                 |
 *   --------------------------------------
 *   |               PTR1                 |
 *   --------------------------------------
 *   |               PTR2                 |
 *   --------------------------------------
 *   |               PTR3                 |
 *   --------------------------------------
 *
 *   Returns 0 if success or a negative errno code on error.
 */
static int create_sg_component(struct nitrox_softreq *sr,
			       struct dma_sgtable *sgtbl, int nr_comp)
{
	struct nitrox_device *ndev = sr->ndev;
	struct sglist_component *sgcomp;
	struct nitrox_buffer *buf;
	dma_addr_t dma;
	size_t sz;
	u16 cnt;
	int i, j;

	/* each component holds 4 dma pointers */
	sz = (nr_comp * sizeof(struct sglist_component));
	sgcomp = kzalloc_node(sz, GFP_ATOMIC, dev_to_node(DEV(ndev)));
	if (!sgcomp)
		return -ENOMEM;

	dma = dma_map_single(DEV(ndev), sgcomp, sz, sgtbl->dir);
	if (dma_mapping_error(DEV(ndev), dma)) {
		kfree(sgcomp);
		return -ENOMEM;
	}
	sgtbl->nr_comp = nr_comp;
	sgtbl->sgcomp = sgcomp;
	sgtbl->dma = dma;
	sgtbl->len = sz;

	cnt = sgtbl->sglist->cnt;
	buf = &sgtbl->sglist->bufs[0];
	/* populate sg component */
	for (i = 0; i < nr_comp; i++) {
		struct sglist_component *comp = &sgcomp[i];

		for (j = 0; (j < 4) && cnt; j++, cnt--) {
			comp->len[j] = cpu_to_be16(buf->len);
			comp->dma[j] = cpu_to_be64(buf->dma);
			buf++;
		}
	}
	return 0;
}

/**
 * dma_map_inbufs - DMA map input sglist and creates sglist component
 *                  for N5 device.
 * @sr: Request structure
 * @req: Crypto request structre
 *
 * Returns 0 if successful or a negative errno code on error.
 */
static int dma_map_inbufs(struct nitrox_softreq *sr, struct crypto_request *req)
{
	struct device *dev = DEV(sr->ndev);
	struct io_sglist *in = req->in;
	struct io_sglist *sglist;
	dma_addr_t dma;
	size_t sz;
	int nr_comp, i, ret = 0;

	if (!in->cnt)
		return -EINVAL;

	sr->in.dir = DMA_TO_DEVICE;
	/* single pointer, send in direct dma mode */
	if (in->cnt == 1) {
		dma = dma_map_single(dev, in->bufs[0].addr, in->bufs[0].len,
				     DMA_TO_DEVICE);
		ret = dma_mapping_error(dev, dma);
		if (ret)
			return ret;

		sr->in.dma = dma;
		sr->in.len = in->bufs[0].len;
		sr->in.total_bytes = in->bufs[0].len;
		sr->in.map_cnt = 1;
	} else {
		/* creater gather component */
		sz = sizeof(*sglist) + (in->cnt * sizeof(struct nitrox_buffer));
		sglist = kzalloc(sz, GFP_ATOMIC);
		if (!sglist)
			return -ENOMEM;

		sr->in.sglist = sglist;
		sglist->cnt = in->cnt;

		for (i = 0; i < sglist->cnt; i++) {
			dma = dma_map_single(dev, in->bufs[i].addr,
					     in->bufs[i].len, DMA_TO_DEVICE);
			ret = dma_mapping_error(dev, dma);
			if (ret)
				goto inmap_err;
			sglist->bufs[i].dma = dma;
			sglist->bufs[i].len = in->bufs[i].len;
			sr->in.total_bytes += in->bufs[i].len;
			sr->in.map_cnt++;
		}
		/* create NITROX gather component */
		nr_comp = roundup(in->cnt, 4) / 4;

		ret = create_sg_component(sr, &sr->in, nr_comp);
		if (ret)
			goto inmap_err;
	}
	return 0;

inmap_err:
	dma_free_sglist(sr->ndev, &sr->in);
	return ret;
}

static int dma_map_outbufs(struct nitrox_softreq *sr,
			   struct crypto_request *req)
{
	struct device *dev = DEV(sr->ndev);
	struct io_sglist *out = req->out;
	struct io_sglist *sglist;
	struct nitrox_buffer *buf;
	dma_addr_t dma;
	size_t sz;
	int i, ret = 0, nr_comp;

	if (!out->cnt)
		return -EINVAL;

	/*
	 * Need two extra out pointers to hold
	 * response header and Completion bytes.
	 */
	sz = sizeof(*sglist) + ((out->cnt + 2) * sizeof(struct nitrox_buffer));
	sglist = kzalloc(sz, GFP_ATOMIC);
	if (!sglist)
		return -ENOMEM;

	sr->out.sglist = sglist;
	sr->out.dir = DMA_BIDIRECTIONAL;
	sglist->cnt = (out->cnt + 2);

	/* Response Header */
	dma = dma_map_single(dev, &sr->resp.orh, ORH_HLEN, DMA_BIDIRECTIONAL);
	ret = dma_mapping_error(dev, dma);
	if (ret) {
		kfree(sglist);
		return ret;
	}
	sglist->bufs[0].dma = dma;
	sglist->bufs[0].len = ORH_HLEN;
	sr->out.total_bytes = ORH_HLEN;
	sr->out.map_cnt = 1;

	buf = &sglist->bufs[1];
	for (i = 0; i < out->cnt; i++) {
		dma = dma_map_single(dev, out->bufs[i].addr,
				     out->bufs[i].len, DMA_BIDIRECTIONAL);
		ret = dma_mapping_error(dev, dma);
		if (ret)
			goto outmap_err;

		buf->dma = dma;
		buf->len = out->bufs[i].len;
		sr->out.total_bytes += out->bufs[i].len;
		sr->out.map_cnt++;
		buf++;
	}

	/* Completion code */
	dma = dma_map_single(dev, &sr->resp.completion, COMP_HLEN,
			     DMA_BIDIRECTIONAL);
	ret = dma_mapping_error(dev, dma);
	if (ret)
		goto outmap_err;

	buf->dma = dma;
	buf->len = COMP_HLEN;
	sr->out.total_bytes += COMP_HLEN;
	sr->out.map_cnt++;

	/* total out count: ORH + (req out cnt) + Completion bytes */
	nr_comp = roundup(out->cnt + 2, 4) / 4;

	ret = create_sg_component(sr, &sr->out, nr_comp);
	if (ret)
		goto outmap_err;
	return 0;

outmap_err:
	dma_free_sglist(sr->ndev, &sr->out);
	return ret;
}

static void soft_request_cleanup(struct nitrox_softreq *sr)
{
	dma_free_sglist(sr->ndev, &sr->in);
	dma_free_sglist(sr->ndev, &sr->out);
	kfree(sr);
}

/**
 * post_se_instr - Post SE instruction to Packet Input ring
 * @sr: Request structure
 *
 * Returns 0 if successful or a negative error code,
 * if no space in ring.
 */
static inline int post_se_instr(struct nitrox_softreq *sr)
{
	struct nitrox_device *ndev = sr->ndev;
	struct nitrox_cmdq *cmdq = sr->cmdq;
	u8 *ent;
	int index;

	/* check for command queue space */
	if (atomic_inc_return(&cmdq->pending_count) > ndev->qlen) {
		atomic_dec(&cmdq->pending_count);
		/* barrier to sync with other cpus */
		smp_mb__after_atomic();
		return -EBUSY;
	}

	spin_lock_bh(&cmdq->cmdq_lock);

	index = cmdq->write_index;
	ent = cmdq->head + (index * cmdq->instr_size);
	memcpy(ent, &sr->instr, cmdq->instr_size);
	/* get the timestamp */
	sr->tstamp = jiffies;

	/* add request to in progress list */
	spin_lock_bh(&cmdq->pending_lock);
	list_add_tail(&sr->in_progress, &cmdq->in_progress_head);
	spin_unlock_bh(&cmdq->pending_lock);

	/* Ring doorbell with count 1 */
	writeq(1, cmdq->dbell_csr_addr);

	cmdq->write_index++;
	if (cmdq->write_index == ndev->qlen)
		cmdq->write_index = 0;

	spin_unlock_bh(&cmdq->cmdq_lock);
	return 0;
}

static inline void add_to_backlog_list(struct nitrox_softreq *sr)
{
	struct nitrox_cmdq *cmdq = sr->cmdq;

	INIT_LIST_HEAD(&sr->backlog);
	spin_lock_bh(&cmdq->backlog_lock);
	list_add_tail(&sr->backlog, &cmdq->backlog_head);
	spin_unlock_bh(&cmdq->backlog_lock);
}

/**
 * nitrox_se_request - Send request to SE core
 * @ndev: NITROX device
 * @req: Crypto request
 *
 * Returns 0 on success, or a negative error code.
 */
int nitrox_se_request(struct nitrox_device *ndev, struct crypto_request *req)
{
	struct nitrox_softreq *sr;
	dma_addr_t ctx_handle = 0;
	int qno, ret = 0;
	gfp_t gfp;

	if (!nitrox_ready(ndev))
		return -ENODEV;

	gfp = (req->flags & CRYPTO_TFM_REQ_MAY_SLEEP) ? GFP_KERNEL : GFP_ATOMIC;
	sr = kzalloc(sizeof(*sr), gfp);
	if (!sr)
		return -ENOMEM;

	INIT_LIST_HEAD(&sr->in_progress);
	sr->ndev = ndev;

	WRITE_ONCE(sr->resp.orh, PENDING_SIG);
	WRITE_ONCE(sr->resp.completion, PENDING_SIG);

	/* map input sg list */
	ret = dma_map_inbufs(sr, req);
	if (ret) {
		kfree(sr);
		return ret;
	}

	/* map output sg list */
	ret = dma_map_outbufs(sr, req);
	if (ret)
		goto send_fail;

	sr->callback = req->callback;
	sr->cb_arg = req->cb_arg;

	/* get the context handle */
	if (req->ctx_handle) {
		struct ctx_hdr *hdr;
		u8 *ctx_ptr;

		ctx_ptr = (u8 *)(uintptr_t)req->ctx_handle;
		hdr = (struct ctx_hdr *)(ctx_ptr - sizeof(struct ctx_hdr));
		ctx_handle = hdr->ctx_dma;
	}

	/* select the queue */
	qno = smp_processor_id() % ndev->nr_queues;

	/*
	 * 64-Byte Instruction Format
	 *
	 *  ----------------------
	 *  |      DPTR0         | 8 bytes
	 *  ----------------------
	 *  |  PKT_IN_INSTR_HDR  | 8 bytes
	 *  ----------------------
	 *  |    PKT_IN_HDR      | 16 bytes
	 *  ----------------------
	 *  |    SLC_INFO        | 16 bytes
	 *  ----------------------
	 *  |   Front data       | 16 bytes
	 *  ----------------------
	 */

	/* fill the packet instruction */
	/* word 0 */
	sr->instr.dptr0 = cpu_to_be64(sr->in.dma);

	/* word 1 */
	sr->instr.ih.value = 0;
	sr->instr.ih.s.g = (sr->in.nr_comp) ? 1 : 0;
	sr->instr.ih.s.gsz = sr->in.map_cnt;
	sr->instr.ih.s.ssz = sr->out.map_cnt;
	sr->instr.ih.s.fsz = FDATA_SIZE + sizeof(struct gphdr);
	sr->instr.ih.s.tlen = sr->instr.ih.s.fsz + sr->in.total_bytes;
	sr->instr.ih.value = cpu_to_be64(sr->instr.ih.value);

	/* word 2 */
	sr->instr.irh.value[0] = 0;
	sr->instr.irh.s.uddl = MIN_UDD_LEN;
	/* context length in 64-bit words */
	sr->instr.irh.s.ctxl = (req->ctrl.s.ctxl / 8);
	/* offset from solicit base port 256 */
	sr->instr.irh.s.destport = SOLICIT_BASE_DPORT + qno;
	sr->instr.irh.s.ctxc = req->ctrl.s.ctxc;
	sr->instr.irh.s.arg = req->ctrl.s.arg;
	sr->instr.irh.s.opcode = req->opcode;
	sr->instr.irh.value[0] = cpu_to_be64(sr->instr.irh.value[0]);

	/* word 3 */
	sr->instr.irh.s.ctxp = cpu_to_be64(ctx_handle);

	/* word 4 */
	sr->instr.slc.value[0] = 0;
	sr->instr.slc.s.ssz = sr->out.map_cnt;
	sr->instr.slc.value[0] = cpu_to_be64(sr->instr.slc.value[0]);

	/* word 5 */
	sr->instr.slc.s.rptr = cpu_to_be64(sr->out.dma);

	/*
	 * No conversion for front data,
	 * It goes into payload
	 * put GP Header in front data
	 */
	sr->instr.fdata[0] = *((u64 *)&req->gph);
	sr->instr.fdata[1] = 0;

	sr->cmdq = &ndev->pkt_cmdqs[qno];
	/* post instruction to device */
	ret = post_se_instr(sr);
	if (ret) {
		if (!(req->flags & CRYPTO_TFM_REQ_MAY_BACKLOG))
			goto send_fail;
		add_to_backlog_list(sr);
	}
	return 0;

send_fail:
	soft_request_cleanup(sr);
	return ret;
}

static inline int cmd_timeout(unsigned long tstamp, unsigned long timeout)
{
	return time_after_eq(jiffies, (tstamp + timeout));
}

/**
 * process_request_list - process completed requests
 * @ndev: N5 device
 * @qno: queue to operate
 *
 * Returns the number of responses processed.
 */
static u32 process_request_list(struct nitrox_device *ndev, int qno)
{
	struct nitrox_cmdq *cmdq = &ndev->pkt_cmdqs[qno];
	struct nitrox_softreq *sr, *tmp;
	void (*callback)(int, void *);
	void *cb_arg;
	u64 status;
	u32 req_completed = 0;

	while (req_completed < DEFAULT_POLL_COUNT) {
		sr = list_first_entry_or_null(&cmdq->in_progress_head,
					      struct nitrox_softreq,
					      in_progress);
		if (!sr)
			break;

		/* check both orh and completion bytes */
		if (READ_ONCE(sr->resp.orh) == READ_ONCE(sr->resp.completion)) {
			/* request not completed, check for timeout */
			if (!cmd_timeout(sr->tstamp, ndev->timeout))
				break;
			dev_err_ratelimited(DEV(ndev),
					    "Request timeout, orh 0x%016llx\n",
					    sr->resp.orh);
		}
		atomic_dec(&cmdq->pending_count);
		/* barrier to sync with other cpus */
		smp_mb__after_atomic();

		/* remove completed request */
		spin_lock_bh(&cmdq->pending_lock);
		list_del(&sr->in_progress);
		spin_unlock_bh(&cmdq->pending_lock);

		dma_free_sglist(sr->ndev, &sr->in);
		dma_free_sglist(sr->ndev, &sr->out);

		/* ORH status code */
		status = READ_ONCE(sr->resp.orh);

		callback = sr->callback;
		cb_arg = sr->cb_arg;
		kfree(sr);

		if (callback)
			callback((u8)status, cb_arg);

		req_completed++;
	}

	/* submit any backlog requests until space available */
	spin_lock_bh(&cmdq->backlog_lock);
	list_for_each_entry_safe(sr, tmp, &cmdq->backlog_head, backlog) {
		if (post_se_instr(sr) == -EBUSY)
			break;
		list_del(&sr->backlog);
	}
	spin_unlock_bh(&cmdq->backlog_lock);

	return req_completed;
}

/**
 * pkt_slc_resp_handler - post processing of SE responses
 */
void pkt_slc_resp_handler(unsigned long data)
{
	struct bh_data *bh = (void *)(uintptr_t)(data);
	union nps_pkt_slc_cnts pkt_slc_cnts;
	u32 slc_cnt, req_completed;

	req_completed = process_request_list(bh->ndev, bh->qno);
	/* read completion count */
	pkt_slc_cnts.value = readq(bh->completion_cnt_csr_addr);
	/* resend the interrupt if more work to do */
	pkt_slc_cnts.s.resend = 1;

	slc_cnt = pkt_slc_cnts.s.cnt;
	if (req_completed)
		pkt_slc_cnts.s.cnt = min(slc_cnt, req_completed);

	/*
	 * clear the interrupt with resend bit enabled,
	 * MSI-X interrupt generates if Completion count > Threshold
	 */
	writeq(pkt_slc_cnts.value, bh->completion_cnt_csr_addr);
}
