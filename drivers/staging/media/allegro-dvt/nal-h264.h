/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2019 Pengutronix, Michael Tretter <kernel@pengutronix.de>
 *
 * Convert NAL units between raw byte sequence payloads (RBSP) and C structs.
 */

#ifndef __NAL_H264_H__
#define __NAL_H264_H__

#include <linux/kernel.h>
#include <linux/types.h>

/* Rec. ITU-T H.264 (04/2017) E.1.2 */
struct nal_h264_hrd_parameters {
	unsigned int cpb_cnt_minus1;
	unsigned int bit_rate_scale:4;
	unsigned int cpb_size_scale:4;
	struct {
		int bit_rate_value_minus1[16];
		int cpb_size_value_minus1[16];
		unsigned int cbr_flag[16]; /* FIXME cbr_flag:1 */
	};
	unsigned int initial_cpb_removal_delay_length_minus1:5;
	unsigned int cpb_removal_delay_length_minus1:5;
	unsigned int dpb_output_delay_length_minus1:5;
	unsigned int time_offset_length:5;
};

/* Rec. ITU-T H.264 (04/2017) E.1.1 */
struct nal_h264_vui_parameters {
	unsigned int aspect_ratio_info_present_flag:1;
	struct {
		unsigned int aspect_ratio_idc:8;
		unsigned int sar_width:16;
		unsigned int sar_height:16;
	};
	unsigned int overscan_info_present_flag:1;
	unsigned int overscan_appropriate_flag:1;
	unsigned int video_signal_type_present_flag:1;
	struct {
		unsigned int video_format:3;
		unsigned int video_full_range_flag:1;
		unsigned int colour_description_present_flag:1;
		struct {
			unsigned int colour_primaries:8;
			unsigned int transfer_characteristics:8;
			unsigned int matrix_coefficients:8;
		};
	};
	unsigned int chroma_loc_info_present_flag:1;
	struct {
		unsigned int chroma_sample_loc_type_top_field;
		unsigned int chroma_sample_loc_type_bottom_field;
	};
	unsigned int timing_info_present_flag:1;
	struct {
	unsigned int num_units_in_tick:32;
	unsigned int time_scale:32;
	unsigned int fixed_frame_rate_flag:1;
	};
	unsigned int nal_hrd_parameters_present_flag:1;
	struct nal_h264_hrd_parameters nal_hrd_parameters;
	unsigned int vcl_hrd_parameters_present_flag:1;
	struct nal_h264_hrd_parameters vcl_hrd_parameters;
	unsigned int low_delay_hrd_flag:1;
	unsigned int pic_struct_present_flag:1;
	unsigned int bitstream_restriction_flag:1;
	struct {
		unsigned int motion_vectors_over_pic_boundaries_flag:1;
		unsigned int max_bytes_per_pic_denom;
		unsigned int max_bits_per_mb_denom;
		unsigned int log2_max_mv_length_horizontal;
		unsigned int log21_max_mv_length_vertical;
		unsigned int max_num_reorder_frames;
		unsigned int max_dec_frame_buffering;
	};
};

/* Rec. ITU-T H.264 (04/2017) 7.3.2.1.1 Sequence parameter set data syntax */
struct nal_h264_sps {
	unsigned int profile_idc:8;
	unsigned int constraint_set0_flag:1;
	unsigned int constraint_set1_flag:1;
	unsigned int constraint_set2_flag:1;
	unsigned int constraint_set3_flag:1;
	unsigned int constraint_set4_flag:1;
	unsigned int constraint_set5_flag:1;
	unsigned int reserved_zero_2bits:2;
	unsigned int level_idc:8;
	unsigned int seq_parameter_set_id;
	struct {
		unsigned int chroma_format_idc;
		unsigned int separate_colour_plane_flag:1;
		unsigned int bit_depth_luma_minus8;
		unsigned int bit_depth_chroma_minus8;
		unsigned int qpprime_y_zero_transform_bypass_flag:1;
		unsigned int seq_scaling_matrix_present_flag:1;
	};
	unsigned int log2_max_frame_num_minus4;
	unsigned int pic_order_cnt_type;
	union {
		unsigned int log2_max_pic_order_cnt_lsb_minus4;
		struct {
			unsigned int delta_pic_order_always_zero_flag:1;
			int offset_for_non_ref_pic;
			int offset_for_top_to_bottom_field;
			unsigned int num_ref_frames_in_pic_order_cnt_cycle;
			int offset_for_ref_frame[255];
		};
	};
	unsigned int max_num_ref_frames;
	unsigned int gaps_in_frame_num_value_allowed_flag:1;
	unsigned int pic_width_in_mbs_minus1;
	unsigned int pic_height_in_map_units_minus1;
	unsigned int frame_mbs_only_flag:1;
	unsigned int mb_adaptive_frame_field_flag:1;
	unsigned int direct_8x8_inference_flag:1;
	unsigned int frame_cropping_flag:1;
	struct {
		unsigned int crop_left;
		unsigned int crop_right;
		unsigned int crop_top;
		unsigned int crop_bottom;
	};
	unsigned int vui_parameters_present_flag:1;
	struct nal_h264_vui_parameters vui;
};

/* Rec. ITU-T H.264 (04/2017) 7.3.2.2 Picture parameter set RBSP syntax */
struct nal_h264_pps {
	unsigned int pic_parameter_set_id;
	unsigned int seq_parameter_set_id;
	unsigned int entropy_coding_mode_flag:1;
	unsigned int bottom_field_pic_order_in_frame_present_flag:1;
	unsigned int num_slice_groups_minus1;
	unsigned int slice_group_map_type;
	union {
		unsigned int run_length_minus1[8];
		struct {
			unsigned int top_left[8];
			unsigned int bottom_right[8];
		};
		struct {
			unsigned int slice_group_change_direction_flag:1;
			unsigned int slice_group_change_rate_minus1;
		};
		struct {
			unsigned int pic_size_in_map_units_minus1;
			unsigned int slice_group_id[8];
		};
	};
	unsigned int num_ref_idx_l0_default_active_minus1;
	unsigned int num_ref_idx_l1_default_active_minus1;
	unsigned int weighted_pred_flag:1;
	unsigned int weighted_bipred_idc:2;
	int pic_init_qp_minus26;
	int pic_init_qs_minus26;
	int chroma_qp_index_offset;
	unsigned int deblocking_filter_control_present_flag:1;
	unsigned int constrained_intra_pred_flag:1;
	unsigned int redundant_pic_cnt_present_flag:1;
	struct {
		unsigned int transform_8x8_mode_flag:1;
		unsigned int pic_scaling_matrix_present_flag:1;
		int second_chroma_qp_index_offset;
	};
};

int nal_h264_level_from_v4l2(enum v4l2_mpeg_video_h264_level level);
int nal_h264_profile_from_v4l2(enum v4l2_mpeg_video_h264_profile profile);

ssize_t nal_h264_write_sps(const struct device *dev,
			   void *dest, size_t n, struct nal_h264_sps *sps);
ssize_t nal_h264_read_sps(const struct device *dev,
			  struct nal_h264_sps *sps, void *src, size_t n);
void nal_h264_print_sps(const struct device *dev, struct nal_h264_sps *sps);

ssize_t nal_h264_write_pps(const struct device *dev,
			   void *dest, size_t n, struct nal_h264_pps *pps);
ssize_t nal_h264_read_pps(const struct device *dev,
			  struct nal_h264_pps *pps, void *src, size_t n);
void nal_h264_print_pps(const struct device *dev, struct nal_h264_pps *pps);

ssize_t nal_h264_write_filler(const struct device *dev, void *dest, size_t n);
ssize_t nal_h264_read_filler(const struct device *dev, void *src, size_t n);

#endif /* __NAL_H264_H__ */
