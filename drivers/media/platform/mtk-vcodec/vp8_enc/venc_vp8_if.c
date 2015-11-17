/*
 * Copyright (c) 2015 MediaTek Inc.
 * Author: Daniel Hsiao <daniel.hsiao@mediatek.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/time.h>

#include "mtk_vcodec_drv.h"
#include "mtk_vcodec_util.h"
#include "mtk_vcodec_intr.h"
#include "mtk_vcodec_enc.h"
#include "mtk_vcodec_pm.h"
#include "mtk_vpu_core.h"

#include "venc_vp8_if.h"
#include "venc_vp8_vpu.h"

#define vp8_enc_write_reg(h, addr, val)	writel(val, h->hw_base + addr)
#define vp8_enc_read_reg(h, addr) readl(h->hw_base + addr)

#define VENC_PIC_BITSTREAM_BYTE_CNT 0x0098
#define VENC_PIC_BITSTREAM_BYTE_CNT1 0x00e8
#define VENC_IRQ_STATUS_ENC_FRM_INT 0x04

#define MAX_AC_TAG_SZ 10

static void vp8_enc_free_work_buf(struct venc_vp8_handle *hndl)
{
	int i;

	mtk_vcodec_debug_enter(hndl);

	/* Except the RC_CODEx buffers, other buffers need to be freed by AP. */
	for (i = 0; i < VENC_VP8_VPU_WORK_BUF_RC_CODE; i++)
		if (hndl->work_bufs[i].va != NULL)
			mtk_vcodec_mem_free(hndl->ctx, &hndl->work_bufs[i]);

	mtk_vcodec_debug_leave(hndl);
}

static int vp8_enc_alloc_work_buf(struct venc_vp8_handle *hndl)
{
	int i;
	int ret = 0;
	struct venc_vp8_vpu_buf *wb = hndl->vpu_inst.drv->work_bufs;

	mtk_vcodec_debug_enter(hndl);
	for (i = 0; i < VENC_VP8_VPU_WORK_BUF_MAX; i++) {
		/*
		 * Only temporal scalability mode will use RC_CODE2 & RC_CODE3
		 * Each three temporal layer has its own rate control code.
		 */
		if ((i == VENC_VP8_VPU_WORK_BUF_RC_CODE2 ||
		     i == VENC_VP8_VPU_WORK_BUF_RC_CODE3) && !hndl->ts_mode)
			continue;

		/*
		 * This 'wb' structure is set by VPU side and shared to AP for
		 * buffer allocation and physical addr mapping. For most of
		 * the buffers, AP will allocate the buffer according to 'size'
		 * field and store the physical addr in 'pa' field. For the
		 * RC_CODEx buffers, they are pre-allocated in the VPU side
		 * because they are inside VPU SRAM, and save the VPU addr in
		 * the 'vpua' field. The AP will translate the VPU addr to the
		 * corresponding physical addr and store in 'pa' field.
		 */
		if (i < VENC_VP8_VPU_WORK_BUF_RC_CODE) {
			hndl->work_bufs[i].size = wb[i].size;
			ret = mtk_vcodec_mem_alloc(hndl->ctx,
						   &hndl->work_bufs[i]);
			if (ret) {
				mtk_vcodec_err(hndl,
					       "cannot alloc work_bufs[%d]", i);
				goto err_alloc;
			}
			wb[i].pa = hndl->work_bufs[i].dma_addr;

			mtk_vcodec_debug(hndl,
					 "work_bufs[%d] va=0x%p,pa=0x%p,size=0x%lx",
					 i, hndl->work_bufs[i].va,
					 (void *)hndl->work_bufs[i].dma_addr,
					 hndl->work_bufs[i].size);
		} else {
			hndl->work_bufs[i].size = wb[i].size;
			hndl->work_bufs[i].va =
				vpu_mapping_dm_addr(hndl->dev,
						    (uintptr_t *)
						    (unsigned long)wb[i].vpua);
			hndl->work_bufs[i].dma_addr =
				(dma_addr_t)vpu_mapping_iommu_dm_addr(hndl->dev,
					(uintptr_t *)(unsigned long)wb[i].vpua);
			wb[i].pa = hndl->work_bufs[i].dma_addr;
		}
	}
	mtk_vcodec_debug_leave(hndl);

	return ret;

err_alloc:
	vp8_enc_free_work_buf(hndl);
	return ret;
}

static unsigned int vp8_enc_wait_venc_done(struct venc_vp8_handle *hndl)
{
	struct mtk_vcodec_ctx *ctx = (struct mtk_vcodec_ctx *)hndl->ctx;
	unsigned int irq_status;

	mtk_vcodec_wait_for_done_ctx(ctx, MTK_INST_IRQ_RECEIVED, 1000, true);
	irq_status = ctx->irq_status;
	mtk_vcodec_debug(hndl, "isr return %x", irq_status);

	return irq_status;
}

/*
 * Compose ac_tag, bitstream header and bitstream payload into
 * one bitstream buffer.
 */
static int vp8_enc_compose_one_frame(struct venc_vp8_handle *hndl,
				     struct mtk_vcodec_mem *bs_buf,
				     unsigned int *bs_size)
{
	unsigned int is_key;
	u32 bs_size_frm;
	u32 bs_hdr_len;
	unsigned int ac_tag_sz;
	u8 ac_tag[MAX_AC_TAG_SZ];

	bs_size_frm = vp8_enc_read_reg(hndl,
				       VENC_PIC_BITSTREAM_BYTE_CNT);
	bs_hdr_len = vp8_enc_read_reg(hndl,
				      VENC_PIC_BITSTREAM_BYTE_CNT1);

	/* if a frame is key frame, is_key is 0 */
	is_key = (hndl->frm_cnt %
		  hndl->vpu_inst.drv->config.intra_period) ? 1 : 0;
	*(u32 *)ac_tag = __cpu_to_le32((bs_hdr_len << 5) | 0x10 | is_key);
	/* key frame */
	if (is_key == 0) {
		ac_tag[3] = 0x9d;
		ac_tag[4] = 0x01;
		ac_tag[5] = 0x2a;
		ac_tag[6] = hndl->vpu_inst.drv->config.pic_w;
		ac_tag[7] = hndl->vpu_inst.drv->config.pic_w >> 8;
		ac_tag[8] = hndl->vpu_inst.drv->config.pic_h;
		ac_tag[9] = hndl->vpu_inst.drv->config.pic_h >> 8;
	}

	if (is_key == 0)
		ac_tag_sz = MAX_AC_TAG_SZ;
	else
		ac_tag_sz = 3;

	if (bs_buf->size <= bs_hdr_len + bs_size_frm + ac_tag_sz) {
		mtk_vcodec_err(hndl, "bitstream buf size is too small(%ld)",
			       bs_buf->size);
		return -EINVAL;
	}

	/*
	* (1) The vp8 bitstream header and body are generated by the HW vp8
	* encoder separately at the same time. We cannot know the bitstream
	* header length in advance.
	* (2) From the vp8 spec, there is no stuffing byte allowed between the
	* ac tag, bitstream header and bitstream body.
	*/
	memmove(bs_buf->va + bs_hdr_len + ac_tag_sz,
		bs_buf->va, bs_size_frm);
	memcpy(bs_buf->va + ac_tag_sz,
	       hndl->work_bufs[VENC_VP8_VPU_WORK_BUF_BS_HD].va,
	       bs_hdr_len);
	memcpy(bs_buf->va, ac_tag, ac_tag_sz);
	*bs_size = bs_size_frm + bs_hdr_len + ac_tag_sz;

	return 0;
}

static int vp8_enc_encode_frame(struct venc_vp8_handle *hndl,
				struct venc_frm_buf *frm_buf,
				struct mtk_vcodec_mem *bs_buf,
				unsigned int *bs_size)
{
	int ret = 0;
	unsigned int irq_status;

	mtk_vcodec_debug(hndl, "->frm_cnt=%d", hndl->frm_cnt);

	ret = vp8_enc_vpu_encode(hndl, frm_buf, bs_buf);

	irq_status = vp8_enc_wait_venc_done(hndl);
	if (irq_status != VENC_IRQ_STATUS_ENC_FRM_INT) {
		mtk_vcodec_err(hndl, "irq_status=%d failed", irq_status);
		return -EINVAL;
	}

	if (vp8_enc_compose_one_frame(hndl, bs_buf, bs_size)) {
		mtk_vcodec_err(hndl, "vp8_enc_compose_one_frame failed");
		return -EINVAL;
	}

	hndl->frm_cnt++;
	mtk_vcodec_debug(hndl, "<-size=%d", *bs_size);

	return ret;
}

int vp8_enc_init(struct mtk_vcodec_ctx *ctx, unsigned long *handle)
{
	int ret = 0;
	struct venc_vp8_handle *h;

	h = kzalloc(sizeof(*h), GFP_KERNEL);
	if (!h)
		return -ENOMEM;

	h->ctx = ctx;
	h->dev = mtk_vcodec_get_plat_dev(ctx);
	h->hw_base = mtk_vcodec_get_reg_addr(h->ctx, VENC_LT_SYS);

	ret = vp8_enc_vpu_init(h);
	if (ret)
		kfree(h);
	else
		(*handle) = (unsigned long)h;

	return ret;
}

int vp8_enc_encode(unsigned long handle,
		   enum venc_start_opt opt,
		   struct venc_frm_buf *frm_buf,
		   struct mtk_vcodec_mem *bs_buf,
		   struct venc_done_result *result)
{
	int ret = 0;
	struct venc_vp8_handle *h = (struct venc_vp8_handle *)handle;

	mtk_vcodec_debug_enter(h);

	switch (opt) {
	case VENC_START_OPT_ENCODE_FRAME:
		ret = vp8_enc_encode_frame(h, frm_buf, bs_buf,
					   &result->bs_size);
		if (ret) {
			result->msg = VENC_MESSAGE_ERR;
		} else {
			result->msg = VENC_MESSAGE_OK;
			result->is_key_frm = ((*((unsigned char *)bs_buf->va) &
					       0x01) == 0);
		}
		break;

	default:
		mtk_vcodec_err(h, "opt not support:%d", opt);
		ret = -EINVAL;
		break;
	}

	mtk_vcodec_debug_leave(h);
	return ret;
}

int vp8_enc_set_param(unsigned long handle,
		      enum venc_set_param_type type, void *in)
{
	int ret = 0;
	struct venc_vp8_handle *h = (struct venc_vp8_handle *)handle;
	struct venc_enc_prm *enc_prm;

	mtk_vcodec_debug(h, "->type=%d", type);

	switch (type) {
	case VENC_SET_PARAM_ENC:
		enc_prm = in;
		ret = vp8_enc_vpu_set_param(h, type, enc_prm);
		if (ret)
			break;
		if (h->work_buf_allocated == 0) {
			ret = vp8_enc_alloc_work_buf(h);
			if (ret)
				break;
			h->work_buf_allocated = 1;
		}
		break;

	case VENC_SET_PARAM_FORCE_INTRA:
		ret = vp8_enc_vpu_set_param(h, type, 0);
		if (ret)
			break;
		h->frm_cnt = 0;
		break;

	case VENC_SET_PARAM_ADJUST_BITRATE:
		enc_prm = in;
		ret = vp8_enc_vpu_set_param(h, type, &enc_prm->bitrate);
		break;

	case VENC_SET_PARAM_ADJUST_FRAMERATE:
		enc_prm = in;
		ret = vp8_enc_vpu_set_param(h, type, &enc_prm->frm_rate);
		break;

	case VENC_SET_PARAM_I_FRAME_INTERVAL:
		ret = vp8_enc_vpu_set_param(h, type, in);
		if (ret)
			break;
		h->frm_cnt = 0; /* reset counter */
		break;

	/*
	 * VENC_SET_PARAM_TS_MODE must be called before
	 * VENC_SET_PARAM_ENC
	 */
	case VENC_SET_PARAM_TS_MODE:
		h->ts_mode = 1;
		mtk_vcodec_debug(h, "set ts_mode");
		break;

	default:
		mtk_vcodec_err(h, "type not support:%d", type);
		ret = -EINVAL;
		break;
	}

	mtk_vcodec_debug_leave(h);
	return ret;
}

int vp8_enc_deinit(unsigned long handle)
{
	int ret = 0;
	struct venc_vp8_handle *h = (struct venc_vp8_handle *)handle;

	mtk_vcodec_debug_enter(h);

	ret = vp8_enc_vpu_deinit(h);

	if (h->work_buf_allocated)
		vp8_enc_free_work_buf(h);

	mtk_vcodec_debug_leave(h);
	kfree(h);

	return ret;
}

struct venc_common_if venc_vp8_if = {
	vp8_enc_init,
	vp8_enc_encode,
	vp8_enc_set_param,
	vp8_enc_deinit,
};

struct venc_common_if *get_vp8_enc_comm_if(void)
{
	return &venc_vp8_if;
}
