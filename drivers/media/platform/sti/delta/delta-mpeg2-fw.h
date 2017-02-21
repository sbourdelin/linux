/*
 * Copyright (C) STMicroelectronics SA 2015
 * Author: Hugues Fruchet <hugues.fruchet@st.com> for STMicroelectronics.
 * License terms:  GNU General Public License (GPL), version 2
 */

#ifndef DELTA_MPEG2_FW_H
#define DELTA_MPEG2_FW_H

#define MPEG2_Q_MATRIX_SIZE	64

#define MPEG2_DECODER_ID	0xCAFE
#define MPEG2_DECODER_BASE	(MPEG2_DECODER_ID << 16)

#define MPEG2_NUMBER_OF_CEH_INTERVALS 32

enum mpeg_decoding_flags_t {
	/* used to determine the type of picture */
	MPEG_DECODING_FLAGS_TOP_FIELD_FIRST = 0x00000001,
	/* used for parsing progression purpose only */
	MPEG_DECODING_FLAGS_FRAME_PRED_FRAME_DCT = 0x00000002,
	/* used for parsing progression purpose only */
	MPEG_DECODING_FLAGS_CONCEALMENT_MOTION_VECTORS = 0x00000004,
	/* used for the inverse Quantisation process */
	MPEG_DECODING_FLAGS_Q_SCALE_TYPE = 0x00000008,
	/* VLC tables selection when decoding the DCT coefficients */
	MPEG_DECODING_FLAGS_INTRA_VLC_FORMAT = 0x00000010,
	/* used for the inverse Scan process */
	MPEG_DECODING_FLAGS_ALTERNATE_SCAN = 0x00000020,
	/* used for progressive frame signaling */
	MPEG_DECODING_FLAGS_PROGRESSIVE_FRAME = 0x00000040
};

/* additional decoding flags */
enum mpeg2_additional_flags_t {
	MPEG2_ADDITIONAL_FLAG_NONE = 0x00000000,
	MPEG2_ADDITIONAL_FLAG_DEBLOCKING_ENABLE = 0x00000001,
	MPEG2_ADDITIONAL_FLAG_DERINGING_ENABLE = 0x00000002,
	MPEG2_ADDITIONAL_FLAG_TRANSCODING_H264 = 0x00000004,
	MPEG2_ADDITIONAL_FLAG_CEH = 0x00000008,
	MPEG2_ADDITIONAL_FLAG_FIRST_FIELD = 0x00000010,
	MPEG2_ADDITIONAL_FLAG_SECOND_FIELD = 0x00000020
};

/* horizontal decimation factor */
enum mpeg2_horizontal_deci_factor_t {
	MPEG2_HDEC_1 = 0x00000000, /* no H resize */
	MPEG2_HDEC_2 = 0x00000001, /* H/2  resize */
	MPEG2_HDEC_4 = 0x00000002, /* H/4  resize */
	/* advanced H/2 resize using improved 8-tap filters */
	MPEG2_HDEC_ADVANCED_2 = 0x00000101,
	/* advanced H/4 resize using improved 8-tap filters */
	MPEG2_HDEC_ADVANCED_4 = 0x00000102
};

/* vertical decimation factor */
enum mpeg2_vertical_deci_factor_t {
	MPEG2_VDEC_1 = 0x00000000, /* no V resize */
	MPEG2_VDEC_2_PROG = 0x00000004, /* V/2, progressive resize */
	MPEG2_VDEC_2_INT = 0x00000008, /* V/2, interlaced resize */
	/* advanced V/2, progressive resize */
	MPEG2_VDEC_ADVANCED_2_PROG = 0x00000204,
	/* advanced V/2, interlaced resize */
	MPEG2_VDEC_ADVANCED_2_INT = 0x00000208
};

/*
 * used to enable main/aux outputs for both display &
 * reference reconstruction blocks
 */
enum mpeg2_rcn_ref_disp_enable_t {
	/* enable decimated (for display) reconstruction */
	MPEG2_DISP_AUX_EN = 0x00000010,
	/* enable main (for display) reconstruction */
	MPEG2_DISP_MAIN_EN = 0x00000020,
	/* enable both main & decimated (for display) reconstruction */
	MPEG2_DISP_AUX_MAIN_EN = 0x00000030,
	/* enable only reference output (ex. for trick modes) */
	MPEG2_REF_MAIN_EN = 0x00000100,
	/*
	 * enable reference output with decimated
	 * (for display) reconstruction
	 */
	MPEG2_REF_MAIN_DISP_AUX_EN = 0x00000110,
	/* enable reference output with main (for display) reconstruction */
	MPEG2_REF_MAIN_DISP_MAIN_EN = 0x00000120,
	/*
	 * enable reference output with main & decimated
	 * (for display) reconstruction
	 */
	MPEG2_REF_MAIN_DISP_MAIN_AUX_EN = 0x00000130
};

/* picture prediction coding type (none, one or two reference pictures) */
enum mpeg2_picture_coding_type_t {
	/* forbidden coding type */
	MPEG2_FORBIDDEN_PICTURE = 0x00000000,
	/* intra (I) picture coding type */
	MPEG2_INTRA_PICTURE = 0x00000001,
	/* predictive (P) picture coding type */
	MPEG2_PREDICTIVE_PICTURE = 0x00000002,
	/* bidirectional (B) picture coding type */
	MPEG2_BIDIRECTIONAL_PICTURE = 0x00000003,
	/* dc intra (D) picture coding type */
	MPEG2_DC_INTRA_PICTURE = 0x00000004,
	/* reserved coding type*/
	MPEG2_RESERVED_1_PICTURE = 0x00000005,
	MPEG2_RESERVED_2_PICTURE = 0x00000006,
	MPEG2_RESERVED_3_PICTURE = 0x00000007
};

/* picture structure type (progressive, interlaced top/bottom) */
enum mpeg2_picture_structure_t {
	MPEG2_RESERVED_TYPE = 0,
	/* identifies a top field picture type */
	MPEG2_TOP_FIELD_TYPE = 1,
	/* identifies a bottom field picture type */
	MPEG2_BOTTOM_FIELD_TYPE = 2,
	/* identifies a frame picture type */
	MPEG2_FRAME_TYPE = 3
};

/* decoding mode */
enum mpeg2_decoding_mode_t {
	MPEG2_NORMAL_DECODE = 0,
	MPEG2_NORMAL_DECODE_WITHOUT_ERROR_RECOVERY = 1,
	MPEG2_DOWNGRADED_DECODE_LEVEL1 = 2,
	MPEG2_DOWNGRADED_DECODE_LEVEL2 = 4
};

/* quantisation matrix flags */
enum mpeg2_default_matrix_flags_t {
	MPEG2_LOAD_INTRA_QUANTISER_MATRIX_FLAG = 0x00000001,
	MPEG2_LOAD_NON_INTRA_QUANTISER_MATRIX_FLAG = 0x00000002
};

/*
 * struct mpeg2_decoded_buffer_address_t
 *
 * defines the addresses where the decoded pictures will be stored
 *
 * @struct_size:		size of the structure in bytes
 * @decoded_luma_p:		address of the luma buffer
 * @decoded_chroma_p:		address of the chroma buffer
 * @decoded_temporal_reference_value:	temporal_reference value
 *				of the decoded (current) picture
 * @mb_descr_p:			buffer where to store data related
 *				to every MBs of the picture
 */
struct mpeg2_decoded_buffer_address_t {
	u32 struct_size;
	u32 decoded_luma_p;
	u32 decoded_chroma_p;
	u32 decoded_temporal_reference_value;	/*  */

	u32 mb_descr_p;
};

/*
 * struct mpeg2_display_buffer_address_t
 *
 * defines the addresses (used by the Display Reconstruction block)
 * where the pictures to be displayed will be stored
 *
 * @struct_size:		size of the structure in bytes
 * @display_luma_p:		address of the luma buffer
 * @display_chroma_p:		address of the chroma buffer
 * @display_decimated_luma_p:	address of the decimated luma buffer
 * @display_decimated_chroma_p:	address of the decimated chroma buffer
 */
struct mpeg2_display_buffer_address_t {
	u32 struct_size;
	u32 display_luma_p;
	u32 display_chroma_p;
	u32 display_decimated_luma_p;
	u32 display_decimated_chroma_p;
};

/*
 * struct mpeg2_display_buffer_address_t
 *
 * defines the addresses where the two reference pictures
 * will be stored
 *
 * @struct_size:		size of the structure in bytes
 * @backward_reference_luma_p:	address of the backward reference luma buffer
 * @backward_reference_chroma_p:address of the backward reference chroma buffer
 * @backward_temporal_reference_value:	temporal_reference value of the
 *				backward reference picture
 * @forward_reference_luma_p:	address of the forward reference luma buffer
 * @forward_reference_chroma_p:	address of the forward reference chroma buffer
 * @forward_temporal_reference_value:	temporal_reference value of the
 *				forward reference picture
 */
struct mpeg2_ref_pic_list_address_t {
	u32 struct_size;
	u32 backward_reference_luma_p;
	u32 backward_reference_chroma_p;
	u32 backward_temporal_reference_value;
	u32 forward_reference_luma_p;
	u32 forward_reference_chroma_p;
	u32 forward_temporal_reference_value;
};

/* identifies the type of chroma of the decoded picture */
enum mpeg2_chroma_format_t {
	MPEG2_CHROMA_RESERVED = 0,
	/* chroma type 4:2:0 */
	MPEG2_CHROMA_4_2_0 = 1,
	/* chroma type 4:2:2 */
	MPEG2_CHROMA_4_2_2 = 2,
	/* chroma type 4:4:4 */
	MPEG2_CHROMA_4_4_4 = 3
};

/* identifies the Intra DC Precision */
enum mpeg2_intra_dc_precision_t {
	/* 8 bits Intra DC Precision*/
	MPEG2_INTRA_DC_PRECISION_8_BITS = 0,
	/* 9 bits Intra DC Precision  */
	MPEG2_INTRA_DC_PRECISION_9_BITS = 1,
	/* 10 bits Intra DC Precision */
	MPEG2_INTRA_DC_PRECISION_10_BITS = 2,
	/* 11 bits Intra DC Precision */
	MPEG2_INTRA_DC_PRECISION_11_BITS = 3
};

/*
 * decoding errors bitfield returned by firmware, several bits can be
 * raised at the same time to signal several errors.
 */
enum mpeg2_decoding_error_t {
	/* the firmware decoding was successful */
	MPEG2_DECODER_NO_ERROR = (MPEG2_DECODER_BASE + 0),
	/*
	 * the firmware decoded too much MBs:
	 * - The mpeg2_command_status_t.status doesn't locate
	 *   these erroneous MBs because the firmware can't know
	 *   where are these extra MBs.
	 * - MPEG2_DECODER_ERROR_RECOVERED could also be set
	 */
	MPEG2_DECODER_ERROR_MB_OVERFLOW = (MPEG2_DECODER_BASE + 1),
	/*
	 * the firmware encountered error(s) that were recovered:
	 * - mpeg2_command_status_t.status locates the erroneous MBs.
	 * - MPEG2_DECODER_ERROR_MB_OVERFLOW could also be set
	 */
	MPEG2_DECODER_ERROR_RECOVERED = (MPEG2_DECODER_BASE + 2),
	/*
	 * the firmware encountered an error that can't be recovered:
	 * - mpeg2_command_status_t.status has no meaning
	 */
	MPEG2_DECODER_ERROR_NOT_RECOVERED = (MPEG2_DECODER_BASE + 4),
	/*
	 * the firmware task is hanged and doesn't get back to watchdog
	 * task even after maximum time alloted has lapsed:
	 * - mpeg2_command_status_t.status has no meaning.
	 */
	MPEG2_DECODER_ERROR_TASK_TIMEOUT = (MPEG2_DECODER_BASE + 8),
	/* This feature is not supported by firmware */
	MPEG2_DECODER_ERROR_FEATURE_NOT_SUPPORTED = (MPEG2_DECODER_BASE + 16)
};

/*
 * struct mpeg2_set_global_param_sequence_t
 *
 * overall video sequence parameters required
 * by firmware to prepare picture decoding
 *
 * @struct_size:		size of the structure in bytes
 * @mpeg_stream_type_flag:	type of the bitstream MPEG1/MPEG2:
 *				 = 0  for MPEG1 coded stream,
 *				 = 1  for MPEG2 coded stream
 * @horizontal_size:		horizontal size of the video picture: based
 *				on the two elements "horizontal_size_value"
 *				and "horizontal_size_extension"
 * @vertical_size:		vertical size of the video picture: based
 *				on the two elements "vertical_size_value"
 *				and "vertical_size_extension"
 * @progressive_sequence:	progressive/interlaced sequence
 * @chroma_format:		type of chroma of the decoded picture
 * @matrix_flags:		load or not the intra or non-intra
 *				quantisation matrices
 * @intra_quantiser_matrix:	intra quantisation matrix
 * @non_intra_quantiser_matrix:	non-intra quantisation matrix
 * @chroma_intra_quantiser_matrix:	chroma of intra quantisation matrix
 * @chroma_non_intra_quantiser_matrix:	chroma of non-intra quantisation matrix
 */
struct mpeg2_set_global_param_sequence_t {
	u32 struct_size;
	bool mpeg_stream_type_flag;
	u32 horizontal_size;
	u32 vertical_size;
	u32 progressive_sequence;
	enum mpeg2_chroma_format_t chroma_format;
	enum mpeg2_default_matrix_flags_t matrix_flags;
	u8 intra_quantiser_matrix[MPEG2_Q_MATRIX_SIZE];
	u8 non_intra_quantiser_matrix[MPEG2_Q_MATRIX_SIZE];
	u8 chroma_intra_quantiser_matrix[MPEG2_Q_MATRIX_SIZE];
	u8 chroma_non_intra_quantiser_matrix[MPEG2_Q_MATRIX_SIZE];
};

/*
 * struct mpeg2_param_picture_t
 *
 * picture specific parameters required by firmware to perform a picture decode
 *
 * @struct_size:		size of the structure in bytes
 * @picture_coding_type:	identifies the picture prediction
 *				(none, one or two reference pictures)
 * @forward_horizontal_f_code:	motion vector: forward horizontal F code
 * @forward_vertical_f_code:	motion vector: forward vertical F code
 * @backward_horizontal_f_code:	motion vector: backward horizontal F code
 * @backward_vertical_f_code:	motion vector: backward vertical F code
 * @intra_dc_precision:		inverse quantisation process precision
 * @picture_structure:		picture structure type (progressive,
 *				interlaced top/bottom)
 * @mpeg_decoding_flags:	flags to control decoding process
 */
struct mpeg2_param_picture_t {
	u32 struct_size;
	enum mpeg2_picture_coding_type_t picture_coding_type;
	u32 forward_horizontal_f_code;
	u32 forward_vertical_f_code;
	u32 backward_horizontal_f_code;
	u32 backward_vertical_f_code;
	enum mpeg2_intra_dc_precision_t intra_dc_precision;
	enum mpeg2_picture_structure_t picture_structure;
	enum mpeg_decoding_flags_t mpeg_decoding_flags;
};

/*
 * struct mpeg2_transform_param_t
 *
 * control parameters required by firmware to decode a picture
 *
 * @struct_size:		size of the structure in bytes
 * @picture_start_addr_compressed_buffer_p:	start address of the
 *						compressed MPEG data
 * @picture_stop_addr_compressed_buffer_p:	stop address of the
 *						compressed MPEG data
 * @decoded_buffer_address:	buffer addresses of decoded frame
 * @display_buffer_address:	buffer addresses of decoded frame
 *				to be displayed
 * @ref_pic_list_address:	buffer addresses where the backward
 *				and forward reference pictures will
 *				be stored
 * @main_aux_enable:		output reconstruction stage control
 * @horizontal_decimation_factor: horizontal decimation control
 * @vertical_decimation_factor:	vertical decimation control
 * @decoding_mode:		decoding control (normal,
 *				recovery, downgraded...)
 * @additional_flags:		optional additional decoding controls
 *				(deblocking, deringing...)
 * @picture_parameters:		picture specific parameters
 */
struct mpeg2_transform_param_t {
	u32 struct_size;
	u32 picture_start_addr_compressed_buffer_p;
	u32 picture_stop_addr_compressed_buffer_p;
	struct mpeg2_decoded_buffer_address_t decoded_buffer_address;
	struct mpeg2_display_buffer_address_t display_buffer_address;
	struct mpeg2_ref_pic_list_address_t ref_pic_list_address;
	enum mpeg2_rcn_ref_disp_enable_t main_aux_enable;
	enum mpeg2_horizontal_deci_factor_t horizontal_decimation_factor;
	enum mpeg2_vertical_deci_factor_t vertical_decimation_factor;
	enum mpeg2_decoding_mode_t decoding_mode;
	enum mpeg2_additional_flags_t additional_flags;
	struct mpeg2_param_picture_t picture_parameters;
	bool reserved;
};

/*
 * struct mpeg2_init_transformer_param_t
 *
 * defines the addresses where the decoded pictures will be stored
 *
 * @input_buffer_begin:	 start address of the input circular buffer
 * @input_buffer_end:	stop address of the input circular buffer
 */
struct mpeg2_init_transformer_param_t {
	u32 input_buffer_begin;
	u32 input_buffer_end;
	bool reserved;
};

#define MPEG2_STATUS_PARTITION   6

/*
 * struct mpeg2_init_transformer_param_t
 *
 * @struct_size:	size of the structure in bytes
 * @status:		decoding quality indicator which can be used
 *			to assess the level corruption of input
 *			MPEG bitstream. The picture to decode
 *			is divided into a maximum of
 *			MPEG2_STATUS_PARTITION * MPEG2_STATUS_PARTITION
 *			areas in order to locate the decoding errors.
 * @error_code:		decoding errors bitfield returned by firmware
 * @decode_time_in_micros:	stop address of the input circular buffer
 * @ceh_registers:	array where values of the Contrast Enhancement
 *			Histogram (CEH) registers will be stored.
 *			ceh_registers[0] correspond to register MBE_CEH_0_7,
 *			ceh_registers[1] correspond to register MBE_CEH_8_15,
 *			ceh_registers[2] correspond to register MBE_CEH_16_23.
 *			Note that elements of this array will be updated
 *			only if mpeg2_transform_param_t.additional_flags has
 *			the flag MPEG2_ADDITIONAL_FLAG_CEH set.
 *			They will remain unchanged otherwise.
 * @picture_mean_qp:	picture mean QP factor
 * @picture_variance_qp:picture variance QP factor
 */
struct mpeg2_command_status_t {
	u32 struct_size;
	u8 status[MPEG2_STATUS_PARTITION][MPEG2_STATUS_PARTITION];
	enum mpeg2_decoding_error_t error_code;
	u32 decode_time_in_micros;
	u32 ceh_registers[MPEG2_NUMBER_OF_CEH_INTERVALS];
	u32 picture_mean_qp;
	u32 picture_variance_qp;
};

#endif /* DELTA_MPEG2_FW_H */
