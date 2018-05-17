/*
 *Copyright © 2018 Intel Corp
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that copyright
 * notice and this permission notice appear in supporting documentation, and
 * that the name of the copyright holders not be used in advertising or
 * publicity pertaining to distribution of the software without specific,
 * written prior permission.  The copyright holders make no representations
 * about the suitability of this software for any purpose.  It is provided "as
 * is" without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO
 * EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY SPECIAL, INDIRECT OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE,
 * DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE
 * OF THIS SOFTWARE.
 *
 * Author:
 * Manasi Navare <manasi.d.navare@intel.com>
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/errno.h>
#include <drm/drm_dp_helper.h>
#include <drm/drm_dsc.h>

/**
 * DOC: dsc helpers
 *
 * These functions contain some common logic and helpers to deal with VESA
 * Display Stream Compression standard required for DSC on Display Port/eDP or
 * MIPI display interfaces.
 */

/**
 * drm_dsc_dp_pps_header_init() - Initializes the PPS Header
 * for DisplayPort as per the DP 1.4 spec.
 * @pps_sdp: Secondary data packet for DSC Picture Parameter Set
 */
void drm_dsc_dp_pps_header_init(struct drm_dsc_pps_infoframe *pps_sdp)
{
	memset(&pps_sdp->pps_header, 0, sizeof(pps_sdp->pps_header));

	pps_sdp->pps_header.HB1 = DP_SDP_PPS;
	pps_sdp->pps_header.HB2 = DP_SDP_PPS_HEADER_PAYLOAD_BYTES_MINUS_1;
}
EXPORT_SYMBOL(drm_dsc_dp_pps_header_init);

/**
 * drm_dsc_pps_infoframe_pack() - Populates the DSC PPS infoframe
 * using the DSC configuration parameters in the order expected
 * by the DSC Display Sink device. For the DSC, the sink device
 * expects the PPS payload in the big endian format for the fields
 * that span more than 1 byte.
 *
 * @pps_sdp:
 * Secondary data packet for DSC Picture Parameter Set
 * @dsc_cfg:
 * DSC Configuration data filled by driver
 * @is_big_endian:
 * Flag to indicate if HW is Big Endian
 */
void drm_dsc_pps_infoframe_pack(struct drm_dsc_pps_infoframe *pps_sdp,
				struct drm_dsc_config *dsc_cfg,
				bool is_big_endian)
{
	u8 i = 0;

	memset(&pps_sdp->pps_payload, 0, sizeof(pps_sdp->pps_payload));

	/* PPS 0 */
	pps_sdp->pps_payload.dsc_version_minor = dsc_cfg->dsc_version_minor;
	pps_sdp->pps_payload.dsc_version_major = dsc_cfg->dsc_version_major;

	/* PPS 1, 2 is 0 */

	/* PPS 3 */
	pps_sdp->pps_payload.linebuf_depth = dsc_cfg->line_buf_depth;
	pps_sdp->pps_payload.bits_per_component = dsc_cfg->bits_per_component;

	/* PPS 4, 5 */
	pps_sdp->pps_payload.block_pred_enable = (u8)dsc_cfg->block_pred_enable;
	pps_sdp->pps_payload.convert_rgb = (u8)dsc_cfg->convert_rgb;
	pps_sdp->pps_payload.simple_422 = (u8)dsc_cfg->enable422;
	pps_sdp->pps_payload.vbr_enable = (u8)dsc_cfg->vbr_enable;
	pps_sdp->pps_payload.bpp_high = (u8)((dsc_cfg->bits_per_pixel &
					      DSC_PPS_BPP_HIGH_MASK) >>
					     DSC_PPS_MSB_SHIFT);
	pps_sdp->pps_payload.bpp_low = (u8)(dsc_cfg->bits_per_pixel &
					    DSC_PPS_LSB_MASK);

	/*
	 * The DSC panel expects the PPS packet to have big endian format
	 * for data spanning 2 bytes. So if the HW does not store the data
	 * in big endian format, it sets big_endian flag to false in which case
	 * we need to convert from little endian to big endian.
	 */

	/* PPS 6, 7 */
	pps_sdp->pps_payload.pic_height = DSC_PPS_SWAP_BYTES(dsc_cfg->pic_height,
							     is_big_endian);

	/* PPS 8, 9 */
	pps_sdp->pps_payload.pic_width = DSC_PPS_SWAP_BYTES(dsc_cfg->pic_height,
							    is_big_endian);

	/* PPS 10, 11 */
	pps_sdp->pps_payload.slice_height = DSC_PPS_SWAP_BYTES(dsc_cfg->slice_height,
							       is_big_endian);

	/* PPS 12, 13 */
	pps_sdp->pps_payload.slice_width = DSC_PPS_SWAP_BYTES(dsc_cfg->slice_width,
							      is_big_endian);

	/* PPS 14, 15 */
	pps_sdp->pps_payload.chunk_size = DSC_PPS_SWAP_BYTES(dsc_cfg->slice_chunk_size,
							     is_big_endian);

	/* PPS 16, 17 */
	pps_sdp->pps_payload.initial_xmit_delay_high = (u8)((dsc_cfg->initial_xmit_delay &
							     DSC_PPS_INIT_XMIT_DELAY_HIGH_MASK) >>
							    DSC_PPS_MSB_SHIFT);
	pps_sdp->pps_payload.initial_xmit_delay_low = (u8)(dsc_cfg->initial_xmit_delay &
							   DSC_PPS_LSB_MASK);

	/* PPS 18, 19 */
	pps_sdp->pps_payload.initial_dec_delay = DSC_PPS_SWAP_BYTES(dsc_cfg->initial_dec_delay,
								    is_big_endian);

	/* PPS 20 is 0 */

	/* PPS 21 */
	pps_sdp->pps_payload.initial_scale_value = (u8)dsc_cfg->initial_scale_value;

	/* PPS 22, 23 */
	pps_sdp->pps_payload.scale_increment_interval = DSC_PPS_SWAP_BYTES(dsc_cfg->scale_increment_interval,
									   is_big_endian);

	/* PPS 24, 25 */
	pps_sdp->pps_payload.scale_decrement_interval_high = (u8)((dsc_cfg->scale_decrement_interval &
								   DSC_PPS_SCALE_DEC_INT_HIGH_MASK) >>
								  DSC_PPS_MSB_SHIFT);
	pps_sdp->pps_payload.scale_decrement_interval_low = (u8)(dsc_cfg->scale_decrement_interval &
								 DSC_PPS_LSB_MASK);

	/* PPS 27 */
	pps_sdp->pps_payload.first_line_bpg_offset = (u8)dsc_cfg->first_line_bpg_offset;

	/* PPS 28, 29 */
	pps_sdp->pps_payload.nfl_bpg_offset = DSC_PPS_SWAP_BYTES(dsc_cfg->nfl_bpg_offset,
								 is_big_endian);

	/* PPS 30, 31 */
	pps_sdp->pps_payload.slice_bpg_offset = DSC_PPS_SWAP_BYTES(dsc_cfg->slice_bpg_offset,
								   is_big_endian);

	/* PPS 32, 33 */
	pps_sdp->pps_payload.initial_offset = DSC_PPS_SWAP_BYTES(dsc_cfg->initial_offset,
								 is_big_endian);

	/* PPS 34, 35 */
	pps_sdp->pps_payload.final_offset = DSC_PPS_SWAP_BYTES(dsc_cfg->final_offset,
							       is_big_endian);

	/* PPS 36 */
	pps_sdp->pps_payload.flatness_min_qp = (u8)dsc_cfg->flatness_min_qp;

	/* PPS 37 */
	pps_sdp->pps_payload.flatness_max_qp = (u8)dsc_cfg->flatness_max_qp;

	/* PPS 38, 39 */
	pps_sdp->pps_payload.rc_model_size = DSC_PPS_SWAP_BYTES(dsc_cfg->rc_model_size,
								is_big_endian);

	/* PPS 40 */
	pps_sdp->pps_payload.rc_edge_factor = (u8)dsc_cfg->rc_edge_factor;

	/* PPS 41 */
	pps_sdp->pps_payload.rc_quant_incr_limit0 = (u8)dsc_cfg->rc_quant_incr_limit0;

	/* PPS 42 */
	pps_sdp->pps_payload.rc_quant_incr_limit1 = (u8)dsc_cfg->rc_quant_incr_limit1;

	/* PPS 43 */
	pps_sdp->pps_payload.rc_tgt_offset_lo = (u8)dsc_cfg->rc_tgt_offset_low;
	pps_sdp->pps_payload.rc_tgt_offset_hi = (u8)dsc_cfg->rc_tgt_offset_high;

	/* PPS 44 - 57 */
	for (i = 0; i < DSC_NUM_BUF_RANGES - 1; i++)
		pps_sdp->pps_payload.rc_buf_thresh[i] = dsc_cfg->rc_buf_thresh[i];

	/* PPS 58 - 87 */
	/*
	 * For DSC sink programming the RC Range parameter fields
	 * are as follows: Min_qp[15:11], max_qp[10:6], offset[5:0]
	 */
	for (i = 0; i < DSC_NUM_BUF_RANGES; i++) {
		pps_sdp->pps_payload.rc_range_parameters[i] =
			(u16)((dsc_cfg->rc_range_params[i].range_min_qp <<
			       DSC_PPS_RC_RANGE_MINQP_SHIFT) |
			      (dsc_cfg->rc_range_params[i].range_max_qp <<
			       DSC_PPS_RC_RANGE_MAXQP_SHIFT) |
			      (dsc_cfg->rc_range_params[i].range_bpg_offset));
		pps_sdp->pps_payload.rc_range_parameters[i] = DSC_PPS_SWAP_BYTES(pps_sdp->pps_payload.rc_range_parameters[i],
										 is_big_endian);
	}

	/* PPS 88 */
	pps_sdp->pps_payload.native_422 = (u8)dsc_cfg->native_422;
	pps_sdp->pps_payload.native_420 = (u8)dsc_cfg->native_420;

	/* PPS 89 */
	pps_sdp->pps_payload.second_line_bpg_offset = (u8)dsc_cfg->second_line_bpg_offset;

	/* PPS 90, 91 */
	pps_sdp->pps_payload.nsl_bpg_offset = DSC_PPS_SWAP_BYTES(dsc_cfg->nsl_bpg_offset,
								 is_big_endian);

	/* PPS 92, 93 */
	pps_sdp->pps_payload.second_line_offset_adj = DSC_PPS_SWAP_BYTES(dsc_cfg->second_line_offset_adj,
									 is_big_endian);

	/* PPS 94 - 127 are O */
}
EXPORT_SYMBOL(drm_dsc_pps_infoframe_pack);
