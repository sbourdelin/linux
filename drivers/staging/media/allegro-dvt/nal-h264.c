// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2019 Pengutronix, Michael Tretter <kernel@pengutronix.de>
 *
 * Convert NAL units between raw byte sequence payloads (RBSP) and C structs
 *
 * The conversion is defined in "ITU-T Rec. H.264 (04/2017) Advanced video
 * coding for generic audiovisual services". Decoder drivers may use the
 * parser to parse RBSP from encoded streams and configure the hardware, if
 * the hardware is not able to parse RBSP itself.  Encoder drivers may use the
 * generator to generate the RBSP for SPS/PPS nal units and add them to the
 * encoded stream if the hardware does not generate the units.
 */

#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/string.h>
#include <linux/v4l2-controls.h>

#include <linux/device.h>
#include <linux/export.h>
#include <linux/log2.h>

#include <nal-h264.h>

struct rbsp {
	char *buf;
	int size;
	int pos;
	int num_consecutive_zeros;
};

int nal_h264_profile_from_v4l2(enum v4l2_mpeg_video_h264_profile profile)
{
	switch (profile) {
	case V4L2_MPEG_VIDEO_H264_PROFILE_BASELINE:
		return 66;
	case V4L2_MPEG_VIDEO_H264_PROFILE_MAIN:
		return 77;
	case V4L2_MPEG_VIDEO_H264_PROFILE_EXTENDED:
		return 88;
	case V4L2_MPEG_VIDEO_H264_PROFILE_HIGH:
		return 100;
	default:
		return -EINVAL;
	}
}

int nal_h264_level_from_v4l2(enum v4l2_mpeg_video_h264_level level)
{
	switch (level) {
	case V4L2_MPEG_VIDEO_H264_LEVEL_1_0:
		return 10;
	case V4L2_MPEG_VIDEO_H264_LEVEL_1B:
		return 9;
	case V4L2_MPEG_VIDEO_H264_LEVEL_1_1:
		return 11;
	case V4L2_MPEG_VIDEO_H264_LEVEL_1_2:
		return 12;
	case V4L2_MPEG_VIDEO_H264_LEVEL_1_3:
		return 13;
	case V4L2_MPEG_VIDEO_H264_LEVEL_2_0:
		return 20;
	case V4L2_MPEG_VIDEO_H264_LEVEL_2_1:
		return 21;
	case V4L2_MPEG_VIDEO_H264_LEVEL_2_2:
		return 22;
	case V4L2_MPEG_VIDEO_H264_LEVEL_3_0:
		return 30;
	case V4L2_MPEG_VIDEO_H264_LEVEL_3_1:
		return 31;
	case V4L2_MPEG_VIDEO_H264_LEVEL_3_2:
		return 32;
	case V4L2_MPEG_VIDEO_H264_LEVEL_4_0:
		return 40;
	case V4L2_MPEG_VIDEO_H264_LEVEL_4_1:
		return 41;
	case V4L2_MPEG_VIDEO_H264_LEVEL_4_2:
		return 42;
	case V4L2_MPEG_VIDEO_H264_LEVEL_5_0:
		return 50;
	case V4L2_MPEG_VIDEO_H264_LEVEL_5_1:
		return 51;
	default:
		return -EINVAL;
	}
}

static int rbsp_read_bits(struct rbsp *rbsp, int num, int *val);
static int rbsp_write_bits(struct rbsp *rbsp, int num, int val);

static int add_emulation_prevention_three_byte(struct rbsp *rbsp)
{
	rbsp->num_consecutive_zeros = 0;
	/*
	 * We are not actually the emulation_prevention_three_byte, but the 2
	 * one bits of the byte and the 6 zero bits of the next byte.
	 * Therefore, the discarded byte shifted by 6 bits.
	 */
	rbsp_write_bits(rbsp, 8, (0x3 << 6));

	return 0;
}

static int discard_emulation_prevention_three_byte(struct rbsp *rbsp)
{
	unsigned int tmp = 0;

	rbsp->num_consecutive_zeros = 0;
	/*
	 * We are not actually discarding the emulation_prevention_three_byte,
	 * but the 2 one bits of the byte and the 6 zero bits of the next
	 * byte. Therefore, the discarded byte shifted by 6 bits.
	 */
	rbsp_read_bits(rbsp, 8, &tmp);
	if (tmp != (0x3 << 6))
		return -EINVAL;

	return 0;
}

static inline int rbsp_read_bit(struct rbsp *rbsp)
{
	int shift;
	int ofs;
	int bit;
	int err;

	if (rbsp->num_consecutive_zeros == 22) {
		err = discard_emulation_prevention_three_byte(rbsp);
		if (err)
			return err;
	}

	shift = 7 - (rbsp->pos % 8);
	ofs = rbsp->pos++ / 8;

	if (ofs >= rbsp->size)
		return -EINVAL;

	bit = (rbsp->buf[ofs] >> shift) & 1;

	/*
	 * Counting zeros for the emulation_prevention_three_byte only starts
	 * at byte boundaries.
	 */
	if (bit == 1 ||
	    (rbsp->num_consecutive_zeros < 7 && (rbsp->pos % 8 == 0)))
		rbsp->num_consecutive_zeros = 0;
	else
		rbsp->num_consecutive_zeros++;

	return bit;
}

static inline int rbsp_write_bit(struct rbsp *rbsp, int bit)
{
	int shift;
	int ofs;

	if (rbsp->num_consecutive_zeros == 22)
		add_emulation_prevention_three_byte(rbsp);

	shift = 7 - (rbsp->pos % 8);
	ofs = rbsp->pos++ / 8;

	if (ofs >= rbsp->size)
		return -EINVAL;

	rbsp->buf[ofs] &= ~(1 << shift);
	rbsp->buf[ofs] |= bit << shift;

	/*
	 * Counting zeros for the emulation_prevention_three_byte only starts
	 * at byte boundaries.
	 */
	if (bit == 1 ||
	    (rbsp->num_consecutive_zeros < 7 && (rbsp->pos % 8 == 0))) {
		rbsp->num_consecutive_zeros = 0;
	} else {
		rbsp->num_consecutive_zeros++;
	}

	return 0;
}

static inline int rbsp_read_bits(struct rbsp *rbsp, int num, int *val)
{
	int i, ret;
	int tmp = 0;

	if (num > 32)
		return -EINVAL;

	for (i = 0; i < num; i++) {
		ret = rbsp_read_bit(rbsp);
		if (ret < 0)
			return ret;
		tmp |= ret << (num - i - 1);
	}

	if (val)
		*val = tmp;

	return 0;
}

static int rbsp_write_bits(struct rbsp *rbsp, int num, int value)
{
	int ret;

	while (num--) {
		ret = rbsp_write_bit(rbsp, (value >> num) & 1);
		if (ret)
			return ret;
	}

	return 0;
}

static int rbsp_read_uev(struct rbsp *rbsp, unsigned int *val)
{
	int leading_zero_bits = 0;
	unsigned int tmp = 0;
	int ret;

	while ((ret = rbsp_read_bit(rbsp)) == 0)
		leading_zero_bits++;
	if (ret < 0)
		return ret;

	if (leading_zero_bits > 0) {
		ret = rbsp_read_bits(rbsp, leading_zero_bits, &tmp);
		if (ret)
			return ret;
	}

	if (val)
		*val = (1 << leading_zero_bits) - 1 + tmp;

	return 0;
}

static int rbsp_write_uev(struct rbsp *rbsp, unsigned int value)
{
	int i;
	int ret;
	int tmp = value + 1;
	int leading_zero_bits = fls(tmp) - 1;

	for (i = 0; i < leading_zero_bits; i++) {
		ret = rbsp_write_bit(rbsp, 0);
		if (ret)
			return ret;
	}

	return rbsp_write_bits(rbsp, leading_zero_bits + 1, tmp);
}

static int rbsp_read_sev(struct rbsp *rbsp, int *val)
{
	unsigned int tmp;
	int ret;

	ret = rbsp_read_uev(rbsp, &tmp);
	if (ret)
		return ret;

	if (val) {
		if (tmp & 1)
			*val = (tmp + 1) / 2;
		else
			*val = -(tmp / 2);
	}

	return 0;
}

static int rbsp_write_sev(struct rbsp *rbsp, int val)
{
	unsigned int tmp;

	if (val > 0)
		tmp = (2 * val) | 1;
	else
		tmp = -2 * val;

	return rbsp_write_uev(rbsp, tmp);
}

#define READ_BIT(field)						\
	do {							\
		int ret = rbsp_read_bit(rbsp);			\
		if (ret < 0)					\
			return ret;				\
		s->field = ret;					\
	} while (0)

#define READ_BITS(num, field)					\
	do {							\
		int val;					\
		int ret = rbsp_read_bits(rbsp, (num), &val);	\
		if (ret)					\
			return ret;				\
		s->field = val;					\
	} while (0)

#define READ_UEV(field)						\
	do {							\
		int ret = rbsp_read_uev(rbsp, &s->field);	\
		if (ret)					\
			return ret;				\
	} while (0)

#define READ_SEV(field)						\
	do {							\
		int ret = rbsp_read_sev(rbsp, &s->field);	\
		if (ret)					\
			return ret;				\
	} while (0)

#define WRITE_BIT(field)						\
	do {								\
		int ret = rbsp_write_bit(rbsp, s->field);		\
		if (ret < 0)						\
			return ret;					\
	} while (0)

#define WRITE_BITS(num, field)						\
	do {								\
		int ret = rbsp_write_bits(rbsp, (num), s->field);	\
		if (ret)						\
			return ret;					\
	} while (0)

#define WRITE_UEV(field)						\
	do {								\
		int ret = rbsp_write_uev(rbsp, s->field);		\
		if (ret)						\
			return ret;					\
	} while (0)

#define WRITE_SEV(field)						\
	do {								\
		int ret = rbsp_write_sev(rbsp, s->field);		\
		if (ret)						\
			return ret;					\
	} while (0)

#define PRINT_BIT(field)						\
		dev_dbg(dev, "field: %u\n", s->field)			\

#define PRINT_BITS(num, field)						\
		dev_dbg(dev, "field: %u\n", s->field)			\

#define PRINT_UEV(field)						\
		dev_dbg(dev, "field: %u\n", s->field)			\

#define PRINT_SEV(field)						\
		dev_dbg(dev, "field: %d\n", s->field)			\

static int nal_h264_write_trailing_bits(const struct device *dev,
					struct rbsp *rbsp)
{
	rbsp_write_bit(rbsp, 1);
	while (rbsp->pos % 8)
		rbsp_write_bit(rbsp, 0);

	return 0;
}

static int nal_h264_write_hrd_parameters(const struct device *dev,
					 struct rbsp *rbsp,
					 struct nal_h264_hrd_parameters *hrd)
{
	struct nal_h264_hrd_parameters *s = hrd;
	int i;

	WRITE_UEV(cpb_cnt_minus1);
	WRITE_BITS(4, bit_rate_scale);
	WRITE_BITS(4, cpb_size_scale);

	for (i = 0; i <= hrd->cpb_cnt_minus1; i++) {
		WRITE_UEV(bit_rate_value_minus1[i]);
		WRITE_UEV(cpb_size_value_minus1[i]);
		WRITE_BIT(cbr_flag[i]);
	}

	WRITE_BITS(5, initial_cpb_removal_delay_length_minus1);
	WRITE_BITS(5, cpb_removal_delay_length_minus1);
	WRITE_BITS(5, dpb_output_delay_length_minus1);
	WRITE_BITS(5, time_offset_length);

	return 0;
}

static int nal_h264_read_hrd_parameters(const struct device *dev,
					struct rbsp *rbsp,
					struct nal_h264_hrd_parameters *hrd)
{
	struct nal_h264_hrd_parameters *s = hrd;
	unsigned int i;

	READ_UEV(cpb_cnt_minus1);
	READ_BITS(4, bit_rate_scale);
	READ_BITS(4, cpb_size_scale);

	for (i = 0; i <= hrd->cpb_cnt_minus1; i++) {
		READ_UEV(bit_rate_value_minus1[i]);
		READ_UEV(cpb_size_value_minus1[i]);
		READ_BIT(cbr_flag[i]);
	}

	READ_BITS(5, initial_cpb_removal_delay_length_minus1);
	READ_BITS(5, cpb_removal_delay_length_minus1);
	READ_BITS(5, dpb_output_delay_length_minus1);
	READ_BITS(5, time_offset_length);

	return 0;
}

static void nal_h264_print_hrd_parameters(const struct device *dev,
					  struct nal_h264_hrd_parameters *hrd)
{
	struct nal_h264_hrd_parameters *s = hrd;
	unsigned int i;

	if (!hrd)
		return;

	PRINT_UEV(cpb_cnt_minus1);
	PRINT_BITS(4, bit_rate_scale);
	PRINT_BITS(4, cpb_size_scale);

	for (i = 0; i <= s->cpb_cnt_minus1; i++) {
		PRINT_UEV(bit_rate_value_minus1[i]);
		PRINT_UEV(cpb_size_value_minus1[i]);
		PRINT_BIT(cbr_flag[i]);
	}

	PRINT_BITS(5, initial_cpb_removal_delay_length_minus1);
	PRINT_BITS(5, cpb_removal_delay_length_minus1);
	PRINT_BITS(5, dpb_output_delay_length_minus1);
	PRINT_BITS(5, time_offset_length);
}

static int nal_h264_read_vui_parameters(const struct device *dev,
					struct rbsp *rbsp,
					struct nal_h264_vui_parameters *vui)
{
	struct nal_h264_vui_parameters *s = vui;
	int err;

	READ_BIT(aspect_ratio_info_present_flag);
	if (vui->aspect_ratio_info_present_flag) {
		READ_BITS(8, aspect_ratio_idc);
		if (vui->aspect_ratio_idc == 255) {
			READ_BITS(16, sar_width);
			READ_BITS(16, sar_height);
		}
	}

	READ_BIT(overscan_info_present_flag);
	if (vui->overscan_info_present_flag)
		READ_BIT(overscan_appropriate_flag);

	READ_BIT(video_signal_type_present_flag);
	if (vui->video_signal_type_present_flag) {
		READ_BITS(3, video_format);
		READ_BIT(video_full_range_flag);
		READ_BIT(colour_description_present_flag);

		if (vui->colour_description_present_flag) {
			READ_BITS(8, colour_primaries);
			READ_BITS(8, transfer_characteristics);
			READ_BITS(8, matrix_coefficients);
		}
	}

	READ_BIT(chroma_loc_info_present_flag);
	if (vui->chroma_loc_info_present_flag) {
		READ_UEV(chroma_sample_loc_type_top_field);
		READ_UEV(chroma_sample_loc_type_bottom_field);
	}

	READ_BIT(timing_info_present_flag);
	if (vui->timing_info_present_flag) {
		READ_BITS(32, num_units_in_tick);
		READ_BITS(32, time_scale);
		READ_BIT(fixed_frame_rate_flag);
	}

	READ_BIT(nal_hrd_parameters_present_flag);
	if (vui->nal_hrd_parameters_present_flag) {
		err = nal_h264_read_hrd_parameters(dev, rbsp,
						   &vui->nal_hrd_parameters);
		if (err)
			return err;
	}

	READ_BIT(vcl_hrd_parameters_present_flag);
	if (vui->vcl_hrd_parameters_present_flag) {
		err = nal_h264_read_hrd_parameters(dev, rbsp,
						   &vui->vcl_hrd_parameters);
		if (err)
			return err;
	}

	if (vui->nal_hrd_parameters_present_flag ||
	    vui->vcl_hrd_parameters_present_flag)
		READ_BIT(low_delay_hrd_flag);

	READ_BIT(pic_struct_present_flag);

	READ_BIT(bitstream_restriction_flag);
	if (vui->bitstream_restriction_flag) {
		READ_BIT(motion_vectors_over_pic_boundaries_flag);
		READ_UEV(max_bytes_per_pic_denom);
		READ_UEV(max_bits_per_mb_denom);
		READ_UEV(log2_max_mv_length_horizontal);
		READ_UEV(log21_max_mv_length_vertical);
		READ_UEV(max_num_reorder_frames);
		READ_UEV(max_dec_frame_buffering);
	}

	return 0;
}

static ssize_t nal_h264_write_vui_parameters(const struct device *dev,
					     struct rbsp *rbsp,
					     struct nal_h264_vui_parameters *vui)
{
	struct nal_h264_vui_parameters *s = vui;
	int err;

	WRITE_BIT(aspect_ratio_info_present_flag);
	if (vui->aspect_ratio_info_present_flag) {
		WRITE_BITS(8, aspect_ratio_idc);
		if (vui->aspect_ratio_idc == 255) {
			WRITE_BITS(16, sar_width);
			WRITE_BITS(16, sar_height);
		}
	}

	WRITE_BIT(overscan_info_present_flag);
	if (vui->overscan_info_present_flag)
		WRITE_BIT(overscan_appropriate_flag);

	WRITE_BIT(video_signal_type_present_flag);
	if (vui->video_signal_type_present_flag) {
		WRITE_BITS(3, video_format);
		WRITE_BIT(video_full_range_flag);
		WRITE_BIT(colour_description_present_flag);

		if (vui->colour_description_present_flag) {
			WRITE_BITS(8, colour_primaries);
			WRITE_BITS(8, transfer_characteristics);
			WRITE_BITS(8, matrix_coefficients);
		}
	}

	WRITE_BIT(chroma_loc_info_present_flag);
	if (vui->chroma_loc_info_present_flag) {
		WRITE_UEV(chroma_sample_loc_type_top_field);
		WRITE_UEV(chroma_sample_loc_type_bottom_field);
	}

	WRITE_BIT(timing_info_present_flag);
	if (vui->timing_info_present_flag) {
		WRITE_BITS(32, num_units_in_tick);
		WRITE_BITS(32, time_scale);
		WRITE_BIT(fixed_frame_rate_flag);
	}

	WRITE_BIT(nal_hrd_parameters_present_flag);
	if (vui->nal_hrd_parameters_present_flag) {
		err = nal_h264_write_hrd_parameters(dev, rbsp,
						    &vui->nal_hrd_parameters);
		if (err)
			return err;
	}

	WRITE_BIT(vcl_hrd_parameters_present_flag);
	if (vui->vcl_hrd_parameters_present_flag) {
		err = nal_h264_write_hrd_parameters(dev, rbsp,
						    &vui->vcl_hrd_parameters);
		if (err)
			return err;
	}

	if (vui->nal_hrd_parameters_present_flag ||
	    vui->vcl_hrd_parameters_present_flag)
		WRITE_BIT(low_delay_hrd_flag);

	WRITE_BIT(pic_struct_present_flag);

	WRITE_BIT(bitstream_restriction_flag);
	if (vui->bitstream_restriction_flag) {
		WRITE_BIT(motion_vectors_over_pic_boundaries_flag);
		WRITE_UEV(max_bytes_per_pic_denom);
		WRITE_UEV(max_bits_per_mb_denom);
		WRITE_UEV(log2_max_mv_length_horizontal);
		WRITE_UEV(log21_max_mv_length_vertical);
		WRITE_UEV(max_num_reorder_frames);
		WRITE_UEV(max_dec_frame_buffering);
	}

	return 0;
}

static void nal_h264_print_vui_parameters(const struct device *dev,
					  struct nal_h264_vui_parameters *vui)
{
	struct nal_h264_vui_parameters *s = vui;

	if (!vui)
		return;

	PRINT_BIT(aspect_ratio_info_present_flag);
	if (vui->aspect_ratio_info_present_flag) {
		PRINT_BITS(8, aspect_ratio_idc);
		if (vui->aspect_ratio_idc == 255) {
			PRINT_BITS(16, sar_width);
			PRINT_BITS(16, sar_height);
		}
	}

	PRINT_BIT(overscan_info_present_flag);
	if (vui->overscan_info_present_flag)
		PRINT_BIT(overscan_appropriate_flag);

	PRINT_BIT(video_signal_type_present_flag);
	if (vui->video_signal_type_present_flag) {
		PRINT_BITS(3, video_format);
		PRINT_BIT(video_full_range_flag);
		PRINT_BIT(colour_description_present_flag);

		if (vui->colour_description_present_flag) {
			PRINT_BITS(8, colour_primaries);
			PRINT_BITS(8, transfer_characteristics);
			PRINT_BITS(8, matrix_coefficients);
		}
	}

	PRINT_BIT(chroma_loc_info_present_flag);
	if (vui->chroma_loc_info_present_flag) {
		PRINT_UEV(chroma_sample_loc_type_top_field);
		PRINT_UEV(chroma_sample_loc_type_bottom_field);
	}

	PRINT_BIT(timing_info_present_flag);
	if (vui->timing_info_present_flag) {
		PRINT_BITS(32, num_units_in_tick);
		PRINT_BITS(32, time_scale);
		PRINT_BIT(fixed_frame_rate_flag);
	}

	PRINT_BIT(nal_hrd_parameters_present_flag);
	if (vui->nal_hrd_parameters_present_flag)
		nal_h264_print_hrd_parameters(dev, &vui->nal_hrd_parameters);

	PRINT_BIT(vcl_hrd_parameters_present_flag);
	if (vui->vcl_hrd_parameters_present_flag)
		nal_h264_print_hrd_parameters(dev, &vui->vcl_hrd_parameters);

	if (vui->nal_hrd_parameters_present_flag ||
	    vui->vcl_hrd_parameters_present_flag)
		PRINT_BIT(low_delay_hrd_flag);

	PRINT_BIT(pic_struct_present_flag);

	PRINT_BIT(bitstream_restriction_flag);
	if (vui->bitstream_restriction_flag) {
		PRINT_BIT(motion_vectors_over_pic_boundaries_flag);
		PRINT_UEV(max_bytes_per_pic_denom);
		PRINT_UEV(max_bits_per_mb_denom);
		PRINT_UEV(log2_max_mv_length_horizontal);
		PRINT_UEV(log21_max_mv_length_vertical);
		PRINT_UEV(max_num_reorder_frames);
		PRINT_UEV(max_dec_frame_buffering);
	}
}

static int nal_h264_rbsp_write_sps(const struct device *dev,
				   struct rbsp *rbsp, struct nal_h264_sps *sps)
{
	struct nal_h264_sps *s = sps;
	unsigned int i;
	int err;

	if (rbsp->size < 3)
		return -EINVAL;

	WRITE_BITS(8, profile_idc);
	WRITE_BIT(constraint_set0_flag);
	WRITE_BIT(constraint_set1_flag);
	WRITE_BIT(constraint_set2_flag);
	WRITE_BIT(constraint_set3_flag);
	WRITE_BIT(constraint_set4_flag);
	WRITE_BIT(constraint_set5_flag);
	WRITE_BITS(2, reserved_zero_2bits);
	WRITE_BITS(8, level_idc);

	WRITE_UEV(seq_parameter_set_id);

	if (sps->profile_idc == 100 || sps->profile_idc == 110 ||
	    sps->profile_idc == 122 || sps->profile_idc == 244 ||
	    sps->profile_idc == 44 || sps->profile_idc == 83 ||
	    sps->profile_idc == 86 || sps->profile_idc == 118 ||
	    sps->profile_idc == 128 || sps->profile_idc == 138 ||
	    sps->profile_idc == 139 || sps->profile_idc == 134 ||
	    sps->profile_idc == 135) {
		WRITE_UEV(chroma_format_idc);

		if (sps->chroma_format_idc == 3)
			WRITE_BIT(separate_colour_plane_flag);

		WRITE_UEV(bit_depth_luma_minus8);
		WRITE_UEV(bit_depth_chroma_minus8);
		WRITE_BIT(qpprime_y_zero_transform_bypass_flag);
		WRITE_BIT(seq_scaling_matrix_present_flag);

		if (sps->seq_scaling_matrix_present_flag) {
			dev_err(dev,
				"%s: Handling scaling matrix not supported\n",
				__func__);
			return -EINVAL;
		}
	}

	WRITE_UEV(log2_max_frame_num_minus4);

	WRITE_UEV(pic_order_cnt_type);
	if (sps->pic_order_cnt_type == 0) {
		WRITE_UEV(log2_max_pic_order_cnt_lsb_minus4);
	} else if (sps->pic_order_cnt_type == 1) {
		WRITE_BIT(delta_pic_order_always_zero_flag);
		WRITE_SEV(offset_for_non_ref_pic);
		WRITE_SEV(offset_for_top_to_bottom_field);

		WRITE_UEV(num_ref_frames_in_pic_order_cnt_cycle);
		for (i = 0; i < sps->num_ref_frames_in_pic_order_cnt_cycle; i++)
			WRITE_SEV(offset_for_ref_frame[i]);
	} else {
		dev_err(dev,
			"%s: Invalid pic_order_cnt_type %u\n", __func__,
			sps->pic_order_cnt_type);
		return -EINVAL;
	}

	WRITE_UEV(max_num_ref_frames);
	WRITE_BIT(gaps_in_frame_num_value_allowed_flag);
	WRITE_UEV(pic_width_in_mbs_minus1);
	WRITE_UEV(pic_height_in_map_units_minus1);

	WRITE_BIT(frame_mbs_only_flag);
	if (!sps->frame_mbs_only_flag)
		WRITE_BIT(mb_adaptive_frame_field_flag);

	WRITE_BIT(direct_8x8_inference_flag);

	WRITE_BIT(frame_cropping_flag);
	if (sps->frame_cropping_flag) {
		WRITE_UEV(crop_left);
		WRITE_UEV(crop_right);
		WRITE_UEV(crop_top);
		WRITE_UEV(crop_bottom);
	}

	WRITE_BIT(vui_parameters_present_flag);
	if (sps->vui_parameters_present_flag) {
		err = nal_h264_write_vui_parameters(dev, rbsp, &sps->vui);
		if (err)
			return err;
	}

	return 0;
}

static int nal_h264_rbsp_read_sps(const struct device *dev,
				  struct rbsp *rbsp, struct nal_h264_sps *sps)
{
	struct nal_h264_sps *s = sps;
	unsigned int i;
	int err;

	if (rbsp->size < 3)
		return -EINVAL;

	READ_BITS(8, profile_idc);
	READ_BIT(constraint_set0_flag);
	READ_BIT(constraint_set1_flag);
	READ_BIT(constraint_set2_flag);
	READ_BIT(constraint_set3_flag);
	READ_BIT(constraint_set4_flag);
	READ_BIT(constraint_set5_flag);
	READ_BITS(2, reserved_zero_2bits);
	READ_BITS(8, level_idc);

	READ_UEV(seq_parameter_set_id);

	if (sps->profile_idc == 100 || sps->profile_idc == 110 ||
	    sps->profile_idc == 122 || sps->profile_idc == 244 ||
	    sps->profile_idc == 44 || sps->profile_idc == 83 ||
	    sps->profile_idc == 86 || sps->profile_idc == 118 ||
	    sps->profile_idc == 128 || sps->profile_idc == 138 ||
	    sps->profile_idc == 139 || sps->profile_idc == 134 ||
	    sps->profile_idc == 135) {
		READ_UEV(chroma_format_idc);

		if (sps->chroma_format_idc == 3)
			READ_BIT(separate_colour_plane_flag);

		READ_UEV(bit_depth_luma_minus8);
		READ_UEV(bit_depth_chroma_minus8);
		READ_BIT(qpprime_y_zero_transform_bypass_flag);
		READ_BIT(seq_scaling_matrix_present_flag);

		if (sps->seq_scaling_matrix_present_flag) {
			dev_err(dev,
				"%s: Handling scaling matrix not supported\n",
				__func__);
			return -EINVAL;
		}
	}

	READ_UEV(log2_max_frame_num_minus4);

	READ_UEV(pic_order_cnt_type);
	if (sps->pic_order_cnt_type == 0) {
		READ_UEV(log2_max_pic_order_cnt_lsb_minus4);
	} else if (sps->pic_order_cnt_type == 1) {
		READ_BIT(delta_pic_order_always_zero_flag);
		READ_SEV(offset_for_non_ref_pic);
		READ_SEV(offset_for_top_to_bottom_field);

		READ_UEV(num_ref_frames_in_pic_order_cnt_cycle);
		for (i = 0; i < sps->num_ref_frames_in_pic_order_cnt_cycle; i++)
			READ_SEV(offset_for_ref_frame[i]);
	} else {
		dev_err(dev,
			"%s: Invalid pic_order_cnt_type %u\n", __func__,
			sps->pic_order_cnt_type);
		return -EINVAL;
	}

	READ_UEV(max_num_ref_frames);
	READ_BIT(gaps_in_frame_num_value_allowed_flag);
	READ_UEV(pic_width_in_mbs_minus1);
	READ_UEV(pic_height_in_map_units_minus1);

	READ_BIT(frame_mbs_only_flag);
	if (!sps->frame_mbs_only_flag)
		READ_BIT(mb_adaptive_frame_field_flag);

	READ_BIT(direct_8x8_inference_flag);

	READ_BIT(frame_cropping_flag);
	if (sps->frame_cropping_flag) {
		READ_UEV(crop_left);
		READ_UEV(crop_right);
		READ_UEV(crop_top);
		READ_UEV(crop_bottom);
	}

	READ_BIT(vui_parameters_present_flag);
	if (sps->vui_parameters_present_flag) {
		err = nal_h264_read_vui_parameters(dev, rbsp, &sps->vui);
		if (err)
			return err;
	}

	return 0;
}

static int nal_h264_rbsp_write_pps(const struct device *dev,
				   struct rbsp *rbsp, struct nal_h264_pps *pps)
{
	struct nal_h264_pps *s = pps;
	int i;

	WRITE_UEV(pic_parameter_set_id);
	WRITE_UEV(seq_parameter_set_id);
	WRITE_BIT(entropy_coding_mode_flag);
	WRITE_BIT(bottom_field_pic_order_in_frame_present_flag);
	WRITE_UEV(num_slice_groups_minus1);
	if (pps->num_slice_groups_minus1 > 0) {
		WRITE_UEV(slice_group_map_type);
		if (pps->slice_group_map_type == 0) {
			for (i = 0; i < pps->num_slice_groups_minus1; i++)
				WRITE_UEV(run_length_minus1[i]);
		} else if (pps->slice_group_map_type == 2) {
			for (i = 0; i < pps->num_slice_groups_minus1; i++) {
				WRITE_UEV(top_left[i]);
				WRITE_UEV(bottom_right[i]);
			}
		} else if (pps->slice_group_map_type == 3 ||
			   pps->slice_group_map_type == 4 ||
			   pps->slice_group_map_type == 5) {
			WRITE_BIT(slice_group_change_direction_flag);
			WRITE_UEV(slice_group_change_rate_minus1);
		} else if (pps->slice_group_map_type == 6) {
			WRITE_UEV(pic_size_in_map_units_minus1);
			for (i = 0; i < pps->pic_size_in_map_units_minus1; i++)
				WRITE_BITS(order_base_2
					   (s->num_slice_groups_minus1 + 1),
					   slice_group_id[i]);
		}
	}
	WRITE_UEV(num_ref_idx_l0_default_active_minus1);
	WRITE_UEV(num_ref_idx_l1_default_active_minus1);
	WRITE_BIT(weighted_pred_flag);
	WRITE_BITS(2, weighted_bipred_idc);
	WRITE_SEV(pic_init_qp_minus26);
	WRITE_SEV(pic_init_qs_minus26);
	WRITE_SEV(chroma_qp_index_offset);
	WRITE_BIT(deblocking_filter_control_present_flag);
	WRITE_BIT(constrained_intra_pred_flag);
	WRITE_BIT(redundant_pic_cnt_present_flag);
	if (/* more_rbsp_data() */ false) {
		WRITE_BIT(transform_8x8_mode_flag);
		WRITE_BIT(pic_scaling_matrix_present_flag);
		if (pps->pic_scaling_matrix_present_flag) {
			dev_err(dev,
				"%s: Handling scaling matrix not supported\n",
				__func__);
			return -EINVAL;
		}
		WRITE_SEV(second_chroma_qp_index_offset);
	}

	return 0;
}

static int nal_h264_rbsp_read_pps(const struct device *dev,
				  struct rbsp *rbsp, struct nal_h264_pps *pps)
{
	struct nal_h264_pps *s = pps;
	unsigned int i;

	READ_UEV(pic_parameter_set_id);
	READ_UEV(seq_parameter_set_id);
	READ_BIT(entropy_coding_mode_flag);
	READ_BIT(bottom_field_pic_order_in_frame_present_flag);
	READ_UEV(num_slice_groups_minus1);
	if (s->num_slice_groups_minus1 > 0) {
		READ_UEV(slice_group_map_type);
		if (pps->slice_group_map_type == 0) {
			for (i = 0; i < pps->num_slice_groups_minus1; i++)
				READ_UEV(run_length_minus1[i]);
		} else if (pps->slice_group_map_type == 2) {
			for (i = 0; i < pps->num_slice_groups_minus1; i++) {
				READ_UEV(top_left[i]);
				READ_UEV(bottom_right[i]);
			}
		} else if (s->slice_group_map_type == 3 ||
			   s->slice_group_map_type == 4 ||
			   s->slice_group_map_type == 5) {
			READ_BIT(slice_group_change_direction_flag);
			READ_UEV(slice_group_change_rate_minus1);
		} else if (s->slice_group_map_type == 6) {
			READ_UEV(pic_size_in_map_units_minus1);
			for (i = 0; i < s->pic_size_in_map_units_minus1; i++)
				READ_BITS(order_base_2
					  (s->num_slice_groups_minus1 + 1),
					  slice_group_id[i]);
		}
	}
	READ_UEV(num_ref_idx_l0_default_active_minus1);
	READ_UEV(num_ref_idx_l1_default_active_minus1);
	READ_BIT(weighted_pred_flag);
	READ_BITS(2, weighted_bipred_idc);
	READ_SEV(pic_init_qp_minus26);
	READ_SEV(pic_init_qs_minus26);
	READ_SEV(chroma_qp_index_offset);
	READ_BIT(deblocking_filter_control_present_flag);
	READ_BIT(constrained_intra_pred_flag);
	READ_BIT(redundant_pic_cnt_present_flag);
	if (/* more_rbsp_data() */ false) {
		READ_BIT(transform_8x8_mode_flag);
		READ_BIT(pic_scaling_matrix_present_flag);
		if (pps->pic_scaling_matrix_present_flag) {
			dev_err(dev,
				"%s: Handling scaling matrix not supported\n",
				__func__);
			return -EINVAL;
		}
		READ_SEV(second_chroma_qp_index_offset);
	}

	return 0;
}

ssize_t nal_h264_write_sps(const struct device *dev,
			   void *dest, size_t n, struct nal_h264_sps *sps)
{
	struct rbsp rbsp;
	int err;
	u8 *p = dest;

	rbsp.buf = p + 5;
	rbsp.size = n - 5;
	rbsp.pos = 0;

	err = nal_h264_rbsp_write_sps(dev, &rbsp, sps);
	if (err)
		return err;

	err = nal_h264_write_trailing_bits(dev, &rbsp);
	if (err)
		return err;

	p[0] = 0x00;
	p[1] = 0x00;
	p[2] = 0x00;
	p[3] = 0x01;
	p[4] = 0x07;

	return ((rbsp.pos + 7) / 8) + 5;
}
EXPORT_SYMBOL_GPL(nal_h264_write_sps);

ssize_t nal_h264_read_sps(const struct device *dev,
			  struct nal_h264_sps *sps, void *src, size_t n)
{
	struct rbsp rbsp;
	int err;

	rbsp.buf = src;
	rbsp.size = n;
	rbsp.pos = 0;

	rbsp.buf += 5;
	rbsp.size -= 5;

	err = nal_h264_rbsp_read_sps(dev, &rbsp, sps);
	if (err)
		return err;

	return ((rbsp.pos + 7) / 8) + 5;
}
EXPORT_SYMBOL_GPL(nal_h264_read_sps);

void nal_h264_print_sps(const struct device *dev, struct nal_h264_sps *sps)
{
	struct nal_h264_sps *s = sps;
	unsigned int i;

	if (!sps)
		return;

	PRINT_BITS(8, profile_idc);
	PRINT_BIT(constraint_set0_flag);
	PRINT_BIT(constraint_set1_flag);
	PRINT_BIT(constraint_set2_flag);
	PRINT_BIT(constraint_set3_flag);
	PRINT_BIT(constraint_set4_flag);
	PRINT_BIT(constraint_set5_flag);
	PRINT_BITS(2, reserved_zero_2bits);
	PRINT_BITS(8, level_idc);

	PRINT_UEV(seq_parameter_set_id);

	if (sps->profile_idc == 100 || sps->profile_idc == 110 ||
	    sps->profile_idc == 122 || sps->profile_idc == 244 ||
	    sps->profile_idc == 44 || sps->profile_idc == 83 ||
	    sps->profile_idc == 86 || sps->profile_idc == 118 ||
	    sps->profile_idc == 128 || sps->profile_idc == 138 ||
	    sps->profile_idc == 139 || sps->profile_idc == 134 ||
	    sps->profile_idc == 135) {
		PRINT_UEV(chroma_format_idc);

		if (sps->chroma_format_idc == 3)
			PRINT_BIT(separate_colour_plane_flag);

		PRINT_UEV(bit_depth_luma_minus8);
		PRINT_UEV(bit_depth_chroma_minus8);
		PRINT_BIT(qpprime_y_zero_transform_bypass_flag);
		PRINT_BIT(seq_scaling_matrix_present_flag);

		if (sps->seq_scaling_matrix_present_flag)
			dev_err(dev,
				"%s: Handling scaling matrix not supported\n",
				__func__);
	}

	PRINT_UEV(log2_max_frame_num_minus4);

	PRINT_UEV(pic_order_cnt_type);
	if (sps->pic_order_cnt_type == 0) {
		PRINT_UEV(log2_max_pic_order_cnt_lsb_minus4);
	} else if (sps->pic_order_cnt_type == 1) {
		PRINT_BIT(delta_pic_order_always_zero_flag);
		PRINT_SEV(offset_for_non_ref_pic);
		PRINT_SEV(offset_for_top_to_bottom_field);

		PRINT_UEV(num_ref_frames_in_pic_order_cnt_cycle);
		for (i = 0; i < sps->num_ref_frames_in_pic_order_cnt_cycle; i++)
			PRINT_SEV(offset_for_ref_frame[i]);
	} else {
		dev_err(dev,
			"%s: Invalid pic_order_cnt_type %u\n", __func__,
			sps->pic_order_cnt_type);
	}

	PRINT_UEV(max_num_ref_frames);
	PRINT_BIT(gaps_in_frame_num_value_allowed_flag);
	PRINT_UEV(pic_width_in_mbs_minus1);
	PRINT_UEV(pic_height_in_map_units_minus1);

	PRINT_BIT(frame_mbs_only_flag);
	if (!sps->frame_mbs_only_flag)
		PRINT_BIT(mb_adaptive_frame_field_flag);

	PRINT_BIT(direct_8x8_inference_flag);

	PRINT_BIT(frame_cropping_flag);
	if (sps->frame_cropping_flag) {
		PRINT_UEV(crop_left);
		PRINT_UEV(crop_right);
		PRINT_UEV(crop_top);
		PRINT_UEV(crop_bottom);
	}

	PRINT_BIT(vui_parameters_present_flag);
	if (sps->vui_parameters_present_flag)
		nal_h264_print_vui_parameters(dev, &sps->vui);
}
EXPORT_SYMBOL_GPL(nal_h264_print_sps);

ssize_t nal_h264_write_pps(const struct device *dev,
			   void *dest, size_t n, struct nal_h264_pps *pps)
{
	struct rbsp rbsp;
	int err;
	u8 *p = dest;

	rbsp.buf = p + 5;
	rbsp.size = n - 5;
	rbsp.pos = 0;

	err = nal_h264_rbsp_write_pps(dev, &rbsp, pps);
	if (err)
		return err;

	err = nal_h264_write_trailing_bits(dev, &rbsp);
	if (err)
		return err;

	p[0] = 0x00;
	p[1] = 0x00;
	p[2] = 0x00;
	p[3] = 0x01;
	p[4] = 0x08;

	return ((rbsp.pos + 7) / 8) + 5;
}
EXPORT_SYMBOL_GPL(nal_h264_write_pps);

ssize_t nal_h264_read_pps(const struct device *dev,
			  struct nal_h264_pps *pps, void *src, size_t n)
{
	struct rbsp rbsp;
	int err;

	rbsp.buf = src;
	rbsp.size = n;
	rbsp.pos = 0;

	rbsp.buf += 5;
	rbsp.size -= 5;

	err = nal_h264_rbsp_read_pps(dev, &rbsp, pps);
	if (err)
		return err;

	return ((rbsp.pos + 7) / 8) + 5;
}
EXPORT_SYMBOL_GPL(nal_h264_read_pps);

void nal_h264_print_pps(const struct device *dev, struct nal_h264_pps *pps)
{
	struct nal_h264_pps *s = pps;
	unsigned int i;

	if (!pps)
		return;

	PRINT_UEV(pic_parameter_set_id);
	PRINT_UEV(seq_parameter_set_id);
	PRINT_BIT(entropy_coding_mode_flag);
	PRINT_BIT(bottom_field_pic_order_in_frame_present_flag);
	PRINT_UEV(num_slice_groups_minus1);
	if (s->num_slice_groups_minus1 > 0) {
		PRINT_UEV(slice_group_map_type);
		if (pps->slice_group_map_type == 0) {
			for (i = 0; i < pps->num_slice_groups_minus1; i++)
				PRINT_UEV(run_length_minus1[i]);
		} else if (pps->slice_group_map_type == 2) {
			for (i = 0; i < pps->num_slice_groups_minus1; i++) {
				PRINT_UEV(top_left[i]);
				PRINT_UEV(bottom_right[i]);
			}
		} else if (s->slice_group_map_type == 3 ||
			   s->slice_group_map_type == 4 ||
			   s->slice_group_map_type == 5) {
			PRINT_BIT(slice_group_change_direction_flag);
			PRINT_UEV(slice_group_change_rate_minus1);
		} else if (s->slice_group_map_type == 6) {
			PRINT_UEV(pic_size_in_map_units_minus1);
			for (i = 0; i < s->pic_size_in_map_units_minus1; i++)
				PRINT_BITS(order_base_2
					   (s->num_slice_groups_minus1 + 1),
					   slice_group_id[i]);
		}
	}
	PRINT_UEV(num_ref_idx_l0_default_active_minus1);
	PRINT_UEV(num_ref_idx_l1_default_active_minus1);
	PRINT_BIT(weighted_pred_flag);
	PRINT_BITS(2, weighted_bipred_idc);
	PRINT_SEV(pic_init_qp_minus26);
	PRINT_SEV(pic_init_qs_minus26);
	PRINT_SEV(chroma_qp_index_offset);
	PRINT_BIT(deblocking_filter_control_present_flag);
	PRINT_BIT(constrained_intra_pred_flag);
	PRINT_BIT(redundant_pic_cnt_present_flag);
	if (/* more_rbsp_data() */ false) {
		PRINT_BIT(transform_8x8_mode_flag);
		PRINT_BIT(pic_scaling_matrix_present_flag);
		if (pps->pic_scaling_matrix_present_flag) {
			dev_err(dev,
				"%s: Handling scaling matrix not supported\n",
				__func__);
		}
		PRINT_SEV(second_chroma_qp_index_offset);
	}
}
EXPORT_SYMBOL_GPL(nal_h264_print_pps);

ssize_t nal_h264_read_filler(const struct device *dev, void *src, size_t n)
{
	char *p = src;
	size_t i = 5;

	if (p[0] != 0x00 || p[1] != 0x00 || p[2] != 0x00 || p[3] != 0x01)
		return -EINVAL;

	if (p[4] != 0x0c)
		return -EINVAL;

	while (p[i] == 0xff && i < n)
		i++;

	if (p[i] != 0x80)
		return -EINVAL;

	return i;
}
EXPORT_SYMBOL_GPL(nal_h264_read_filler);

ssize_t nal_h264_write_filler(const struct device *dev, void *dest, size_t n)
{
	char *p = dest;

	if (n < 6)
		return -EINVAL;

	p[0] = 0x00;
	p[1] = 0x00;
	p[2] = 0x00;
	p[3] = 0x01;
	p[4] = 0x0c;
	memset(p + 5, 0xff, n - 6);
	p[n - 1] = 0x80;

	return n;
}
EXPORT_SYMBOL_GPL(nal_h264_write_filler);
