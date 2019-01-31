// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2019 Randy Li <ayaka@soulik.info>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#ifndef RKVDEC_REGS_H_
#define RKVDEC_REGS_H_

#define RKVDEC_REG_SYS_CTRL			0x008
#define RKVDEC_REG_SYS_CTRL_INDEX		(2)
#define		RKVDEC_GET_FORMAT(x)		(((x) >> 20) & 0x3)
#define		RKVDEC_FMT_H265D		(0)
#define		RKVDEC_FMT_H264D		(1)
#define		RKVDEC_FMT_VP9D			(2)

struct rkvdec_regs {
	struct {
		u32 minor_ver:8;
		u32 level:1;
		u32 dec_support:3;
		u32 profile:1;
		u32 reserve0:1;
		u32 codec_flag:1;
		u32 reserve1:1;
		u32 prod_num:16;
	} sw_id;

#if 0
	struct {
		u32 dec_e:1;
		u32 dec_clkgate_e:1;
		u32 reserved0:1;
		u32 timeout_mode:1;
		u32 dec_irq_dis:1;
		u32 dec_timeout_e:1;
		u32 buf_empty_en:1;
		u32 stmerror_waitdecfifo_empty:1;
		u32 dec_irq:1;
		u32 dec_irq_raw:1;
		u32 reserved2:2;
		u32 dec_rdy_sta:1;
		u32 dec_bus_sta:1;
		u32 dec_error_sta:1;
		u32 dec_timeout_sta:1;
		u32 dec_empty_sta:1;
		u32 colmv_ref_error_sta:1;
		u32 cabu_end_sta:1;
		u32 h264orvp9_error_mode:1;
		u32 softrst_en_p:1;
		u32 force_softreset_valid:1;
		u32 softreset_rdy:1;
		u32 reserved1:9;
	} sw01;
#else

	struct swreg_int {
		u32 sw_dec_e:1;
		u32 sw_dec_clkgate_e:1;
		u32 reserve0:2;
		u32 sw_dec_irq_dis:1;
		u32 sw_dec_timeout_e:1;
		u32 sw_buf_empty_en:1;
		u32 reserve1:1;
		u32 sw_dec_irq:1;
		u32 sw_dec_irq_raw:1;
		u32 reserve2:2;
		u32 sw_dec_rdy_sta:1;
		u32 sw_dec_bus_sta:1;
		u32 sw_dec_error_sta:1;
		u32 sw_dec_empty_sta:1;
		u32 reserve4:4;
		u32 sw_softrst_en_p:1;
		u32 sw_force_softreset_valid:1;
		u32 sw_softreset_rdy:1;
		u32 sw_wr_ddr_align_en:1;
		u32 sw_scl_down_en:1;
		u32 sw_allow_not_wr_unref_bframe:1;
	} sw_interrupt;
#endif

	struct swreg_sysctrl {
		u32 sw_in_endian:1;
		u32 sw_in_swap32_e:1;
		u32 sw_in_swap64_e:1;
		u32 sw_str_endian:1;
		u32 sw_str_swap32_e:1;
		u32 sw_str_swap64_e:1;
		u32 sw_out_endian:1;
		u32 sw_out_swap32_e:1;
		u32 sw_out_cbcr_swap:1;
		u32 reserve:1;
		u32 sw_rlc_mode_direct_write:1;
		u32 sw_rlc_mode:1;
		u32 sw_strm_start_bit:7;
	} sw_sysctrl;

	struct swreg_pic {
		u32 sw_y_hor_virstride:9;
		u32 reserve:3;
		u32 sw_uv_hor_virstride:9;
		u32 sw_slice_num:8;
	} sw_picparameter;

	u32 sw_strm_rlc_base;
	u32 sw_stream_len;
	u32 sw_cabactbl_base;
	u32 sw_decout_base;
	u32 sw_y_virstride;
	u32 sw_yuv_virstride;
	u32 sw_refer_base[15];
	RK_S32 sw_refer_poc[15];
	/* SWREG 40 */
	RK_S32 sw_cur_poc;
	u32 sw_rlcwrite_base;
	u32 sw_pps_base;
	u32 sw_rps_base;
	u32 cabac_error_en;
	/* SWREG 45 */
	u32 cabac_error_status;

	struct cabac_error_ctu {
		u32 sw_cabac_error_ctu_xoffset:8;
		u32 sw_cabac_error_ctu_yoffset:8;
		u32 sw_streamfifo_space2full:7;
		u32 reversed0:9;
	} cabac_error_ctu;

	/* SWREG 47 */
	struct sao_ctu_position {
		u32 sw_saowr_xoffset:9;
		u32 reversed0:7;
		u32 sw_saowr_yoffset:10;
		u32 reversed1:6;
	} sao_ctu_position;

	/* SWREG 48 */
	u32 sw_ref_valid;
	/* SWREG 49 - 63 */
	u32 sw_refframe_index[15];

	/* SWREG 64 */
	u32 performance_cycle;
	u32 axi_ddr_rdata;
	u32 axi_ddr_wdata;
	u32 swreg67_reserved;

	struct {
		u32 perf_cnt0_sel:6;
		u32 reserved2:2;
		u32 perf_cnt1_sel:6;
		u32 reserved1:2;
		u32 perf_cnt2_sel:6;
		u32 reserved0:10;
	} sw68_perf_sel;

	u32 sw69_perf_cnt0;
	u32 sw70_perf_cnt1;
	u32 sw71_perf_cnt2;
	u32 sw72_h264_refer30_poc;
	u32 sw73_h264_refer31_poc;
	u32 sw74_h264_cur_poc1;
	u32 sw75_errorinfo_base;

	struct {
		u32 slicedec_num:14;
		u32 reserved1:1;
		u32 strmd_detect_error_flag:1;
		u32 sw_error_packet_num:14;
		u32 reserved0:2;
	} sw76_errorinfo_num;

	/* SWREG 77 */
	u32 extern_error_en;
};

#if 0
typedef struct h264d_rkv_regs_t {
	struct {
		u32 minor_ver:8;
		u32 level:1;
		u32 dec_support:3;
		u32 profile:1;
		u32 reserve0:1;
		u32 codec_flag:1;
		u32 reserve1:1;
		u32 prod_num:16;
	} sw00;
	struct {
		u32 dec_e:1;	//0
		u32 dec_clkgate_e:1;	// 1
		u32 reserve0:1;	// 2
		u32 timeout_mode:1;	// 3
		u32 dec_irq_dis:1;	//4    // 4
		u32 dec_timeout_e:1;	//5
		u32 buf_empty_en:1;	// 6
		u32 stmerror_waitdecfifo_empty:1;	// 7
		u32 dec_irq:1;	// 8
		u32 dec_irq_raw:1;	// 9
		u32 reserve2:2;
		u32 dec_rdy_sta:1;	//12
		u32 dec_bus_sta:1;	//13
		u32 dec_error_sta:1;	// 14
		u32 dec_timeout_sta:1;	//15
		u32 dec_empty_sta:1;	// 16
		u32 colmv_ref_error_sta:1;	// 17
		u32 cabu_end_sta:1;	// 18
		u32 h264orvp9_error_mode:1;	//19
		u32 softrst_en_p:1;	//20
		u32 force_softreset_valid:1;	//21
		u32 softreset_rdy:1;	// 22
		u32 reserve1:9;
	} sw01;
	struct {
		u32 in_endian:1;
		u32 in_swap32_e:1;
		u32 in_swap64_e:1;
		u32 str_endian:1;
		u32 str_swap32_e:1;
		u32 str_swap64_e:1;
		u32 out_endian:1;
		u32 out_swap32_e:1;
		u32 out_cbcr_swap:1;
		u32 reserve0:1;
		u32 rlc_mode_direct_write:1;
		u32 rlc_mode:1;
		u32 strm_start_bit:7;
		u32 reserve1:1;
		u32 dec_mode:2;
		u32 reserve2:2;
		u32 rps_mode:1;
		u32 stream_mode:1;
		u32 stream_lastpacket:1;
		u32 firstslice_flag:1;
		u32 frame_orslice:1;
		u32 buspr_slot_disable:1;
		u32 reverse3:2;
	} sw02;
	struct {
		u32 y_hor_virstride:9;
		u32 reserve:2;
		u32 slice_num_highbit:1;
		u32 uv_hor_virstride:9;
		u32 slice_num_lowbits:11;
	} sw03;
	struct {
		u32 strm_rlc_base:32;
	} sw04;
	struct {
		u32 stream_len:27;
		u32 reverse0:5;
	} sw05;
	struct {
		u32 cabactbl_base:32;
	} sw06;
	struct {
		u32 decout_base:32;
	} sw07;
	struct {
		u32 y_virstride:20;
		u32 reverse0:12;
	} sw08;
	struct {
		u32 yuv_virstride:21;
		u32 reverse0:11;
	} sw09;
	struct {
		u32 ref0_14_base:10;
		u32 ref0_14_field:1;
		u32 ref0_14_topfield_used:1;
		u32 ref0_14_botfield_used:1;
		u32 ref0_14_colmv_use_flag:1;
	} sw10_24[15];
	struct {
		u32 ref0_14_poc:32;
	} sw25_39[15];
	struct {
		u32 cur_poc:32;
	} sw40;
	struct {
		u32 rlcwrite_base;
	} sw41;
	struct {
		u32 pps_base;
	} sw42;
	struct {
		u32 rps_base;
	} sw43;
	struct {
		u32 strmd_error_e:28;
		u32 reserve:4;
	} sw44;
	struct {
		u32 strmd_error_status:28;
		u32 colmv_error_ref_picidx:4;
	} sw45;
	struct {
		u32 strmd_error_ctu_xoffset:8;
		u32 strmd_error_ctu_yoffset:8;
		u32 streamfifo_space2full:7;
		u32 reserve0:1;
		u32 vp9_error_ctu0_en:1;
		u32 reverse1:7;
	} sw46;
	struct {
		u32 saowr_xoffet:9;
		u32 reserve0:7;
		u32 saowr_yoffset:10;
		u32 reverse1:6;
	} sw47;
	struct {
		u32 ref15_base:10;
		u32 ref15_field:1;
		u32 ref15_topfield_used:1;
		u32 ref15_botfield_used:1;
		u32 ref15_colmv_use_flag:1;
		u32 reverse0:18;
	} sw48;
	struct {
		u32 ref15_29_poc:32;
	} sw49_63[15];
	struct {
		u32 performance_cycle:32;
	} sw64;
	struct {
		u32 axi_ddr_rdata:32;
	} sw65;
	struct {
		u32 axi_ddr_rdata:32;
	} sw66;
	struct {
		u32 busifd_resetn:1;
		u32 cabac_resetn:1;
		u32 dec_ctrl_resetn:1;
		u32 transd_resetn:1;
		u32 intra_resetn:1;
		u32 inter_resetn:1;
		u32 recon_resetn:1;
		u32 filer_resetn:1;
		u32 reverse0:24;
	} sw67;
	struct {
		u32 perf_cnt0_sel:6;
		u32 reserve0:2;
		u32 perf_cnt1_sel:6;
		u32 reserve1:2;
		u32 perf_cnt2_sel:6;
		u32 reverse1:10;
	} sw68;
	struct {
		u32 perf_cnt0:32;
	} sw69;
	struct {
		u32 perf_cnt1:32;
	} sw70;
	struct {
		u32 perf_cnt2:32;
	} sw71;
	struct {
		u32 ref30_poc;
	} sw72;
	struct {
		u32 ref31_poc;
	} sw73;
	struct {
		u32 cur_poc1:32;
	} sw74;
	struct {
		u32 errorinfo_base:32;
	} sw75;
	struct {
		u32 slicedec_num:14;
		u32 reserve0:1;
		u32 strmd_detect_error_flag:1;
		u32 error_packet_num:14;
		u32 reverse1:2;
	} sw76;
	struct {
		u32 error_en_highbits:30;
		u32 reserve:2;
	} sw77;
	u32 reverse[2];
} H264dRkvRegs_t;
#endif

#endif
