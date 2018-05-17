/*
 * Copyright (C) 2018 Intel Corp.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 * Manasi Navare <manasi.d.navare@intel.com>
 */

#ifndef DRM_DSC_H_
#define DRM_DSC_H_

#include <drm/drm_dp_helper.h>

/* VESA Display Stream Compression DSC 1.2 constants */
#define DSC_NUM_BUF_RANGES	15

/**
 * struct picture_parameter_set - Represents 128 bytes of Picture Parameter Set
 *
 * The VESA DSC standard defines picture parameter set (PPS) which display
 * stream compression encoders must communicate to decoders.
 * The PPS is encapsulated in 128 bytes (PPS 0 through PPS 127). The fields in
 * this structure are as per Table 4.1 in Vesa DSC specification v1.1/v1.2.
 * The PPS fields that span over more than a byte should be stored in Big Endian
 * format.
 */
struct picture_parameter_set {
	/**
	 * @dsc_version_minor:
	 * PPS0[3:0] - Contains Minor version of DSC
	 */
	u8 dsc_version_minor:4;
	/**
	 * @dsc_version_major:
	 * PPS0[7:4] - Contains major version of DSC
	 */
	u8 dsc_version_major:4;
	/**
	 * @pps_identifier:
	 * PPS1[7:0] - Application specific identifier that can be
	 * used to differentiate between different PPS tables.
	 */
	u8 pps_identifier;
	/**
	 * @pps2_reserved:
	 * PPS2[7:0]- RESERVED Byte
	 */
	u8 pps2_reserved;
	/**
	 * @linebuf_depth:
	 * PPS3[3:0] - Contains linebuffer bit depth used to generate
	 * the bitstream. (0x0 - 16 bits for DSc 1.2, 0x8 - 8 bits,
	 * 0xA - 10 bits, 0xB - 11 bits, 0xC - 12 bits, 0xD - 13 bits,
	 * 0xE - 14 bits for DSC1.2, 0xF - 14 bits for DSC 1.2.
	 */
	u8 linebuf_depth:4;
	/**
	 * @bits_per_component:
	 * PPS3[7:4] - Bits per component for the original pixels
	 * of the encoded picture.
	 * 0x0 = 16bpc (allowed only when dsc_version_minor = 0x2)
	 * 0x8 = 8bpc, 0xA = 10bpc, 0xC = 12bpc, 0xE = 14bpc (also
	 * allowed only when dsc_minor_version = 0x2)
	 */
	u8 bits_per_component:4;
	/**
	 * @bpp_high:
	 * PPS4[1:0] - These are the most significant 2 bits of
	 * compressed BPP bits_per_pixel[9:0] syntax element.
	 */
	u8 bpp_high:2;
	/**
	 * @vbr_enable:
	 * PPS4[2] - 0 = VBR disabled, 1 = VBR enabled
	 */
	u8 vbr_enable:1;
	/**
	 * @simple_422:
	 * PPS4[3] - Indicates if decoder drops samples to
	 * reconstruct the 4:2:2 picture.
	 */
	u8 simple_422:1;
	/**
	 * @convert_rgb:
	 * PPS4[4] - Indicates if DSC color space conversion is active
	 */
	u8 convert_rgb:1;
	/**
	 * @block_pred_enable:
	 * PPS4[5] - Indicates if BP is used to code any groups in picture
	 */
	u8 block_pred_enable:1;
	/**
	 * @pps4_reserved:
	 * PPS4[7:6] - Reseved bits
	 */
	u8 pps4_reserved:2;
	/**
	 * @bpp_low:
	 * PPS5[7:0] - This indicates the lower significant 8 bits of
	 * the compressed BPP bits_per_pixel[9:0] element.
	 */
	u8 bpp_low;
	/**
	 * @pic_height:
	 * PPS6[7:0], PPS7[7:0] -  Specifies the number of pixel rows within
	 * the raster.
	 */
	u16 pic_height;
	/**
	 * @pic_width:
	 * PPS8[7:0], PPS9[7:0] - Number of pixel columns within the raster.
	 */
	u16 pic_width;
	/**
	 * @slice_height:
	 * PPS10[7:0], PPS11[7:0] - Slice height in units of pixels.
	 */
	u16 slice_height;
	/**
	 * @slice_width:
	 * PPS12[7:0], PPS13[7:0] - Slice width in terms of pixels.
	 */
	u16 slice_width;
	/**
	 * @chunk_size:
	 * PPS14[7:0], PPS15[7:0] - Size in units of bytes of the chunks
	 * that are used for slice multiplexing.
	 */
	u16 chunk_size;
	/**
	 * @initial_xmit_delay_high:
	 * PPS16[1:0] - Most Significant two bits of initial transmission delay.
	 * It specifies the number of pixel times that the encoder waits before
	 * transmitting data from its rate buffer.
	 */
	u8 initial_xmit_delay_high:2;
	/**
	 * @pps16_reserved:
	 * PPS16[7:2] - Reserved
	 */
	u8 pps16_reserved:6;
	/**
	 * @initial_xmit_delay_low:
	 * PPS17[7:0] - Least significant 8 bits of initial transmission delay.
	 */
	u8 initial_xmit_delay_low;
	/**
	 * @initial_dec_delay:
	 *
	 * PPS18[7:0], PPS19[7:0] - Initial decoding delay which is the number
	 * of pixel times that the decoder accumulates data in its rate buffer
	 * before starting to decode and output pixels.
	 */
	u16 initial_dec_delay;
	/**
	 * @pps20_reserved:
	 *
	 * PPS20[7:0] - Reserved
	 */
	u8 pps20_reserved;
	/**
	 * @initial_scale_value:
	 * PPS21[5:0] - Initial rcXformScale factor used at beginning
	 * of a slice.
	 */
	u8 initial_scale_value:6;
	/**
	 * @pps21_reserved:
	 * PPS21[7:6] - Reserved
	 */
	u8 pps21_reserved:2;
	/**
	 * @scale_increment_interval:
	 * PPS22[7:0], PPS23[7:0] - Number of group times between incrementing
	 * the rcXformScale factor at end of a slice.
	 */
	u16 scale_increment_interval;
	/**
	 * @scale_decrement_interval_high:
	 * PPS24[3:0] - Higher 4 bits indicating number of group times between
	 * decrementing the rcXformScale factor at beginning of a slice.
	 */
	u8 scale_decrement_interval_high:4;
	/**
	 * @pps24_reserved:
	 * PPS24[7:4] - Reserved
	 */
	u8 pps24_reserved:4;
	/**
	 * @scale_decrement_interval_low:
	 * PPS25[7:0] - Lower 8 bits of scale decrement interval
	 */
	u8 scale_decrement_interval_low;
	/**
	 * @pps26_reserved:
	 * PPS26[7:0]
	 */
	u8 pps26_reserved;
	/**
	 * @first_line_bpg_offset:
	 * PPS27[4:0] - Number of additional bits that are allocated
	 * for each group on first line of a slice.
	 */
	u8 first_line_bpg_offset:5;
	/**
	 * @pps27_reserved:
	 * PPS27[7:5]
	 */
	u8 pps27_reserved:3;
	/**
	 * @nfl_bpg_offset:
	 * PPS28[7:0], PPS29[7:0] - Number of bits including frac bits
	 * deallocated for each group for groups after the first line of slice.
	 */
	u16 nfl_bpg_offset;
	/**
	 * @slice_bpg_offset:
	 * PPS30, PPS31[7:0] - Number of bits that are deallocated for each
	 * group to enforce the slice constraint.
	 */
	u16 slice_bpg_offset;
	/**
	 * @initial_offset:
	 * PPS32,33[7:0] - Initial value for rcXformOffset
	 */
	u16 initial_offset;
	/**
	 * @final_offset:
	 * PPS34,35[7:0] - Maximum end-of-slice value for rcXformOffset
	 */
	u16 final_offset;
	/**
	 * @flatness_min_qp:
	 * PPS36[4:0] - Minimum QP at which flatness is signaled and
	 * flatness QP adjustment is made.
	 */
	u8 flatness_min_qp:5;
	/**
	 * @pps36_reserved:
	 * PPS36[7:5] - Reserved
	 */
	u8 pps36_reserved:3;
	/**
	 * @flatness_max_qp:
	 * PPS37[4:0] - Max QP at which flatness is signalled and
	 * the flatness adjustment is made.
	 */
	u8 flatness_max_qp:5;
	/**
	 * @pps37_reserved:
	 * PPS37[7:5]
	 */
	u8 pps37_reserved:3;
	/**
	 * @rc_model_size:
	 * PPS38,39[7:0] - Number of bits within RC Model.
	 */
	u16 rc_model_size;
	/**
	 * @rc_edge_factor:
	 * PPS40[3:0] - Ratio of current activity vs, previous
	 * activity to determine presence of edge.
	 */
	u8 rc_edge_factor:4;
	/**
	 * @pps40_reserved:
	 * PPS40[7:4]
	 */
	u8 pps40_reserved:4;
	/**
	 * @rc_quant_incr_limit0:
	 * PPS41[4:0] - QP threshold used in short term RC
	 */
	u8 rc_quant_incr_limit0:5;
	/**
	 * @pps41_reserved:
	 * PPS41[7:5]
	 */
	u8 pps41_reserved:3;
	/**
	 * @rc_quant_incr_limit1:
	 * PPS42[4:0] - QP threshold used in short term RC
	 */
	u8 rc_quant_incr_limit1:5;
	/**
	 * @pps42_reserved:
	 * PPS42[7:5]
	 */
	u8 pps42_reserved:3;
	/**
	 * @rc_tgt_offset_lo:
	 * Lower end of the variability range around the target
	 * bits per group that is allowed by short term RC.
	 */
	u8 rc_tgt_offset_lo:4;
	/**
	 * @rc_tgt_offset_hi:
	 * Upper end of the variability range around the target
	 * bits per group that i allowed by short term rc.
	 */
	u8 rc_tgt_offset_hi:4;
	/**
	 * @rc_buf_thresh:
	 * PPS44[7:0] - PPS57[7:0] - Specifies the thresholds in RC model for
	 * the 15 ranges defined by 14 thresholds.
	 */
	u8 rc_buf_thresh[DSC_NUM_BUF_RANGES - 1];
	/**
	 * @rc_range_parameters:
	 * PPS58[7:0] - PPS87[7:0]
	 * Parameters that correspond to each of the 15 ranges.
	 */
	u16 rc_range_parameters[DSC_NUM_BUF_RANGES];
	/**
	 * @native_422:
	 * PPS88[0] - 0 = Native 4:2:2 not used
	 * 1 = Native 4:2:2 used
	 */
	u8 native_422:1;
	/**
	 * @native_420:
	 * PPS88[1] - 0 = Native 4:2:0 not used
	 * 1 = Native 4:2:0 not used.
	 */
	u8 native_420:1;
	/**
	 * @pps88_reserved:
	 * PPS[7:2] - Reserved 6 bits
	 */
	u8 pps88_reserved:6;
	/**
	 * @second_line_bpg_offset:
	 * PPS89[4:0] - Additional bits/group budget for the
	 * second line of a slice in Native 4:2:0 mode.
	 * Set to 0 if DSC minor version is 1 or native420 is 0.
	 */
	u8 second_line_bpg_offset:5;
	/**
	 * @pps89_reserved:
	 * PPS89[7:5] - Reserved
	 */
	u8 pps89_reserved:3;
	/**
	 * @nsl_bpg_offset:
	 * PPS90[7:0], PPS91[7:0] - Number of bits that are deallocated
	 * for each group that is not in the second line of a slice.
	 */
	u16 nsl_bpg_offset;
	/**
	 * @second_line_offset_adj:
	 * PPS92[7:0], PPS93[7:0] - Used as offset adjustment for the second
	 * line in Native 4:2:0 mode.
	 */
	u16 second_line_offset_adj;
	/**
	 * @pps_long_94_reserved:
	 * PPS 94, 95, 96, 97 - Reserved
	 */
	u32 pps_long_94_reserved;
	/**
	 * @pps_long_98_reserved:
	 * PPS 98, 99, 100, 101 - Reserved
	 */
	u32 pps_long_98_reserved;
	/**
	 * @pps_long_102_reserved:
	 * PPS 102, 103, 104, 105 - Reserved
	 */
	u32 pps_long_102_reserved;
	/**
	 * @pps_long_106_reserved:
	 * PPS 106, 107, 108, 109 - reserved
	 */
	u32 pps_long_106_reserved;
	/**
	 * @pps_long_110_reserved:
	 * PPS 110, 111, 112, 113 - reserved
	 */
	u32 pps_long_110_reserved;
	/**
	 * @pps_long_114_reserved:
	 * PPS 114 - 117 - reserved
	 */
	u32 pps_long_114_reserved;
	/**
	 * @pps_long_118_reserved:
	 * PPS 118 - 121 - reserved
	 */
	u32 pps_long_118_reserved;
	/**
	 * @pps_long_122_reserved:
	 * PPS 122- 125 - reserved
	 */
	u32 pps_long_122_reserved;
	/**
	 * @pps_short_126_reserved:
	 * PPS 126, 127 - reserved
	 */
	u16 pps_short_126_reserved;
};

/**
 * struct drm_dsc_pps_infoframe - DSC infoframe carrying the Picture Parameter
 * Set Metadata
 *
 * This structure represents the DSC PPS infoframe required to send the Picture
 * Parameter Set metadata required before enabling VESA Display Stream
 * Compression. This is based on the DP Secondary Data Packet structure and
 * comprises of SDP Header as defined in drm_dp_helper.h and PPS payload.
 *
 * @pps_header:
 *
 * Header for PPS as per DP SDP header format
 *
 * @pps_payload:
 *
 * PPS payload fields as per DSC specification Table 4-1
 */
struct drm_dsc_pps_infoframe {
	struct dp_sdp_header pps_header;
	struct picture_parameter_set pps_payload;
} __packed;

#endif /* _DRM_DSC_H_ */
