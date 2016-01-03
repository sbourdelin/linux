/*
 * Copyright(c) 2015, Analogix Semiconductor. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */
#include <drm/drm_crtc.h>
#include <drm/drm_dp_helper.h>
#include <drm/drm_edid.h>

#include <linux/delay.h>
#include <linux/hdmi.h>
#include <linux/types.h>

#include "anx78xx.h"
#include "slimport_tx_drv.h"

#define XTAL_27M	270
#define XTAL_CLK	XTAL_27M

#define AUX_CH_BUFFER_SIZE	16

struct slimport {
	bool	hdcp_enabled;	/* HDCP control enable/ disable from AP */

	u8	tx_test_bw;
	bool	tx_test_lt;
	bool	tx_test_edid;

	u8	changed_bandwidth;

	bool	need_clean_status;

	u8	hdcp_error_count;
	u8	hdcp_fail_count;
	u8	audio_stable_count;	/* Audio stable counter */

	u8	edid_blocks[2 * EDID_LENGTH];

	bool	read_edid_flag;
	bool	down_sample_en;

	/* Infoframes */
	union hdmi_infoframe	frame;

	/* Interrupt status registers */
	u8	common_int[4];
	u8	dp_int;
	u8	sp_hdmi_int[7];

	enum sp_tx_state		tx_system_state;
	enum audio_output_status	tx_ao_state;
	enum video_output_status	tx_vo_state;
	enum sp_tx_lt_status		tx_lt_state;
	enum hdcp_status		hdcp_state;
	enum repeater_status		repeater_state;
};

static struct slimport sp;

static const u16 chipid_list[] = {
	0x7802,
	0x7806,
	0x7810,
	0x7812,
	0x7814,
	0x7816,
	0x7818,
};

static void sp_hdmi_audio_samplechg_int(struct anx78xx *anx78xx);
static void sp_hdmi_new_avi_int(struct anx78xx *anx78xx);
static void sp_hdmi_new_vsi_int(struct anx78xx *anx78xx);
static void sp_print_system_state(struct anx78xx *anx78xx,
				  enum sp_tx_state state);
static void sp_show_information(struct anx78xx *anx78xx);

/**
 * sp_reg_read: Read a value from a single register.
 *
 * @anx78xx: Device to read from.
 * @addr: Address to read from.
 * @reg: Register to be read from.
 * @val: Pointer to store read value.
 *
 * A value of zero will be returned on success, a negative errno will
 * be returned in error cases.
 */
static int sp_reg_read(struct anx78xx *anx78xx, u8 addr, u8 offset, u8 *val)
{
	int ret;
	struct i2c_client *client = anx78xx->client;

	client->addr = addr >> 1;

	ret = i2c_smbus_read_byte_data(client, offset);
	if (ret < 0) {
		dev_err(&client->dev, "failed to read i2c addr=%x\n", addr);
		return ret;
	}

	*val = ret;

	return 0;
}

/**
 * sp_reg_write(): Write a value to a single register
 *
 * @anx78xx: Device to read from.
 * @addr: Address to write to.
 * @offset: Byte interpreted by slave.
 * @val: Value to be written.
 *
 * A value of zero will be returned on success, a negative errno will
 * be returned in error cases.
 */
static int sp_reg_write(struct anx78xx *anx78xx, u8 addr, u8 offset, u8 val)
{
	int ret;
	struct i2c_client *client = anx78xx->client;

	client->addr = addr >> 1;

	ret = i2c_smbus_write_byte_data(client, offset, val);
	if (ret < 0)
		dev_err(&client->dev, "failed to write i2c addr=%x\n", addr);

	return ret;
}

/**
 * sp_reg_update_bits: Perform a read/modify/write cycle on the register.
 *
 * @anx78xx: Device to read from.
 * @addr: Address to write to.
 * @offset: Byte interpreted by slave.
 * @mask: Bitmask to change.
 * @val: New value for bitmask.
 *
 * Returns zero for success, a negative number on error.
 */
static int sp_reg_update_bits(struct anx78xx *anx78xx, u8 addr, u8 offset,
			      u8 mask, u8 val)
{
	int ret;
	u8 orig, tmp;

	ret = sp_reg_read(anx78xx, addr, offset, &orig);
	if (ret < 0)
		return ret;

	tmp = orig & ~mask;
	tmp |= val & mask;

	return sp_reg_write(anx78xx, addr, offset, tmp);
}

/**
 * sp_reg_set_bits: Perform a read/write cycle to set bits in register.
 *
 * @anx78xx: Device to read from.
 * @addr: Address to write to.
 * @offset: Byte interpreted by slave.
 * @mask: Bitmask to change.
 *
 * Returns zero for success, a negative number on error.
 */
static inline int sp_reg_set_bits(struct anx78xx *anx78xx, u8 addr,
				  u8 offset, u8 mask)
{
	return sp_reg_update_bits(anx78xx, addr, offset, mask, mask);
}

/**
 * sp_reg_clear_bits: Perform a read/write cycle to clear bits in register.
 *
 * @anx78xx: Device to read from.
 * @addr: Address to write to.
 * @offset: Byte interpreted by slave.
 * @mask: Bitmask to change.
 *
 * Returns zero for success, a negative number on error.
 */
static inline int sp_reg_clear_bits(struct anx78xx *anx78xx, u8 addr,
				    u8 offset, u8 mask)
{
	return sp_reg_update_bits(anx78xx, addr, offset, mask, 0);
}

static inline void sp_video_mute(struct anx78xx *anx78xx, bool enable)
{
	if (enable)
		sp_reg_set_bits(anx78xx, TX_P2, SP_VID_CTRL1_REG,
				SP_VIDEO_MUTE);
	else
		sp_reg_clear_bits(anx78xx, TX_P2, SP_VID_CTRL1_REG,
				  SP_VIDEO_MUTE);
}

static inline void sp_hdmi_mute_audio(struct anx78xx *anx78xx, bool enable)
{
	if (enable)
		sp_reg_set_bits(anx78xx, RX_P0, SP_HDMI_MUTE_CTRL_REG,
				SP_AUD_MUTE);
	else
		sp_reg_clear_bits(anx78xx, RX_P0, SP_HDMI_MUTE_CTRL_REG,
				  SP_AUD_MUTE);
}

static inline void sp_hdmi_mute_video(struct anx78xx *anx78xx, bool enable)
{
	if (enable)
		sp_reg_set_bits(anx78xx, RX_P0, SP_HDMI_MUTE_CTRL_REG,
				SP_VID_MUTE);
	else
		sp_reg_clear_bits(anx78xx, RX_P0, SP_HDMI_MUTE_CTRL_REG,
				  SP_VID_MUTE);
}

static inline void sp_addronly_set(struct anx78xx *anx78xx, bool enable)
{
	if (enable)
		sp_reg_set_bits(anx78xx, TX_P0, SP_DP_AUX_CH_CTRL2_REG,
				SP_ADDR_ONLY);
	else
		sp_reg_clear_bits(anx78xx, TX_P0, SP_DP_AUX_CH_CTRL2_REG,
				  SP_ADDR_ONLY);
}

static inline void sp_set_link_bandwidth(struct anx78xx *anx78xx, u8 bw)
{
	sp_reg_write(anx78xx, TX_P0, SP_DP_MAIN_LINK_BW_SET_REG, bw);
}

static inline u8 sp_get_link_bandwidth(struct anx78xx *anx78xx)
{
	u8 val;

	sp_reg_read(anx78xx, TX_P0, SP_DP_MAIN_LINK_BW_SET_REG, &val);

	return val & SP_LINK_BW_SET_MASK;
}

static inline bool sp_get_pll_lock_status(struct anx78xx *anx78xx)
{
	u8 val;

	sp_reg_read(anx78xx, TX_P0, SP_DP_DEBUG1_REG, &val);

	return (val & SP_DEBUG_PLL_LOCK) != 0;
}

static inline void sp_gen_m_clk_with_downspreading(struct anx78xx *anx78xx)
{
	sp_reg_set_bits(anx78xx, TX_P0, SP_DP_M_CALCULATION_CTRL_REG,
			SP_M_GEN_CLK_SEL);
}

static inline void sp_gen_m_clk_without_downspreading(struct anx78xx *anx78xx)
{
	sp_reg_clear_bits(anx78xx, TX_P0, SP_DP_M_CALCULATION_CTRL_REG,
			  SP_M_GEN_CLK_SEL);
}

static inline void sp_hdmi_set_hpd(struct anx78xx *anx78xx, bool enable)
{
	if (enable)
		sp_reg_set_bits(anx78xx, TX_P2, SP_VID_CTRL3_REG, SP_HPD_OUT);
	else
		sp_reg_clear_bits(anx78xx, TX_P2, SP_VID_CTRL3_REG,
				  SP_HPD_OUT);
}

static inline void sp_hdmi_set_termination(struct anx78xx *anx78xx,
					   bool enable)
{
	if (enable)
		sp_reg_clear_bits(anx78xx, RX_P0, SP_TMDS_CTRL_BASE + 7,
				  SP_PD_RT);
	else
		sp_reg_set_bits(anx78xx, RX_P0, SP_TMDS_CTRL_BASE + 7,
				SP_PD_RT);
}

static inline bool sp_hdcp_repeater_mode(struct anx78xx *anx78xx)
{
	u8 val;

	sp_reg_read(anx78xx, RX_P1, SP_HDCP_BCAPS_SHADOW_REG, &val);

	return (val & SP_BCAPS_REPEATER);
}

static inline void sp_clean_hdcp_status(struct anx78xx *anx78xx)
{
	sp_reg_write(anx78xx, TX_P0, SP_HDCP_CTRL0_REG,
		     SP_BKSV_SRM_PASS | SP_KSVLIST_VLD);
	sp_reg_set_bits(anx78xx, TX_P0, SP_HDCP_CTRL0_REG, SP_RE_AUTH);
	usleep_range(2000, 4000);
}

static const u8 dp_tx_output_precise_tune_bits[20] = {
	0x01, 0x03, 0x07, 0x7f, 0x71, 0x6b, 0x7f,
	0x73, 0x7f, 0x7f, 0x00, 0x00, 0x00, 0x00,
	0x0c, 0x42, 0x1e, 0x3e, 0x72, 0x7e,
};

static void sp_link_phy_initialization(struct anx78xx *anx78xx)
{
	int i;

	/*
	 * REVISIT : It is writing to a RESERVED bits in Analog Control 0
	 * register.
	 */
	sp_reg_write(anx78xx, TX_P2, SP_ANALOG_CTRL0_REG, 0x02);

	/*
	 * Write DP TX output emphasis precise tune bits.
	 */
	for (i = 0; i < ARRAY_SIZE(dp_tx_output_precise_tune_bits); i++)
		sp_reg_write(anx78xx, TX_P1, SP_DP_TX_LT_CTRL0_REG + i,
			     dp_tx_output_precise_tune_bits[i]);
}

static void sp_set_system_state(struct anx78xx *anx78xx,
				enum sp_tx_state new_state)
{
	u8 val;

	if ((sp.tx_system_state >= STATE_LINK_TRAINING) &&
	    (new_state < STATE_LINK_TRAINING))
		sp_reg_set_bits(anx78xx, TX_P0, SP_DP_ANALOG_POWER_DOWN_REG,
				SP_CH0_PD);

	if (sp.tx_system_state >= STATE_LINK_TRAINING) {
		if (new_state >= STATE_AUDIO_OUTPUT) {
			sp_hdmi_mute_audio(anx78xx, true);
		} else {
			sp_hdmi_mute_video(anx78xx, true);
			sp_video_mute(anx78xx, true);
		}
	}

	if (!sp_hdcp_repeater_mode(anx78xx)) {
		if (sp.tx_system_state >= STATE_HDCP_AUTH &&
		    new_state <= STATE_HDCP_AUTH) {
			sp_reg_read(anx78xx, TX_P0, SP_HDCP_CTRL0_REG, &val);
			if (val & ~(SP_KSVLIST_VLD | SP_BKSV_SRM_PASS))
				sp_clean_hdcp_status(anx78xx);
		}
	} else {
		if (sp.tx_system_state > STATE_LINK_TRAINING &&
		    new_state <= STATE_LINK_TRAINING) {
			/* Inform AP to re-auth */
			sp_hdmi_set_hpd(anx78xx, false);
			sp_hdmi_set_termination(anx78xx, false);
			msleep(50);
		}
	}

	sp.tx_system_state = new_state;
	sp.hdcp_state = HDCP_CAPABLE_CHECK;
	sp.tx_lt_state = LT_INIT;
	sp.tx_vo_state = VO_WAIT_VIDEO_STABLE;
	/* Reset audio stable counter */
	sp.audio_stable_count = 0;
	sp_print_system_state(anx78xx, sp.tx_system_state);
}

static inline void sp_reg_hardware_reset(struct anx78xx *anx78xx)
{
	sp_reg_set_bits(anx78xx, TX_P2, SP_RESET_CTRL1_REG, SP_HW_RST);
	sp_variable_init(anx78xx);
	sp_set_system_state(anx78xx, STATE_SP_INITIALIZED);
	msleep(500);
}

static int sp_wait_aux_op_finish(struct anx78xx *anx78xx)
{
	u8 retry_cnt;
	u8 val;
	struct device *dev = &anx78xx->client->dev;

	retry_cnt = 150;
	while (--retry_cnt) {
		sp_reg_read(anx78xx, TX_P0, SP_DP_AUX_CH_CTRL2_REG, &val);
		if (!(val & SP_AUX_EN))
			break;
		usleep_range(2000, 4000);
	}

	if (retry_cnt == 0) {
		dev_err(dev, "aux operate failed!\n");
		return -ETIMEDOUT;
	}

	sp_reg_read(anx78xx, TX_P0, SP_AUX_CH_STATUS_REG, &val);
	if (val & SP_AUX_STATUS) {
		dev_err(dev, "wait aux operation status %.2x\n", val);
		return -ETIMEDOUT;
	}

	return 0;
}

static void sp_print_system_state(struct anx78xx *anx78xx,
				  enum sp_tx_state state)
{
	static const char *const name[] = {
		[STATE_CABLE_UNPLUGGED]		= "Cable is pulled out",
		[STATE_WAITING_CABLE_PLUG]	= "Waiting cable plug",
		[STATE_SP_INITIALIZED]		= "SlimPort initialized",
		[STATE_SINK_CONNECTION]		= "Sink connection",
		[STATE_PARSE_EDID]		= "Parse EDID",
		[STATE_LINK_TRAINING]		= "Link training",
		[STATE_VIDEO_OUTPUT]		= "Video output",
		[STATE_HDCP_AUTH]		= "HDCP authentication",
		[STATE_AUDIO_OUTPUT]		= "Audio output",
		[STATE_PLAY_BACK]		= "Playback",
	};

	dev_dbg(&anx78xx->client->dev, "SlimPort state: %s\n",
		(state < ARRAY_SIZE(name)) ? name[state] : "INVALID");
}

static void sp_reset_aux(struct anx78xx *anx78xx)
{
	sp_reg_set_bits(anx78xx, TX_P2, SP_RESET_CTRL2_REG, SP_AUX_RST);
	sp_reg_clear_bits(anx78xx, TX_P2, SP_RESET_CTRL2_REG, SP_AUX_RST);
}

static void sp_write_dpcd_addr(struct anx78xx *anx78xx, unsigned int addr)
{
	sp_reg_write(anx78xx, TX_P0, SP_AUX_ADDR_7_0_REG, addr & 0xff);
	sp_reg_write(anx78xx, TX_P0, SP_AUX_ADDR_15_8_REG,
		     (addr & 0xff00) >> 8);

	/*
	 * DP AUX CH Address Register #2, only update bits[3:0]
	 * [7:4] RESERVED
	 * [3:0] AUX_ADDR[19:16], Register control AUX CH address.
	 */
	sp_reg_update_bits(anx78xx, TX_P0, SP_AUX_ADDR_19_16_REG,
			   SP_AUX_ADDR_19_16_MASK, (addr & 0xf0000) >> 16);
}

static int sp_dp_read_bytes_from_dpcd(struct anx78xx *anx78xx,
				      unsigned int addr, u8 count, u8 *buf)
{
	u8 val, val1, i;

	if (count > AUX_CH_BUFFER_SIZE)
		return -EINVAL;

	/* Clear AUX CH data buffer */
	sp_reg_write(anx78xx, TX_P0, SP_BUF_DATA_COUNT_REG, SP_BUF_CLR);
	sp_reg_write(anx78xx, TX_P0, SP_DP_AUX_CH_CTRL1_REG,
		     ((count - 1) << SP_AUX_LENGTH_SHIFT) | 0x09);
	sp_write_dpcd_addr(anx78xx, addr);
	sp_reg_set_bits(anx78xx, TX_P0, SP_DP_AUX_CH_CTRL2_REG, SP_AUX_EN);
	usleep_range(2000, 4000);

	if (sp_wait_aux_op_finish(anx78xx)) {
		sp_reg_read(anx78xx, TX_P2, SP_DP_INT_STATUS_REG, &val);
		sp_reg_read(anx78xx, TX_P0, SP_DP_DEBUG1_REG, &val1);
		if (!(val1 & SP_POLLING_EN) || (val & SP_POLLING_ERR))
			sp_reset_aux(anx78xx);
		return -ETIMEDOUT;
	}

	for (i = 0; i < count; i++) {
		sp_reg_read(anx78xx, TX_P0, SP_DP_BUF_DATA0_REG + i, &val);
		buf[i] = val;
	}

	return 0;
}

static int sp_dp_write_bytes_to_dpcd(struct anx78xx *anx78xx,
				     unsigned int addr, u8 count, u8 *buf)
{
	int i;

	if (count > AUX_CH_BUFFER_SIZE)
		return -EINVAL;

	sp_reg_write(anx78xx, TX_P0, SP_DP_AUX_CH_CTRL1_REG,
		     ((count - 1) << SP_AUX_LENGTH_SHIFT) | 0x08);
	sp_write_dpcd_addr(anx78xx, addr);
	for (i = 0; i < count; i++)
		sp_reg_write(anx78xx, TX_P0, SP_DP_BUF_DATA0_REG + i, buf[i]);

	sp_reg_set_bits(anx78xx, TX_P0, SP_DP_AUX_CH_CTRL2_REG, SP_AUX_EN);
	return sp_wait_aux_op_finish(anx78xx);
}

static void sp_block_power_ctrl(struct anx78xx *anx78xx,
				enum sp_tx_power_block sp_tx_pd_block,
				bool power)
{
	if (power)
		sp_reg_clear_bits(anx78xx, TX_P2, SP_POWERDOWN_CTRL_REG,
				  sp_tx_pd_block);
	else
		sp_reg_set_bits(anx78xx, TX_P2, SP_POWERDOWN_CTRL_REG,
				sp_tx_pd_block);

	dev_dbg(&anx78xx->client->dev,
		"sp_tx_power_on: %.2x\n", sp_tx_pd_block);
}

void sp_variable_init(struct anx78xx *anx78xx)
{
	sp.hdcp_enabled = false;

	sp.tx_system_state = STATE_WAITING_CABLE_PLUG;
	sp_print_system_state(anx78xx, sp.tx_system_state);

	sp.read_edid_flag = false;

	memset(sp.edid_blocks, 0, sizeof(*sp.edid_blocks));

	sp.tx_lt_state = LT_INIT;
	sp.hdcp_state = HDCP_CAPABLE_CHECK;
	sp.repeater_state = HDCP_DONE;
	sp.tx_vo_state = VO_WAIT_VIDEO_STABLE;
	sp.tx_ao_state = AO_INIT;
	sp.changed_bandwidth = DP_LINK_BW_5_4;

	sp.hdcp_error_count = 0;
	sp.hdcp_fail_count = 0;
	sp.audio_stable_count = 0;

	sp.tx_test_lt = false;
	sp.tx_test_bw = 0;
	sp.tx_test_edid = false;
}

static void sp_hdmi_tmds_phy_initialization(struct anx78xx *anx78xx)
{
	sp_reg_write(anx78xx, RX_P0, SP_TMDS_CTRL_BASE + 1, 0x90);
	sp_reg_write(anx78xx, RX_P0, SP_TMDS_CTRL_BASE + 2, 0xa9);
	sp_reg_write(anx78xx, RX_P0, SP_TMDS_CTRL_BASE + 6, 0x92);
	sp_reg_write(anx78xx, RX_P0, SP_TMDS_CTRL_BASE + 7, 0x80);
	sp_reg_write(anx78xx, RX_P0, SP_TMDS_CTRL_BASE + 20, 0xf2);
}

static void sp_hdmi_initialization(struct anx78xx *anx78xx)
{
	sp_reg_write(anx78xx, RX_P0, SP_HDMI_MUTE_CTRL_REG, SP_AUD_MUTE |
		     SP_VID_MUTE);
	sp_reg_set_bits(anx78xx, RX_P0, SP_CHIP_CTRL_REG, SP_MAN_HDMI5V_DET |
			SP_PLLLOCK_CKDT_EN | SP_DIGITAL_CKDT_EN);

	sp_reg_set_bits(anx78xx, RX_P0, SP_SOFTWARE_RESET1_REG,
			SP_HDCP_MAN_RST | SP_SW_MAN_RST | SP_TMDS_RST |
			SP_VIDEO_RST);
	sp_reg_clear_bits(anx78xx, RX_P0, SP_SOFTWARE_RESET1_REG,
			  SP_HDCP_MAN_RST | SP_SW_MAN_RST | SP_TMDS_RST |
			  SP_VIDEO_RST);

	/* Sync detect change, GP set mute */
	sp_reg_set_bits(anx78xx, RX_P0, SP_AUD_EXCEPTION_ENABLE_BASE + 1,
			BIT(5) | BIT(6));
	sp_reg_set_bits(anx78xx, RX_P0, SP_AUD_EXCEPTION_ENABLE_BASE + 3,
			SP_AEC_EN21);
	sp_reg_set_bits(anx78xx, RX_P0, SP_AUDVID_CTRL_REG, SP_AVC_EN |
			SP_AAC_OE | SP_AAC_EN);

	sp_reg_clear_bits(anx78xx, RX_P0, SP_SYSTEM_POWER_DOWN1_REG,
			  SP_PWDN_CTRL);

	sp_reg_set_bits(anx78xx, RX_P0, SP_VID_DATA_RANGE_CTRL_REG,
			SP_R2Y_INPUT_LIMIT);
	sp_reg_write(anx78xx, RX_P0, SP_TMDS_CTRL_BASE + 22, 0xc4);
	sp_reg_write(anx78xx, RX_P0, SP_TMDS_CTRL_BASE + 23, 0x18);

	/* Enable DDC stretch */
	sp_reg_write(anx78xx, TX_P0, SP_DP_EXTRA_I2C_DEV_ADDR_REG,
		     SP_I2C_EXTRA_ADDR);

	sp_hdmi_tmds_phy_initialization(anx78xx);
	sp_hdmi_set_hpd(anx78xx, false);
	sp_hdmi_set_termination(anx78xx, false);
}

static void sp_xtal_clk_sel(struct anx78xx *anx78xx)
{
	u8 val;

	sp_reg_update_bits(anx78xx, TX_P2, SP_ANALOG_DEBUG2_REG,
			   SP_XTAL_FRQ | SP_FORCE_SW_OFF_BYPASS,
			   SP_XTAL_FRQ_27M);

	sp_reg_write(anx78xx, TX_P0, SP_DP_AUX_CH_CTRL3_REG,
		     XTAL_CLK & SP_WAIT_COUNTER_7_0_MASK);
	sp_reg_write(anx78xx, TX_P0, SP_DP_AUX_CH_CTRL4_REG,
		     ((XTAL_CLK & 0xff00) >> 2) | (XTAL_CLK / 10));

	sp_reg_write(anx78xx, TX_P0, SP_I2C_GEN_10US_TIMER0_REG,
		     XTAL_CLK & 0xff);
	sp_reg_write(anx78xx, TX_P0, SP_I2C_GEN_10US_TIMER1_REG,
		     (XTAL_CLK & 0xff00) >> 8);
	sp_reg_write(anx78xx, TX_P0, SP_AUX_MISC_CTRL_REG,
		     XTAL_CLK / 10 - 1);

	sp_reg_read(anx78xx, RX_P0, SP_HDMI_US_TIMER_CTRL_REG, &val);
	sp_reg_write(anx78xx, RX_P0, SP_HDMI_US_TIMER_CTRL_REG,
		     (val & SP_MS_TIMER_MARGIN_10_8_MASK) |
		     ((((XTAL_CLK / 10) >> 1) - 2) << 3));
}

void sp_tx_initialization(struct anx78xx *anx78xx)
{
	/* set terminal resistor to 50 ohm */
	sp_reg_write(anx78xx, TX_P0, SP_DP_AUX_CH_CTRL2_REG, 0x30);
	/* enable aux double diff output */
	sp_reg_set_bits(anx78xx, TX_P0, SP_DP_AUX_CH_CTRL2_REG, 0x08);

	if (!sp_hdcp_repeater_mode(anx78xx)) {
		sp_reg_clear_bits(anx78xx, TX_P0, SP_DP_HDCP_CTRL_REG,
				  SP_AUTO_EN | SP_AUTO_START);
		sp_reg_write(anx78xx, TX_P0, SP_OTP_KEY_PROTECT1_REG,
			     SP_OTP_PSW1);
		sp_reg_write(anx78xx, TX_P0, SP_OTP_KEY_PROTECT2_REG,
			     SP_OTP_PSW2);
		sp_reg_write(anx78xx, TX_P0, SP_OTP_KEY_PROTECT3_REG,
			     SP_OTP_PSW3);
		sp_reg_set_bits(anx78xx, TX_P0, SP_HDCP_KEY_COMMAND_REG,
				SP_DISABLE_SYNC_HDCP);
	}

	sp_reg_write(anx78xx, TX_P2, SP_VID_CTRL8_REG, SP_VID_VRES_TH);

	/*
	 * DP HDCP auto authentication wait timer (when downstream starts to
	 * auth, DP side will wait for this period then do auth automatically)
	 */
	sp_reg_write(anx78xx, TX_P0, SP_HDCP_AUTO_TIMER_REG, 0x00);

	sp_reg_set_bits(anx78xx, TX_P0, SP_DP_HDCP_CTRL_REG, SP_LINK_POLLING);

	sp_reg_set_bits(anx78xx, TX_P0, SP_DP_LINK_DEBUG_CTRL_REG,
			SP_M_VID_DEBUG);
	sp_reg_set_bits(anx78xx, TX_P2, SP_ANALOG_DEBUG2_REG,
			SP_POWERON_TIME_1P5MS);

	sp_xtal_clk_sel(anx78xx);
	sp_reg_write(anx78xx, TX_P0, SP_AUX_DEFER_CTRL_REG,
		     SP_DEFER_CTRL_EN | 0x0c);

	sp_reg_set_bits(anx78xx, TX_P0, SP_DP_POLLING_CTRL_REG,
			SP_AUTO_POLLING_DISABLE);
	/*
	 * Short the link integrity check timer to speed up bstatus
	 * polling for HDCP CTS item 1A-07
	 */
	sp_reg_write(anx78xx, TX_P0, SP_HDCP_LINK_CHECK_TIMER_REG, 0x1d);
	sp_reg_set_bits(anx78xx, TX_P0, SP_DP_MISC_CTRL_REG,
			SP_EQ_TRAINING_LOOP);

	/* power down the main link by default */
	sp_reg_set_bits(anx78xx, TX_P0, SP_DP_ANALOG_POWER_DOWN_REG,
			SP_CH0_PD);

	sp_reg_write(anx78xx, TX_P2, SP_INT_CTRL_REG, 0x01);

	sp_link_phy_initialization(anx78xx);
	sp_gen_m_clk_with_downspreading(anx78xx);

	sp.down_sample_en = false;
}

/*
 * Check if it is ANALOGIX dongle.
 */
static const u8 anx_oui[3] = {0x00, 0x22, 0xb9};

static bool is_anx_dongle(struct anx78xx *anx78xx)
{
	u8 buf[3];

	sp_dp_read_bytes_from_dpcd(anx78xx, DP_SINK_OUI, 3, buf);
	if (!memcmp(buf, anx_oui, 3))
		return true;

	/* 0x0500~0x0502: BRANCH_IEEE_OUI */
	sp_dp_read_bytes_from_dpcd(anx78xx, DP_BRANCH_OUI, 3, buf);
	if (!memcmp(buf, anx_oui, 3))
		return true;

	return false;
}

static const u8 anx7750[4] = {0x37, 0x37, 0x35, 0x30};

static u8 sp_dp_get_max_rx_bandwidth(struct anx78xx *anx78xx)
{
	u8 bandwidth, max_link_rate;
	u8 buf[4];

	bandwidth = 0;
	/*
	 * When ANX dongle is connected, if CHIP_ID=0x7750 the bandwidth is
	 * 6.75G because ANX7750 DPCD 0x052x is not available.
	 */
	if (is_anx_dongle(anx78xx)) {
		/*
		 * 00503h - 005FFh: RESERVED for Branch Device vendor-specific
		 * usage
		 */
		sp_dp_read_bytes_from_dpcd(anx78xx, 0x503, 4, buf);
		if (!memcmp(buf, anx7750, sizeof(anx7750)))
			bandwidth = DP_LINK_BW_6_75;
		else
			sp_dp_read_bytes_from_dpcd(anx78xx, 0x521,
						   1, &bandwidth);
	}

	sp_dp_read_bytes_from_dpcd(anx78xx, DP_MAX_LINK_RATE, 1,
				   &max_link_rate);
	if (bandwidth < max_link_rate)
		bandwidth = max_link_rate;

	return bandwidth;
}

static bool sp_get_dp_connection(struct anx78xx *anx78xx)
{
	u8 val;

	if (sp_dp_read_bytes_from_dpcd(anx78xx, DP_SINK_COUNT, 1, &val))
		return false;

	/* FIXME: Shouldn't be 0x3f SINK_COUNT[5:0] */
	if (!(val & 0x1f))
		return false;

	if (sp_dp_read_bytes_from_dpcd(anx78xx, DP_NORP, 1, &val))
		return false;

	if (val & 0x20) {
		sp_dp_read_bytes_from_dpcd(anx78xx, DP_SET_POWER, 1, &val);
		/*
		 * Bit 5 = SET_DN_DEVICE_DP_PWR_5V
		 * Bit 6 = SET_DN_DEVICE_DP_PWR_12V
		 * Bit 7 = SET_DN_DEVICE_DP_PWR_18V
		 */
		val &= 0x1f;
		val |= 0x20;
		sp_dp_write_bytes_to_dpcd(anx78xx, DP_SET_POWER, 1, &val);
	}

	return true;
}

static void sp_enable_video_input(struct anx78xx *anx78xx, bool enable)
{
	struct device *dev = &anx78xx->client->dev;
	u8 val;

	sp_reg_read(anx78xx, TX_P2, SP_VID_CTRL1_REG, &val);
	if (enable) {
		sp_reg_set_bits(anx78xx, TX_P2, SP_VID_CTRL1_REG, SP_VIDEO_EN);
		dev_dbg(dev, "Slimport video is enabled!\n");
	} else {
		sp_reg_clear_bits(anx78xx, TX_P2, SP_VID_CTRL1_REG,
				  SP_VIDEO_EN);
		dev_dbg(dev, "Slimport video is disabled!\n");
	}
}

static u8 sp_get_edid_bandwidth(u8 *data)
{
	u16 pclk;

	pclk = ((u16)data[1] << 8) | ((u16)data[0] & 0xff);
	if (pclk <= 5300)
		return DP_LINK_BW_1_62;
	else if (pclk <= 8900)
		return DP_LINK_BW_2_7;
	else if (pclk <= 18000)
		return DP_LINK_BW_5_4;
	else
		return DP_LINK_BW_6_75;
}

static u8 sp_parse_edid_to_get_bandwidth(struct anx78xx *anx78xx)
{
	int i;
	u8 bandwidth, temp;

	bandwidth = DP_LINK_BW_1_62;
	for (i = 0; i < 4; i++) {
		if (sp.edid_blocks[0x36 + 0x12 * i] == 0)
			break;
		temp = sp_get_edid_bandwidth(sp.edid_blocks + 0x36 + 0x12 * i);
		dev_dbg(&anx78xx->client->dev, "bandwidth via EDID : %x\n",
			temp);
		if (bandwidth < temp)
			bandwidth = temp;
		if (bandwidth >= DP_LINK_BW_6_75)
			break;
	}

	return bandwidth;
}

u8 sp_get_link_bandwidth_from_edid(struct anx78xx *anx78xx)
{
	u8 bandwidth, max_bandwidth;

	bandwidth = sp_dp_get_max_rx_bandwidth(anx78xx);
	max_bandwidth = sp_parse_edid_to_get_bandwidth(anx78xx);
	if (bandwidth > max_bandwidth)
		return max_bandwidth;

	return bandwidth;
}

static int sp_tx_aux_wr(struct anx78xx *anx78xx, u8 offset)
{
	sp_reg_write(anx78xx, TX_P0, SP_DP_BUF_DATA0_REG, offset);
	sp_reg_write(anx78xx, TX_P0, SP_DP_AUX_CH_CTRL1_REG, 0x04);
	sp_reg_set_bits(anx78xx, TX_P0, SP_DP_AUX_CH_CTRL2_REG, SP_AUX_EN);

	return sp_wait_aux_op_finish(anx78xx);
}

static int sp_tx_aux_rd(struct anx78xx *anx78xx, u8 len)
{
	sp_reg_write(anx78xx, TX_P0, SP_DP_AUX_CH_CTRL1_REG, len);
	sp_reg_set_bits(anx78xx, TX_P0, SP_DP_AUX_CH_CTRL2_REG, SP_AUX_EN);

	return sp_wait_aux_op_finish(anx78xx);
}

static int sp_edid_read(struct anx78xx *anx78xx, u8 offset,
			u8 *buf)
{
	u8 data_cnt, errcnt;
	u8 val, ret, i;

	sp_tx_aux_wr(anx78xx, offset);
	/* read 16 bytes (MOT = 1) */
	sp_tx_aux_rd(anx78xx, 0xf0 | DP_AUX_I2C_MOT | DP_AUX_I2C_READ);
	data_cnt = 0;
	errcnt = 0;

	while (data_cnt < 16) {
		sp_reg_read(anx78xx, TX_P0, SP_BUF_DATA_COUNT_REG, &val);
		val &= SP_BUF_DATA_COUNT_MASK;
		if (val) {
			for (i = 0; i < val; i++)
				sp_reg_read(anx78xx, TX_P0,
					    SP_DP_BUF_DATA0_REG + i,
					    &buf[data_cnt + i]);
			data_cnt += val;
		} else {
			if (errcnt++ <= 2)
				sp_reset_aux(anx78xx);
			else
				return -ETIMEDOUT;
		}
		/* read 16 - data_cnt bytes (MOT = 1) */
		val = DP_AUX_I2C_MOT | DP_AUX_I2C_READ |
		      ((0x0f - data_cnt) << 4);
		sp_tx_aux_rd(anx78xx, val);
	}
	sp_reg_write(anx78xx, TX_P0, SP_DP_AUX_CH_CTRL1_REG, 0x01);
	sp_reg_set_bits(anx78xx, TX_P0, SP_DP_AUX_CH_CTRL2_REG,
			SP_ADDR_ONLY | SP_AUX_EN);
	ret = sp_wait_aux_op_finish(anx78xx);
	sp_addronly_set(anx78xx, false);

	return ret;
}

static void sp_tx_edid_read_initial(struct anx78xx *anx78xx)
{
	/* Write AUX CH address 0x00050 */
	sp_reg_write(anx78xx, TX_P0, SP_AUX_ADDR_7_0_REG, SP_I2C_EXTRA_ADDR);
	sp_reg_write(anx78xx, TX_P0, SP_AUX_ADDR_15_8_REG, 0);
	sp_reg_clear_bits(anx78xx, TX_P0, SP_AUX_ADDR_19_16_REG,
			  SP_AUX_ADDR_19_16_MASK);
}

static int sp_seg_edid_read(struct anx78xx *anx78xx,
			    u8 segment, u8 offset)
{
	struct device *dev = &anx78xx->client->dev;
	u8 val, retry_cnt;
	int i;

	sp_reg_write(anx78xx, TX_P0, SP_DP_AUX_CH_CTRL1_REG, 0x04);
	sp_reg_write(anx78xx, TX_P0, SP_AUX_ADDR_7_0_REG, 0x30);

	sp_reg_set_bits(anx78xx, TX_P0, SP_DP_AUX_CH_CTRL2_REG,
			SP_ADDR_ONLY | SP_AUX_EN);

	if (sp_wait_aux_op_finish(anx78xx))
		return -ETIMEDOUT;

	sp_reg_write(anx78xx, TX_P0, SP_DP_BUF_DATA0_REG, segment);
	sp_reg_write(anx78xx, TX_P0, SP_DP_AUX_CH_CTRL1_REG, 0x04);

	sp_reg_update_bits(anx78xx, TX_P0, SP_DP_AUX_CH_CTRL2_REG,
			   SP_ADDR_ONLY | SP_AUX_EN, SP_AUX_EN);

	retry_cnt = 10;
	while (--retry_cnt) {
		sp_reg_read(anx78xx, TX_P0, SP_DP_AUX_CH_CTRL2_REG, &val);
		if (!(val & SP_AUX_EN))
			break;
		usleep_range(1000, 2000);
	}

	if (retry_cnt == 0) {
		dev_err(dev,
			"failed to read DP AUX Channel Control Register 2\n");
		sp_reset_aux(anx78xx);
		return -ETIMEDOUT;
	}

	sp_reg_write(anx78xx, TX_P0, SP_AUX_ADDR_7_0_REG, SP_I2C_EXTRA_ADDR);
	sp_tx_aux_wr(anx78xx, offset);
	/* read 16 bytes (MOT = 1) */
	sp_tx_aux_rd(anx78xx, 0xf0 | DP_AUX_I2C_MOT | DP_AUX_I2C_READ);

	for (i = 0; i < 16; i++) {
		retry_cnt = 10;
		while (--retry_cnt) {
			sp_reg_read(anx78xx, TX_P0, SP_BUF_DATA_COUNT_REG,
				    &val);
			if (val & SP_BUF_DATA_COUNT_MASK)
				break;
			usleep_range(2000, 4000);
		}

		if (retry_cnt == 0) {
			dev_err(dev,
				"failed to read DP Buffer Data Count Register\n");
			sp_reset_aux(anx78xx);
			return -ETIMEDOUT;
		}

		sp_reg_read(anx78xx, TX_P0, SP_DP_BUF_DATA0_REG + i, &val);
	}

	sp_reg_write(anx78xx, TX_P0, SP_DP_AUX_CH_CTRL1_REG, 0x01);
	sp_reg_set_bits(anx78xx, TX_P0, SP_DP_AUX_CH_CTRL2_REG,
			SP_ADDR_ONLY | SP_AUX_EN);
	sp_reg_clear_bits(anx78xx, TX_P0, SP_DP_AUX_CH_CTRL2_REG,
			  SP_ADDR_ONLY);
	sp_reg_read(anx78xx, TX_P0, SP_DP_AUX_CH_CTRL2_REG, &val);

	retry_cnt = 10;
	while (--retry_cnt) {
		sp_reg_read(anx78xx, TX_P0, SP_DP_AUX_CH_CTRL2_REG, &val);
		if (!(val & SP_AUX_EN))
			break;
		usleep_range(1000, 2000);
	}

	if (retry_cnt) {
		dev_err(dev,
			"failed to read DP AUX Channel Control Register 2\n");
		sp_reset_aux(anx78xx);
		return -ETIMEDOUT;
	}

	return 0;
}

static int sp_edid_block_checksum(const u8 *raw_edid)
{
	int i;
	u8 csum = 0;

	for (i = 0; i < EDID_LENGTH; i++)
		csum += raw_edid[i];

	return csum;
}

static int sp_tx_edid_read(struct anx78xx *anx78xx)
{
	struct device *dev = &anx78xx->client->dev;
	u8 val, last_block, offset = 0;
	u8 buf[16];
	int i, j, count;

	sp_tx_edid_read_initial(anx78xx);
	sp_reg_write(anx78xx, TX_P0, SP_DP_AUX_CH_CTRL1_REG, 0x04);
	sp_reg_set_bits(anx78xx, TX_P0, SP_DP_AUX_CH_CTRL2_REG,
			SP_AUX_EN | SP_ADDR_ONLY);

	if (sp_wait_aux_op_finish(anx78xx))
		return -ETIMEDOUT;

	sp_addronly_set(anx78xx, false);

	/* Read the number of blocks */
	sp_tx_aux_wr(anx78xx, 0x7e);
	sp_tx_aux_rd(anx78xx, DP_AUX_I2C_READ);
	sp_reg_read(anx78xx, TX_P0, SP_DP_BUF_DATA0_REG, &last_block);
	dev_dbg(dev, "last EDID block is %d\n", last_block);

	/* FIXME: Why not just cap to 3 if the reported value is >3 */
	if (last_block > 3)
		last_block = 1;

	/* for every block */
	for (count = 0; count <= last_block; count++) {
		switch (count) {
		case 0:
		case 1:
			for (i = 0; i < 8; i++) {
				offset = (i + count * 8) * 16;
				if (sp_edid_read(anx78xx, offset, buf))
					return -EINVAL;
				for (j = 0; j < 16; j++)
					sp.edid_blocks[offset + j] = buf[j];
			}
			break;
		case 2:
		case 3:
			offset = (count == 2) ? 0x00 : 0x80;
			for (j = 0; j < 8; j++) {
				if (sp_seg_edid_read(anx78xx, count / 2,
						     offset))
					return -EINVAL;
				offset = offset + 0x10;
			}
			break;
		}
	}

	sp_reset_aux(anx78xx);

	if (!drm_edid_block_valid(sp.edid_blocks, 0, true, NULL)) {
		dev_err(dev, "EDID block is invalid\n");
		return -EINVAL;
	}

	sp_dp_read_bytes_from_dpcd(anx78xx, DP_TEST_REQUEST, 1, &val);
	if (val & DP_TEST_LINK_EDID_READ) {
		dev_dbg(dev, "EDID test requested\n");
		val = sp_edid_block_checksum(sp.edid_blocks);
		dev_dbg(dev, "EDID checksum is %d\n", val);
		sp_dp_write_bytes_to_dpcd(anx78xx, DP_TEST_EDID_CHECKSUM, 1,
					  &val);
		sp.tx_test_edid = true;
		val = DP_TEST_EDID_CHECKSUM_WRITE;
		sp_dp_write_bytes_to_dpcd(anx78xx, DP_TEST_RESPONSE, 1, &val);
	}

	return 0;
}

static bool sp_check_with_pre_edid(struct anx78xx *anx78xx)
{
	struct device *dev = &anx78xx->client->dev;
	u8 i;
	u8 buf[16];
	bool ret = false;

	sp_tx_edid_read_initial(anx78xx);
	sp_reg_write(anx78xx, TX_P0, SP_DP_AUX_CH_CTRL1_REG, 0x04);
	sp_reg_set_bits(anx78xx, TX_P0, SP_DP_AUX_CH_CTRL2_REG, 0x03);

	if (sp_wait_aux_op_finish(anx78xx))
		goto return_point;

	sp_addronly_set(anx78xx, false);

	if (sp_edid_read(anx78xx, 0x70, buf))
		goto return_point;

	if (memcmp(&sp.edid_blocks[0x70], buf, 16) != 0) {
		dev_dbg(dev, "different checksum and blocks num\n");
		goto return_point;
	}

	if (sp_edid_read(anx78xx, 0x08, buf))
		goto return_point;

	for (i = 0; i < 16; i++) {
		if (sp.edid_blocks[i + 8] != buf[i]) {
			dev_dbg(dev, "different edid information\n");
			goto return_point;
		}
	}

	ret = true;
return_point:
	sp_reset_aux(anx78xx);

	return ret;
}

static bool sp_edid_process(struct anx78xx *anx78xx)
{
	struct device *dev = &anx78xx->client->dev;
	u8 bw, edid_bw, val;
	int i;

	if (sp.read_edid_flag) {
		if (!sp_check_with_pre_edid(anx78xx))
			sp.read_edid_flag = false;
	} else {
		if (sp_tx_edid_read(anx78xx)) {
			dev_err(dev, "EDID corruption!\n");
			return false;
		}
	}

	/* Release the HPD after the OTP loaddown */
	for (i = 0; i < 10; i++) {
		sp_reg_read(anx78xx, TX_P0, SP_HDCP_KEY_STATUS_REG, &val);
		if (val & 0x01)
			break;

		dev_dbg(dev, "waiting HDCP KEY loaddown\n");
		usleep_range(1000, 2000);
	}

	sp_reg_write(anx78xx, RX_P0, SP_INT_MASK_BASE + 1,
		     SP_HDMI_DVI | SP_CKDT_CHG | SP_SCDT_CHG |
		     SP_CABLE_PLUG_CHG);

	if (!sp_hdcp_repeater_mode(anx78xx)) {
		sp_hdmi_set_hpd(anx78xx, true);
		sp_hdmi_set_termination(anx78xx, true);
	}

	bw = sp_dp_get_max_rx_bandwidth(anx78xx);
	dev_dbg(dev, "RX BW %x\n", bw);

	edid_bw = sp_parse_edid_to_get_bandwidth(anx78xx);
	if (bw <= edid_bw)
		edid_bw = bw;

	dev_dbg(dev, "set link bw in edid %x\n", edid_bw);
	sp.changed_bandwidth = edid_bw;

	return true;
}

static void sp_lvttl_bit_mapping(struct anx78xx *anx78xx)
{
	struct device *dev = &anx78xx->client->dev;
	u8 val;
	u8 vid_bit;

	sp_reg_read(anx78xx, RX_P0, SP_VIDEO_STATUS_REG, &val);
	switch ((val & SP_COLOR_DEPTH_MASK) >> SP_COLOR_DEPTH_SHIFT) {
	default:
	case SP_COLOR_DEPTH_MODE_LEGACY:
		val = SP_IN_BPC_8BIT;
		vid_bit = 0;
		break;
	case SP_COLOR_DEPTH_MODE_24BIT:
		val = SP_IN_BPC_8BIT;
		if (sp.frame.avi.colorspace == HDMI_COLORSPACE_YUV422)
			vid_bit = 5;
		else
			vid_bit = 1;
		break;
	case SP_COLOR_DEPTH_MODE_30BIT:
		val = SP_IN_BPC_10BIT;
		if (sp.frame.avi.colorspace == HDMI_COLORSPACE_YUV422)
			vid_bit = 6;
		else
			vid_bit = 2;
		/*
		 * For 10bit video must be set this value to 12bit by
		 * someone
		 */
		if (sp.down_sample_en)
			vid_bit = 3;
		break;
	case SP_COLOR_DEPTH_MODE_36BIT:
		val = SP_IN_BPC_12BIT;
		if (sp.frame.avi.colorspace == HDMI_COLORSPACE_YUV422)
			vid_bit = 6;
		else
			vid_bit = 3;
		break;
	}

	/*
	 * For down sample video (12bit, 10bit ---> 8bit),
	 * this register doesn't change
	 */
	if (!sp.down_sample_en)
		sp_reg_update_bits(anx78xx, TX_P2, SP_VID_CTRL2_REG,
				   SP_IN_BPC_MASK | SP_IN_COLOR_F_MASK,
				   (val << SP_IN_BPC_SHIFT) |
				   sp.frame.avi.colorspace);

	sp_reg_write(anx78xx, TX_P2, SP_BIT_CTRL_SPECIFIC_REG,
		     SP_ENABLE_BIT_CTRL | vid_bit << SP_BIT_CTRL_SELECT_SHIFT);

	if (sp.tx_test_edid) {
		/* Set color depth to 6 bpc (18 bpp) for link cts */
		sp_reg_update_bits(anx78xx, TX_P2, SP_VID_CTRL2_REG,
				   SP_IN_BPC_MASK, SP_IN_BPC_6BIT);
		sp.tx_test_edid = false;
		dev_dbg(dev, "color space is set to 6 bpc (18 bpp)\n");
	}

	if (sp.frame.avi.colorspace) {
		/*
		 * Set video values to default of channel 0, 1 and 2 for HDCP
		 * embedded "blue screen" when HDCP authentication failed.
		 */
		sp_reg_write(anx78xx, TX_P0, SP_HDCP_VID0_BLUE_SCREEN_REG,
			     0x80);
		sp_reg_write(anx78xx, TX_P0, SP_HDCP_VID1_BLUE_SCREEN_REG,
			     0x00);
		sp_reg_write(anx78xx, TX_P0, SP_HDCP_VID2_BLUE_SCREEN_REG,
			     0x80);
	} else {
		sp_reg_write(anx78xx, TX_P0, SP_HDCP_VID0_BLUE_SCREEN_REG,
			     0x00);
		sp_reg_write(anx78xx, TX_P0, SP_HDCP_VID0_BLUE_SCREEN_REG,
			     0x00);
		sp_reg_write(anx78xx, TX_P0, SP_HDCP_VID0_BLUE_SCREEN_REG,
			     0x00);
	}
}

static unsigned long sp_pclk_calc(struct anx78xx *anx78xx)
{
	struct device *dev = &anx78xx->client->dev;
	unsigned long str_plck;
	u16 vid_counter;
	u8 val;

	sp_reg_read(anx78xx, RX_P0, SP_PCLK_HIGHRES_CNT_BASE + 2, &val);
	vid_counter = val << 8;
	sp_reg_read(anx78xx, RX_P0, SP_PCLK_HIGHRES_CNT_BASE + 1, &val);
	vid_counter |= val;
	str_plck = (vid_counter * XTAL_CLK) >> 12;
	dev_dbg(dev, "pixel clock is %ld.%ld\n", str_plck / 10, str_plck % 10);
	return str_plck;
}

static int sp_tx_bw_lc_sel(struct anx78xx *anx78xx)
{
	struct device *dev = &anx78xx->client->dev;
	unsigned long pixel_clk;
	u8 link, val;

	pixel_clk = sp_pclk_calc(anx78xx);

	sp_reg_read(anx78xx, RX_P0, SP_VIDEO_STATUS_REG, &val);
	switch ((val & SP_COLOR_DEPTH_MASK) >> SP_COLOR_DEPTH_SHIFT) {
	case SP_COLOR_DEPTH_MODE_LEGACY:
	case SP_COLOR_DEPTH_MODE_24BIT:
	default:
		break;
	case SP_COLOR_DEPTH_MODE_30BIT:
		pixel_clk = (pixel_clk * 5) >> 2;
		break;
	case SP_COLOR_DEPTH_MODE_36BIT:
		pixel_clk = (pixel_clk * 3) >> 1;
		break;
	}

	sp.down_sample_en = false;
	if (pixel_clk <= 530) {
		link = DP_LINK_BW_1_62;
	} else if (pixel_clk <= 890) {
		link = DP_LINK_BW_2_7;
	} else if (pixel_clk <= 1800) {
		link = DP_LINK_BW_5_4;
	} else {
		link = DP_LINK_BW_6_75;
		if (pixel_clk > 2240)
			sp.down_sample_en = true;
	}

	if (sp_get_link_bandwidth(anx78xx) != link) {
		sp.changed_bandwidth = link;
		dev_err(dev,
			"different bandwidth between sink and video %.2x",
			link);
		return -EINVAL;
	}
	return 0;
}

static void sp_downspreading_enable(struct anx78xx *anx78xx, bool enable)
{
	u8 val;

	sp_reg_read(anx78xx, TX_P0, SP_DP_DOWNSPREADING_CTRL1_REG, &val);

	if (enable) {
		val |= SP_TX_SSC_DOWNSPREADING;
		sp_reg_write(anx78xx, TX_P0, SP_DP_DOWNSPREADING_CTRL1_REG,
			     val);

		sp_dp_read_bytes_from_dpcd(anx78xx, DP_DOWNSPREAD_CTRL, 1,
					   &val);
		val |= DP_SPREAD_AMP_0_5;
		sp_dp_write_bytes_to_dpcd(anx78xx, DP_DOWNSPREAD_CTRL, 1, &val);
	} else {
		val &= ~SP_TX_SSC_DISABLE;
		sp_reg_write(anx78xx, TX_P0, SP_DP_DOWNSPREADING_CTRL1_REG,
			     val);

		sp_dp_read_bytes_from_dpcd(anx78xx, DP_DOWNSPREAD_CTRL, 1,
					   &val);
		val &= ~DP_SPREAD_AMP_0_5;
		sp_dp_write_bytes_to_dpcd(anx78xx, DP_DOWNSPREAD_CTRL, 1, &val);
	}
}

static void sp_config_ssc(struct anx78xx *anx78xx, enum sp_ssc_dep sscdep)
{
	sp_reg_write(anx78xx, TX_P0, SP_DP_DOWNSPREADING_CTRL1_REG, 0x0);
	sp_reg_write(anx78xx, TX_P0, SP_DP_DOWNSPREADING_CTRL1_REG, sscdep);
	sp_downspreading_enable(anx78xx, true);
}

void sp_dp_enable_enhanced_mode(struct anx78xx *anx78xx, bool enable)
{
	if (enable)
		sp_reg_set_bits(anx78xx, TX_P0, SP_DP_SYSTEM_CTRL_BASE + 4,
				SP_ENHANCED_MODE);
	else
		sp_reg_clear_bits(anx78xx, TX_P0, SP_DP_SYSTEM_CTRL_BASE + 4,
				  SP_ENHANCED_MODE);
}

static void sp_dp_enable_rx_to_enhanced_mode(struct anx78xx *anx78xx,
					     bool enable)
{
	struct device *dev = &anx78xx->client->dev;
	u8 val;

	if (enable) {
		sp_dp_read_bytes_from_dpcd(anx78xx, DP_LANE_COUNT_SET, 1, &val);
		val |= DP_ENHANCED_FRAME_CAP;
		sp_dp_write_bytes_to_dpcd(anx78xx, DP_LANE_COUNT_SET, 1, &val);
		dev_dbg(dev, "rx to enhanced mode enabled\n");
	} else {
		sp_dp_read_bytes_from_dpcd(anx78xx, DP_LANE_COUNT_SET, 1, &val);
		val &= ~DP_ENHANCED_FRAME_CAP;
		sp_dp_write_bytes_to_dpcd(anx78xx, DP_LANE_COUNT_SET, 1, &val);
		dev_dbg(dev, "rx to enhanced mode disabled\n");
	}
}

static bool sp_dp_is_enhanced_mode_available(struct anx78xx *anx78xx)
{
	u8 val;

	sp_dp_read_bytes_from_dpcd(anx78xx, DP_MAX_LANE_COUNT, 1, &val);

	return val & DP_ENHANCED_FRAME_CAP;
}

static void sp_dp_set_enhanced_mode(struct anx78xx *anx78xx)
{
	u8 val;

	val = sp_dp_is_enhanced_mode_available(anx78xx);
	sp_dp_enable_rx_to_enhanced_mode(anx78xx, val);
	sp_dp_enable_enhanced_mode(anx78xx, val);
}

static u16 sp_link_err_check(struct anx78xx *anx78xx)
{
	struct device *dev = &anx78xx->client->dev;
	u16 err;
	u8 buf[2];

	sp_dp_read_bytes_from_dpcd(anx78xx, DP_SYMBOL_ERROR_COUNT_LANE0, 2,
				   buf);
	usleep_range(5000, 10000);
	sp_dp_read_bytes_from_dpcd(anx78xx, DP_SYMBOL_ERROR_COUNT_LANE0, 2,
				   buf);

	if (buf[1] & DP_ERROR_COUNT_VALID) {
		err = ((buf[1] & DP_ERROR_COUNT_BITS_14_8_MASK) << 8) + buf[0];
		dev_err(dev, "error of Lane %d\n", err);
		return err;
	}

	return 0;
}

static bool sp_lt_finish(struct anx78xx *anx78xx)
{
	u8 val;
	struct device *dev = &anx78xx->client->dev;

	sp_dp_read_bytes_from_dpcd(anx78xx, DP_LANE0_1_STATUS, 1, &val);

	val &= DP_LANE_CR_DONE | DP_LANE_CHANNEL_EQ_DONE |
	       DP_LANE_SYMBOL_LOCKED;
	if (val != (DP_LANE_CR_DONE | DP_LANE_CHANNEL_EQ_DONE |
	    DP_LANE_SYMBOL_LOCKED)) {
		dev_dbg(dev, "Lane0 status error %.2x\n", val);
		sp.tx_lt_state = LT_ERROR;
		return false;
	}

	/*
	 * If there is link error, adjust pre-emphasis to check error
	 * again. If there is no error, keep the setting, otherwise
	 * use 400mv0db
	 */
	if (sp.tx_test_lt) {
		sp.tx_test_lt = false;
		sp.tx_lt_state = LT_INIT;
		return true;
	}

	if (sp_link_err_check(anx78xx)) {
		sp_reg_read(anx78xx, TX_P0, SP_DP_LANE0_LT_CTRL_REG, &val);
		if (!(val & SP_MAX_PRE_REACH)) {
			/* Increase one pre-level */
			sp_reg_write(anx78xx, TX_P0, SP_DP_LANE0_LT_CTRL_REG,
				     val + 0x08);
			/*
			 * If error still exists, return to the link training
			 * value
			 */
			if (sp_link_err_check(anx78xx))
				sp_reg_write(anx78xx, TX_P0,
					     SP_DP_LANE0_LT_CTRL_REG, val);
		}
	}

	val = sp_get_link_bandwidth(anx78xx);
	if (val != sp.changed_bandwidth) {
		dev_dbg(dev, "bandwidth changed, cur:%.2x, per:%.2x\n", val,
			sp.changed_bandwidth);
		sp.tx_lt_state = LT_ERROR;
		return false;
	}

	dev_dbg(dev, "LT succeed, bandwidth: %.2x", val);
	sp_reg_read(anx78xx, TX_P0, SP_DP_LANE0_LT_CTRL_REG, &val);
	dev_dbg(dev, "Lane0 set to %.2x\n", val);
	sp.tx_lt_state = LT_INIT;

	if (sp_hdcp_repeater_mode(anx78xx)) {
		dev_dbg(dev, "HPD set to 1!\n");
		sp_hdmi_set_hpd(anx78xx, true);
		sp_hdmi_set_termination(anx78xx, true);
	}

	/*
	 * Under low voltage (DVD10 = 0.97V), some chips cannot output video,
	 * link down interrupt always happens.
	 */
	if (sp_link_err_check(anx78xx) > 200) {
		dev_dbg(dev, "need to reset Serdes FIFO");
		sp.tx_lt_state = LT_ERROR;
	} else {
		return true;
	}

	return false;
}

static bool sp_link_training(struct anx78xx *anx78xx)
{
	struct device *dev = &anx78xx->client->dev;
	u8 val, version;

	switch (sp.tx_lt_state) {
	case LT_INIT:
		sp_block_power_ctrl(anx78xx, SP_TX_PWR_VIDEO, 1);
		sp_video_mute(anx78xx, true);
		sp_enable_video_input(anx78xx, false);
		sp.tx_lt_state = LT_WAIT_PLL_LOCK;
	/* fallthrough */
	case LT_WAIT_PLL_LOCK:
		if (!sp_get_pll_lock_status(anx78xx)) {
			sp_reg_read(anx78xx, TX_P0, SP_DP_PLL_CTRL_REG,
				    &val);

			val |= SP_PLL_RST;
			sp_reg_write(anx78xx, TX_P0, SP_DP_PLL_CTRL_REG,
				     val);

			val &= ~SP_PLL_RST;
			sp_reg_write(anx78xx, TX_P0, SP_DP_PLL_CTRL_REG,
				     val);

			dev_dbg(dev, "PLL not lock!\n");
			break;
		} else {
			sp.tx_lt_state = LT_CHECK_LINK_BW;
		}
	/* fallthrough */
	case LT_CHECK_LINK_BW:
		val = sp_dp_get_max_rx_bandwidth(anx78xx);
		if (val < sp.changed_bandwidth) {
			dev_dbg(dev, "over bandwidth!\n");
			sp.changed_bandwidth = val;
			break;
		} else {
			sp.tx_lt_state = LT_START;
		}
	/* fallthrough */
	case LT_START:
		if (sp.tx_test_lt) {
			sp.changed_bandwidth = sp.tx_test_bw;
			sp_reg_clear_bits(anx78xx, TX_P2, SP_VID_CTRL2_REG,
					  0x70);
		} else {
			sp_reg_write(anx78xx, TX_P0, SP_DP_LANE0_LT_CTRL_REG,
				     0x00);
		}

		sp_reg_clear_bits(anx78xx, TX_P0, SP_DP_ANALOG_POWER_DOWN_REG,
				  SP_CH0_PD);

		sp_config_ssc(anx78xx, SSC_DEP_4000PPM);
		sp_set_link_bandwidth(anx78xx, sp.changed_bandwidth);
		sp_dp_set_enhanced_mode(anx78xx);

		sp_dp_read_bytes_from_dpcd(anx78xx, DP_DPCD_REV, 1, &version);
		sp_dp_read_bytes_from_dpcd(anx78xx, DP_SET_POWER, 1, &val);
		if (version >= 0x12)
			val &= ~DP_SET_POWER_12_MASK;
		else
			val &= ~DP_SET_POWER_MASK;
		val |= DP_SET_POWER_D0;
		sp_dp_write_bytes_to_dpcd(anx78xx, DP_SET_POWER, 1, &val);

		sp_reg_write(anx78xx, TX_P0, SP_DP_LT_CTRL_REG, SP_LT_EN);
		sp.tx_lt_state = LT_WAITING_FINISH;
	/* fallthrough */
	case LT_WAITING_FINISH:
		/* Waiting interrupt to change training state. */
		break;
	case LT_ERROR:
		sp_reg_set_bits(anx78xx, TX_P2, SP_RESET_CTRL2_REG,
				SP_SERDES_FIFO_RST);
		msleep(20);
		sp_reg_clear_bits(anx78xx, TX_P2, SP_RESET_CTRL2_REG,
				  SP_SERDES_FIFO_RST);
		dev_err(dev, "LT ERROR reset SERDES FIFO");
		sp.tx_lt_state = LT_INIT;
		break;
	case LT_FINISH:
		if (sp_lt_finish(anx78xx))
			return true;
		break;
	default:
		break;
	}

	return false;
}

static bool sp_match_vic_for_bt709(u8 vic)
{
	/* Video Identification Code (VIC) for BT709 */
	return ((vic == 0x04) || (vic == 0x05) || (vic == 0x10) ||
		(vic == 0x13) || (vic == 0x14) || (vic == 0x1f) ||
		(vic == 0x20) || (vic == 0x21) || (vic == 0x22) ||
		(vic == 0x27) || (vic == 0x28) || (vic == 0x29) ||
		(vic == 0x2e) || (vic == 0x2f) || (vic == 0x3c) ||
		(vic == 0x3d) || (vic == 0x3e) || (vic == 0x3f) ||
		(vic == 0x40));
}

static void sp_set_colorspace(struct anx78xx *anx78xx)
{
	struct device *dev = &anx78xx->client->dev;
	u8 colorspace, val;

	if (sp.down_sample_en) {
		if (sp.frame.avi.colorspace == HDMI_COLORSPACE_YUV422) {
			dev_dbg(dev, "YCbCr4:2:2 ---> PASS THROUGH.\n");
			sp_reg_write(anx78xx, TX_P2, SP_VID_CTRL6_REG, 0x00);
			sp_reg_write(anx78xx, TX_P2, SP_VID_CTRL5_REG, 0x00);
		} else if (sp.frame.avi.colorspace == HDMI_COLORSPACE_YUV444) {
			dev_dbg(dev, "YCbCr4:4:4 ---> YCbCr4:2:2\n");
			sp_reg_write(anx78xx, TX_P2, SP_VID_CTRL6_REG,
				     SP_VIDEO_PROCESS_EN | SP_UP_SAMPLE);
			sp_reg_write(anx78xx, TX_P2, SP_VID_CTRL5_REG, 0x00);
		} else if (sp.frame.avi.colorspace == HDMI_COLORSPACE_RGB) {
			dev_dbg(dev, "RGB4:4:4 ---> YCbCr4:2:2\n");
			sp_reg_write(anx78xx, TX_P2, SP_VID_CTRL6_REG,
				     SP_VIDEO_PROCESS_EN | SP_UP_SAMPLE);
			sp_reg_write(anx78xx, TX_P2, SP_VID_CTRL5_REG,
				     SP_CSC_STD_SEL | SP_RANGE_R2Y |
				     SP_CSPACE_R2Y);
		}
		sp_reg_write(anx78xx, TX_P2, SP_VID_CTRL2_REG,
			     (SP_IN_BPC_8BIT << SP_IN_BPC_SHIFT) | colorspace);
	} else {
		sp_reg_read(anx78xx, TX_P2, SP_VID_CTRL2_REG, &colorspace);
		colorspace &= SP_IN_COLOR_F_MASK;

		/*
		 * To change the CSC_STD_SEL bit we need to set
		 * CSPACE_Y2R and CSPACE_ R2Y, otherwise has no
		 * effect or is undetermined.
		 */
		if (colorspace == HDMI_COLORSPACE_RGB) {
			sp_reg_clear_bits(anx78xx, TX_P2, SP_VID_CTRL5_REG,
					  SP_RANGE_Y2R | SP_CSPACE_Y2R |
					  SP_CSC_STD_SEL);
			sp_reg_clear_bits(anx78xx, TX_P2, SP_VID_CTRL6_REG,
					  SP_VIDEO_PROCESS_EN | SP_UP_SAMPLE);
		} else {
			/*
			 * Colorimetric format of input video is YCbCr422
			 * or YCbCr444
			 */
			sp_reg_set_bits(anx78xx, TX_P2, SP_VID_CTRL5_REG,
					SP_RANGE_Y2R | SP_CSPACE_Y2R);

			sp_reg_read(anx78xx, RX_P1,
				    SP_AVI_INFOFRAME_DATA_BASE + 3,
				    &val);

			if (sp_match_vic_for_bt709(val))
				sp_reg_set_bits(anx78xx, TX_P2,
						SP_VID_CTRL5_REG,
						SP_CSC_STD_SEL);
			else	/* Convert based on BT601 */
				sp_reg_clear_bits(anx78xx, TX_P2,
						  SP_VID_CTRL5_REG,
						  SP_CSC_STD_SEL);
			/*
			 * Enable 4:2:2 to 4:4:4 up sample when is required
			 * and enable video process function.
			 */
			if (colorspace == HDMI_COLORSPACE_YUV422)
				sp_reg_set_bits(anx78xx, TX_P2,
						SP_VID_CTRL6_REG,
						SP_VIDEO_PROCESS_EN |
						SP_UP_SAMPLE);
			else	/* YCBCR444 */
				sp_reg_update_bits(anx78xx, TX_P2,
						   SP_VID_CTRL6_REG,
						   SP_VIDEO_PROCESS_EN |
						   SP_UP_SAMPLE,
						   SP_VIDEO_PROCESS_EN);
		}
	}
}

static u8 sp_hdmi_infoframe_checksum(u8 *ptr, int size)
{
	u8 csum = 0;
	int i;

	/* compute checksum */
	for (i = 0; i < size; i++)
		csum += ptr[i];

	return 256 - csum;
}

static void sp_audio_infoframe_init(struct anx78xx *anx78xx)
{
	int i;
	u8 buffer[HDMI_INFOFRAME_SIZE(AUDIO)];
	u8 *ptr = buffer;
	struct device *dev = &anx78xx->client->dev;

	ptr[0] = HDMI_INFOFRAME_TYPE_AUDIO;
	ptr[1] = 1;
	ptr[2] = HDMI_AUDIO_INFOFRAME_SIZE;

	/* start infoframe paylload */
	ptr += HDMI_INFOFRAME_HEADER_SIZE;
	for (i = 0; i < HDMI_AUDIO_INFOFRAME_SIZE; i++)
		sp_reg_read(anx78xx, RX_P1, SP_AUD_INFOFRAME_DATA_BASE + i,
			    &ptr[i]);

	ptr[3] = 0;
	ptr[3] = sp_hdmi_infoframe_checksum(buffer,
					    HDMI_INFOFRAME_SIZE(AUDIO));

	if (hdmi_infoframe_unpack(&sp.frame, buffer) < 0) {
		dev_err(dev, "unpack of AUDIO infoframe failed\n");
		return;
	}
}

static void sp_avi_infoframe_init(struct anx78xx *anx78xx)
{
	int i;
	u8 buffer[HDMI_INFOFRAME_SIZE(AVI)];
	u8 *ptr = buffer;
	struct device *dev = &anx78xx->client->dev;

	ptr[0] = HDMI_INFOFRAME_TYPE_AVI;
	ptr[1] = 2;
	ptr[2] = HDMI_AVI_INFOFRAME_SIZE;

	/* start infoframe payload */
	ptr += HDMI_INFOFRAME_HEADER_SIZE;
	for (i = 0; i < HDMI_AVI_INFOFRAME_SIZE; i++)
		sp_reg_read(anx78xx, RX_P1, SP_AVI_INFOFRAME_DATA_BASE + i,
			    &ptr[i]);

	ptr[3] = 0;	/* checksum */
	ptr[3] = sp_hdmi_infoframe_checksum(buffer, HDMI_INFOFRAME_SIZE(AVI));

	if (hdmi_infoframe_unpack(&sp.frame, buffer) < 0) {
		dev_err(dev, "unpack of AVI infoframe failed\n");
		return;
	}
}

static void sp_mpeg_infoframe_init(struct anx78xx *anx78xx)
{
	int i;
	u8 buffer[HDMI_INFOFRAME_SIZE(MPEG)];
	u8 *ptr = buffer;
	struct device *dev = &anx78xx->client->dev;

	ptr[0] = HDMI_INFOFRAME_TYPE_MPEG;
	ptr[1] = 1;
	ptr[2] = HDMI_MPEG_INFOFRAME_SIZE;

	/* start infoframe payload */
	ptr += HDMI_INFOFRAME_HEADER_SIZE;

	for (i = 0; i < HDMI_MPEG_INFOFRAME_SIZE; i++)
		sp_reg_read(anx78xx, RX_P1, SP_MPEG_VS_INFOFRAME_DATA_BASE + i,
			    &ptr[i]);

	ptr[3] = 0;	/* checksum */
	ptr[3] = sp_hdmi_infoframe_checksum(buffer, HDMI_INFOFRAME_SIZE(MPEG));

	if (hdmi_infoframe_unpack(&sp.frame, buffer) < 0) {
		dev_err(dev, "unpack of MPEG infoframe failed\n");
		return;
	}
}

static void sp_vsi_infoframe_init(struct anx78xx *anx78xx)
{
	int i;
	u8 len, buffer[10];
	u8 *ptr = buffer;
	struct device *dev = &anx78xx->client->dev;

	ptr[0] = HDMI_INFOFRAME_TYPE_MPEG;
	ptr[1] = 1;

	/* get infoframe length */
	sp_reg_read(anx78xx, RX_P1, SP_MPEG_VS_INFOFRAME_LEN_REG,
		    &len);
	if (len > 10)
		return;

	ptr[2] = len;

	/* start infoframe payload */
	ptr += HDMI_INFOFRAME_HEADER_SIZE;

	for (i = 0; i < len; i++)
		sp_reg_read(anx78xx, RX_P1, SP_MPEG_VS_INFOFRAME_DATA_BASE + i,
			    &ptr[i]);

	ptr[3] = 0;	/* checksum */
	ptr[3] = sp_hdmi_infoframe_checksum(buffer,
					    HDMI_INFOFRAME_HEADER_SIZE + len);

	if (hdmi_infoframe_unpack(&sp.frame, buffer) < 0) {
		dev_err(dev, "unpack of VSI infoframe failed\n");
		return;
	}
}

static void sp_load_packet(struct anx78xx *anx78xx,
			   enum hdmi_infoframe_type type)
{
	int i;
	u8 buffer[32];
	u8 *ptr = buffer;

	memset(buffer, 0, sizeof(*buffer));
	ptr += HDMI_INFOFRAME_HEADER_SIZE;

	switch (type) {
	case HDMI_INFOFRAME_TYPE_AUDIO:
		sp_reg_write(anx78xx, TX_P2, SP_INFOFRAME_AUD_TYPE_REG,
			     sp.frame.audio.type);
		sp_reg_write(anx78xx, TX_P2, SP_INFOFRAME_AUD_VER_REG,
			     sp.frame.audio.version);
		sp_reg_write(anx78xx, TX_P2, SP_INFOFRAME_AUD_LEN_REG,
			     sp.frame.audio.length);
		/* packs the audio information into a binary buffer */
		hdmi_audio_infoframe_pack(&sp.frame.audio, buffer,
					  HDMI_INFOFRAME_SIZE(AUDIO));
		/* start infoframe payload */
		for (i = 0; i < sp.frame.audio.length; i++)
			sp_reg_write(anx78xx, TX_P2,
				     SP_INFOFRAME_AUD_DB0_REG + i, ptr[i]);
		break;
	case HDMI_INFOFRAME_TYPE_AVI:
		sp_reg_write(anx78xx, TX_P2, SP_INFOFRAME_AVI_TYPE_REG,
			     sp.frame.avi.type);
		sp_reg_write(anx78xx, TX_P2, SP_INFOFRAME_AVI_VER_REG,
			     sp.frame.avi.version);
		sp_reg_write(anx78xx, TX_P2, SP_INFOFRAME_AVI_LEN_REG,
			     sp.frame.avi.length);
		/* packs the AVI information into a binary buffer */
		hdmi_avi_infoframe_pack(&sp.frame.avi, buffer,
					HDMI_INFOFRAME_SIZE(AVI));
		/* start infoframe payload */
		for (i = 0; i < sp.frame.avi.length; i++)
			sp_reg_write(anx78xx, TX_P2,
				     SP_INFOFRAME_AVI_DB0_REG + i, ptr[i]);
		break;
	case HDMI_INFOFRAME_TYPE_MPEG:
		sp_reg_write(anx78xx, TX_P2, SP_INFOFRAME_MPEG_TYPE_REG,
			     sp.frame.mpeg.type);
		sp_reg_write(anx78xx, TX_P2, SP_INFOFRAME_MPEG_VER_REG,
			     sp.frame.mpeg.version);
		sp_reg_write(anx78xx, TX_P2, SP_INFOFRAME_MPEG_LEN_REG,
			     sp.frame.mpeg.length);
		/* packs the MPEG information into a binary buffer */
		hdmi_mpeg_infoframe_pack(&sp.frame.mpeg, buffer,
					 HDMI_INFOFRAME_SIZE(MPEG));
		/* start infoframe payload */
		for (i = 0; i < sp.frame.mpeg.length; i++)
			sp_reg_write(anx78xx, TX_P2,
				     SP_INFOFRAME_MPEG_DB0_REG + i, ptr[i]);
		break;
	case HDMI_INFOFRAME_TYPE_VENDOR:
		sp_reg_write(anx78xx, TX_P2, SP_INFOFRAME_MPEG_TYPE_REG,
			     sp.frame.vendor.hdmi.type);
		sp_reg_write(anx78xx, TX_P2, SP_INFOFRAME_MPEG_VER_REG,
			     sp.frame.vendor.hdmi.version);
		sp_reg_write(anx78xx, TX_P2, SP_INFOFRAME_MPEG_LEN_REG,
			     sp.frame.vendor.hdmi.length);
		/* pack the VSI information into a binary buffer */
		hdmi_vendor_infoframe_pack(&sp.frame.vendor.hdmi, buffer,
					   HDMI_INFOFRAME_HEADER_SIZE +
					   sp.frame.vendor.hdmi.length);
		for (i = 0; i < sp.frame.vendor.hdmi.length; i++)
			sp_reg_write(anx78xx, TX_P2,
				     SP_INFOFRAME_MPEG_DB0_REG + i, ptr[i]);
		break;
	default:
		break;
	}
}

static void sp_config_packets(struct anx78xx *anx78xx,
			      enum hdmi_infoframe_type type)
{
	switch (type) {
	case HDMI_INFOFRAME_TYPE_AUDIO:
		sp_reg_clear_bits(anx78xx, TX_P0, SP_PACKET_SEND_CTRL_REG,
				  SP_AUD_IF_EN);
		sp_load_packet(anx78xx, HDMI_INFOFRAME_TYPE_AUDIO);
		sp_reg_set_bits(anx78xx, TX_P0, SP_PACKET_SEND_CTRL_REG,
				SP_AUD_IF_UP);
		sp_reg_set_bits(anx78xx, TX_P0, SP_PACKET_SEND_CTRL_REG,
				SP_AUD_IF_EN);
		break;
	case HDMI_INFOFRAME_TYPE_AVI:
		sp_reg_clear_bits(anx78xx, TX_P0, SP_PACKET_SEND_CTRL_REG,
				  SP_AVI_IF_EN);
		sp_load_packet(anx78xx, HDMI_INFOFRAME_TYPE_AVI);
		sp_reg_set_bits(anx78xx, TX_P0, SP_PACKET_SEND_CTRL_REG,
				SP_AVI_IF_UD);
		sp_reg_set_bits(anx78xx, TX_P0, SP_PACKET_SEND_CTRL_REG,
				SP_AVI_IF_EN);
		break;
	case HDMI_INFOFRAME_TYPE_MPEG:
		sp_reg_clear_bits(anx78xx, TX_P0, SP_PACKET_SEND_CTRL_REG,
				  SP_MPEG_IF_EN);
		sp_load_packet(anx78xx, HDMI_INFOFRAME_TYPE_MPEG);
		sp_reg_set_bits(anx78xx, TX_P0, SP_PACKET_SEND_CTRL_REG,
				SP_MPEG_IF_UD);
		sp_reg_set_bits(anx78xx, TX_P0, SP_PACKET_SEND_CTRL_REG,
				SP_MPEG_IF_EN);
		break;
	case HDMI_INFOFRAME_TYPE_VENDOR:
		sp_reg_clear_bits(anx78xx, TX_P0, SP_PACKET_SEND_CTRL_REG,
				  SP_MPEG_IF_EN);
		sp_load_packet(anx78xx, HDMI_INFOFRAME_TYPE_VENDOR);
		sp_reg_set_bits(anx78xx, TX_P0, SP_PACKET_SEND_CTRL_REG,
				SP_MPEG_IF_UD);
		sp_reg_set_bits(anx78xx, TX_P0, SP_PACKET_SEND_CTRL_REG,
				SP_MPEG_IF_EN);
		break;
	default:
		break;
	}
}

static bool sp_config_video_output(struct anx78xx *anx78xx)
{
	struct device *dev = &anx78xx->client->dev;
	u8 val;

	switch (sp.tx_vo_state) {
	default:
	case VO_WAIT_VIDEO_STABLE:
		sp_reg_read(anx78xx, RX_P0, SP_SYSTEM_STATUS_REG, &val);
		if ((val & SP_TMDS_DE_DET) && (val & SP_TMDS_CLOCK_DET)) {
			if (sp_tx_bw_lc_sel(anx78xx))
				return false;
			sp_enable_video_input(anx78xx, false);
			sp_hdmi_new_avi_int(anx78xx);
			sp_reg_read(anx78xx, RX_P0,
				    SP_PACKET_RECEIVING_STATUS_REG, &val);
			if (val & SP_VSI_RCVD)
				sp_hdmi_new_vsi_int(anx78xx);
			sp_enable_video_input(anx78xx, true);
			sp.tx_vo_state = VO_WAIT_TX_VIDEO_STABLE;
		} else {
			dev_dbg(dev, "HDMI input video not stable!\n");
			break;
		}
	/* fallthrough */
	case VO_WAIT_TX_VIDEO_STABLE:
		/*
		 * The flag is write clear and can be latched from last
		 * status. So the first read and write is to clear the
		 * previous status.
		 */
		sp_reg_read(anx78xx, TX_P0, SP_DP_SYSTEM_CTRL_BASE + 2, &val);
		sp_reg_write(anx78xx, TX_P0, SP_DP_SYSTEM_CTRL_BASE + 2, val);

		sp_reg_read(anx78xx, TX_P0, SP_DP_SYSTEM_CTRL_BASE + 2, &val);
		if (val & SP_CHA_STA) {
			dev_dbg(dev, "stream clock not stable!\n");
			break;
		}

		/*
		 * The flag is write clear and can be latched from
		 * last status. So the first read and write is to
		 * clear the previous status.
		 */
		sp_reg_read(anx78xx, TX_P0, SP_DP_SYSTEM_CTRL_BASE + 3, &val);
		sp_reg_write(anx78xx, TX_P0, SP_DP_SYSTEM_CTRL_BASE + 3,
			     val);

		sp_reg_read(anx78xx, TX_P0, SP_DP_SYSTEM_CTRL_BASE + 3, &val);
		if (val & SP_STRM_VALID) {
			if (sp.tx_test_lt)
				sp.tx_test_lt = false;
			sp.tx_vo_state = VO_FINISH;
		} else {
			dev_err(dev, "video stream not valid!\n");
			break;
		}
	/* fallthrough */
	case VO_FINISH:
		sp_block_power_ctrl(anx78xx, SP_TX_PWR_AUDIO, false);
		sp_hdmi_mute_video(anx78xx, false);
		sp_video_mute(anx78xx, false);
		sp_show_information(anx78xx);
		return true;
	}

	return false;
}

static inline void sp_hdcp_encryption_disable(struct anx78xx *anx78xx)
{
	sp_reg_clear_bits(anx78xx, TX_P0, SP_HDCP_CTRL0_REG, SP_HDCP_ENC_EN);
}

static inline void sp_hdcp_encryption_enable(struct anx78xx *anx78xx)
{
	sp_reg_set_bits(anx78xx, TX_P0, SP_HDCP_CTRL0_REG, SP_HDCP_ENC_EN);
}

static void sp_hw_hdcp_enable(struct anx78xx *anx78xx)
{
	sp_reg_clear_bits(anx78xx, TX_P0, SP_HDCP_CTRL0_REG,
			  SP_HDCP_ENC_EN | SP_HARD_AUTH_EN);
	sp_reg_set_bits(anx78xx, TX_P0, SP_HDCP_CTRL0_REG,
			SP_HARD_AUTH_EN | SP_BKSV_SRM_PASS |
			SP_KSVLIST_VLD | SP_HDCP_ENC_EN);

	/*
	 * Set the wait timing value for R0 checking of HDCP first step
	 * authentication after write AKSV to receiver. Default value is
	 * 0x64 (100ms).
	 */
	sp_reg_write(anx78xx, TX_P0, SP_HDCP_WAIT_R0_TIME_REG, 0xb0);

	/*
	 * Set the wait timing value for repeater KSVFIFO ready in HDCP first
	 * step authentication. Default value is 0x9c (4.2s)
	 */
	sp_reg_write(anx78xx, TX_P0, SP_HDCP_RPTR_RDY_WAIT_TIME_REG, 0xc8);
}

static bool sp_hdcp_process(struct anx78xx *anx78xx)
{
	struct device *dev = &anx78xx->client->dev;

	switch (sp.hdcp_state) {
	case HDCP_CAPABLE_CHECK:
		sp.hdcp_fail_count = 0;
		if (is_anx_dongle(anx78xx))
			sp.hdcp_state = HDCP_WAITING_VID_STB;
		else
			sp.hdcp_state = HDCP_HW_ENABLE;
		if (!sp.hdcp_enabled)
			sp.hdcp_state = HDCP_NOT_SUPPORTED;
		if (sp.hdcp_state != HDCP_WAITING_VID_STB)
			break;
	/* fallthrough */
	case HDCP_WAITING_VID_STB:
		msleep(100);
		sp.hdcp_state = HDCP_HW_ENABLE;
	/* fallthrough */
	case HDCP_HW_ENABLE:
		sp_video_mute(anx78xx, true);
		sp_clean_hdcp_status(anx78xx);
		sp_block_power_ctrl(anx78xx, SP_TX_PWR_HDCP, false);
		msleep(20);
		sp_block_power_ctrl(anx78xx, SP_TX_PWR_HDCP, true);
		sp_reg_write(anx78xx, TX_P2, SP_COMMON_INT_MASK_BASE + 2,
			     0x01);
		msleep(50);
		sp_hw_hdcp_enable(anx78xx);
		sp.hdcp_state = HDCP_WAITING_FINISH;
	/* fallthrough */
	case HDCP_WAITING_FINISH:
		break;
	case HDCP_FINISH:
		sp_hdcp_encryption_enable(anx78xx);
		sp_hdmi_mute_video(anx78xx, false);
		sp_video_mute(anx78xx, false);
		sp.hdcp_state = HDCP_CAPABLE_CHECK;
		dev_dbg(dev, "HDCP authentication pass\n");
		return true;
	case HDCP_FAILED:
		if (sp.hdcp_fail_count > 5) {
			sp_reg_hardware_reset(anx78xx);
			sp.hdcp_state = HDCP_CAPABLE_CHECK;
			sp.hdcp_fail_count = 0;
			dev_dbg(dev, "HDCP authentication failed\n");
		} else {
			sp.hdcp_fail_count++;
			sp.hdcp_state = HDCP_WAITING_VID_STB;
		}
		break;
	default:
	case HDCP_NOT_SUPPORTED:
		dev_dbg(dev, "sink is not capable HDCP\n");
		sp_block_power_ctrl(anx78xx, SP_TX_PWR_HDCP, false);
		sp_video_mute(anx78xx, false);
		sp.hdcp_state = HDCP_CAPABLE_CHECK;
		return true;
	}

	return false;
}

static void sp_enable_audio_output(struct anx78xx *anx78xx, bool enable)
{
	u8 val;

	sp_reg_clear_bits(anx78xx, TX_P0, SP_DP_AUDIO_CTRL_REG, SP_AUD_EN);
	if (enable) {
		sp_audio_infoframe_init(anx78xx);
		sp_config_packets(anx78xx, HDMI_INFOFRAME_TYPE_AUDIO);

		sp_reg_read(anx78xx, RX_P0, SP_HDMI_STATUS_REG, &val);
		if (val & SP_HDMI_AUD_LAYOUT) {
			sp_reg_read(anx78xx, RX_P1, SP_AUD_INFOFRAME_DATA_BASE,
				    &val);
			sp_reg_write(anx78xx, TX_P2, SP_AUD_CH_STATUS_BASE + 5,
				     (val & 0x07) << 5 | SP_AUDIO_LAYOUT);
		} else {
			sp_reg_write(anx78xx, TX_P2, SP_AUD_CH_STATUS_BASE + 5,
				     SP_I2S_CH_NUM_2 & ~SP_AUDIO_LAYOUT);
		}
		sp_reg_set_bits(anx78xx, TX_P0, SP_DP_AUDIO_CTRL_REG,
				SP_AUD_EN);
	} else {
		sp_reg_clear_bits(anx78xx, TX_P0, SP_PACKET_SEND_CTRL_REG,
				  SP_AUD_IF_EN);
	}
}

static int sp_calculate_audio_m_value(struct anx78xx *anx78xx)
{
	u8 val;
	struct device *dev = &anx78xx->client->dev;
	unsigned long m_aud, ls_clk = 0;
	unsigned long aud_freq = 0;

	sp_reg_read(anx78xx, RX_P0, SP_AUD_SPDIF_CH_STATUS_BASE + 4, &val);

	switch (val & SP_FS_FREQ_MASK) {
	case SP_FS_FREQ_44100HZ:
		aud_freq = 44100;
		break;
	case SP_FS_FREQ_48000HZ:
		aud_freq = 48000;
		break;
	case SP_FS_FREQ_32000HZ:
		aud_freq = 32000;
		break;
	case SP_FS_FREQ_88200HZ:
		aud_freq = 88200;
		break;
	case SP_FS_FREQ_96000HZ:
		aud_freq = 96000;
		break;
	case SP_FS_FREQ_176400HZ:
		aud_freq = 176400;
		break;
	case SP_FS_FREQ_192000HZ:
		aud_freq = 192000;
		break;
	default:
		dev_err(dev, "invalid sampling clock frequency %d\n",
			val & SP_FS_FREQ_MASK);
		return -EINVAL;
	}

	switch (sp_get_link_bandwidth(anx78xx)) {
	case DP_LINK_BW_1_62:
		ls_clk = 162000;
		break;
	case DP_LINK_BW_2_7:
		ls_clk = 270000;
		break;
	case DP_LINK_BW_5_4:
		ls_clk = 540000;
		break;
	case DP_LINK_BW_6_75:
		ls_clk = 675000;
		break;
	default:
		dev_err(dev, "invalid main link bandwidth setting\n");
		return -EINVAL;
	}

	dev_dbg(dev, "aud_freq = %ld , LS_CLK = %ld\n", aud_freq, ls_clk);

	m_aud = (((512 * aud_freq) / ls_clk) * 32768) / 1000;
	sp_reg_write(anx78xx, TX_P1, SP_AUD_INTERFACE_CTRL4_REG, m_aud & 0xff);
	m_aud = m_aud >> 8;
	sp_reg_write(anx78xx, TX_P1, SP_AUD_INTERFACE_CTRL5_REG, m_aud & 0xff);
	sp_reg_write(anx78xx, TX_P1, SP_AUD_INTERFACE_CTRL6_REG, 0x00);

	return 0;
}

static void sp_config_audio(struct anx78xx *anx78xx)
{
	u8 val;

	sp_block_power_ctrl(anx78xx, SP_TX_PWR_AUDIO, true);

	sp_reg_read(anx78xx, TX_P0, SP_DP_MAIN_LINK_BW_SET_REG, &val);
	if ((val & SP_INITIAL_SLIM_M_AUD_SEL) &&
	    sp_calculate_audio_m_value(anx78xx))
			return;

	sp_reg_clear_bits(anx78xx, TX_P1, SP_AUD_INTERFACE_CTRL0_REG,
			  SP_AUD_INTERFACE_DISABLE);

	sp_reg_set_bits(anx78xx, TX_P1, SP_AUD_INTERFACE_CTRL2_REG,
			SP_M_AUD_ADJUST_ST);

	sp_reg_read(anx78xx, RX_P0, SP_HDMI_STATUS_REG, &val);
	if (val & SP_HDMI_AUD_LAYOUT)
		sp_reg_set_bits(anx78xx, TX_P2, SP_AUD_CH_STATUS_BASE + 5,
				SP_I2S_CH_NUM_8 | SP_AUDIO_LAYOUT);
	else
		sp_reg_clear_bits(anx78xx, TX_P2, SP_AUD_CH_STATUS_BASE + 5,
				  SP_I2S_CHANNEL_NUM_MASK | SP_AUDIO_LAYOUT);

	sp_hdmi_audio_samplechg_int(anx78xx);

	/* enable audio */
	sp_enable_audio_output(anx78xx, true);
}

static bool sp_config_audio_output(struct anx78xx *anx78xx)
{
	u8 val;

	switch (sp.tx_ao_state) {
	default:
	case AO_INIT:
	case AO_CTS_RCV_INT:
	case AO_AUDIO_RCV_INT:
		sp_reg_read(anx78xx, RX_P0, SP_HDMI_STATUS_REG, &val);
		if (!(val & SP_HDMI_MODE)) {
			sp.tx_ao_state = AO_INIT;
			return true;
		}
		break;
	case AO_RCV_INT_FINISH:
		if (sp.audio_stable_count++ > 2) {
			sp.tx_ao_state = AO_OUTPUT;
		} else {
			sp.tx_ao_state = AO_INIT;
			break;
		}
	/* fallthrough */
	case AO_OUTPUT:
		sp.audio_stable_count = 0;
		sp.tx_ao_state = AO_INIT;
		sp_video_mute(anx78xx, false);
		sp_hdmi_mute_audio(anx78xx, false);
		sp_config_audio(anx78xx);
		return true;
	}

	return false;
}

static void sp_initialization(struct anx78xx *anx78xx)
{
	sp.read_edid_flag = false;

	/* Power on all modules */
	sp_reg_write(anx78xx, TX_P2, SP_POWERDOWN_CTRL_REG, 0x00);
	/* Driver Version */
	sp_reg_write(anx78xx, TX_P1, SP_FW_VER_REG, FW_VERSION);
	sp_hdmi_initialization(anx78xx);
	sp_tx_initialization(anx78xx);
	msleep(200);
}

/*
 * Interrupt receiver function, gets the service interrupts and updates the
 * status of the interrupts so that correct interrupt service routines can
 * be called in the SlimPort task handler function.
 */
static void sp_int_receiver(struct anx78xx *anx78xx)
{
	int i;

	/* Common Interrupt Status Registers */
	for (i = 0; i < ARRAY_SIZE(sp.common_int); i++) {
		sp_reg_read(anx78xx, TX_P2, SP_COMMON_INT_STATUS_BASE + 1 + i,
			    &sp.common_int[i]);
		sp_reg_write(anx78xx, TX_P2, SP_COMMON_INT_STATUS_BASE + 1 + i,
			     sp.common_int[i]);
	}

	/* Display Port Interrupt Status Register */
	sp_reg_read(anx78xx, TX_P2, SP_DP_INT_STATUS_REG, &sp.dp_int);
	sp_reg_write(anx78xx, TX_P2, SP_DP_INT_STATUS_REG, sp.dp_int);

	/* Interrupt Status Registers */
	for (i = 0; i < ARRAY_SIZE(sp.sp_hdmi_int); i++) {
		sp_reg_read(anx78xx, RX_P0, SP_INT_STATUS1_REG + i,
			    &sp.sp_hdmi_int[i]);
		sp_reg_write(anx78xx, RX_P0, SP_INT_STATUS1_REG + i,
			     sp.sp_hdmi_int[i]);
	}
}

static void sp_pll_changed_int_handler(struct anx78xx *anx78xx)
{
	struct device *dev = &anx78xx->client->dev;

	if (sp.tx_system_state >= STATE_LINK_TRAINING) {
		if (!sp_get_pll_lock_status(anx78xx)) {
			dev_dbg(dev, "PLL not lock!\n");
			sp_set_system_state(anx78xx, STATE_LINK_TRAINING);
		}
	}
}

static u8 sp_dp_autotest_link_training(struct anx78xx *anx78xx)
{
	u8 bandwidth, response;

	sp_dp_read_bytes_from_dpcd(anx78xx, DP_TEST_LINK_RATE, 1,
				   &bandwidth);
	switch (bandwidth) {
	case DP_LINK_BW_1_62:
	case DP_LINK_BW_2_7:
	case DP_LINK_BW_5_4:
	case DP_LINK_BW_6_75:
		sp_set_link_bandwidth(anx78xx, bandwidth);
		sp.tx_test_bw = bandwidth;
		break;
	default:
		sp_set_link_bandwidth(anx78xx, DP_LINK_BW_6_75);
		sp.tx_test_bw = DP_LINK_BW_6_75;
		break;
	}

	sp_dp_read_bytes_from_dpcd(anx78xx, DP_TEST_RESPONSE, 1, &response);
	response |= DP_TEST_ACK;

	return response;
}

static u8 sp_autotest_phy_pattern(struct anx78xx *anx78xx)
{
	struct device *dev = &anx78xx->client->dev;
	u8 b_sw, response;
	u8 buf[16];
	int i;

	sp_dp_autotest_link_training(anx78xx);

	sp_dp_read_bytes_from_dpcd(anx78xx, DP_PHY_TEST_PATTERN, 1, buf);
	dev_dbg(dev, "DPCD: PHY test pattern = %.2x\n", buf[0]);
	switch (buf[0]) {
	case 0:
		break;
	case 1:
		sp_reg_write(anx78xx, TX_P0, SP_DP_TRAINING_PATTERN_SET_REG,
			     0x04);
		break;
	case 2:
		sp_reg_write(anx78xx, TX_P0, SP_DP_TRAINING_PATTERN_SET_REG,
			     0x08);
		break;
	case 3:
		sp_reg_write(anx78xx, TX_P0, SP_DP_TRAINING_PATTERN_SET_REG,
			     0x0c);
		break;
	case 4:
		/* 00249h - 0025Fh: Reserved for test automation extensions */
		sp_dp_read_bytes_from_dpcd(anx78xx, 0x250, 10,
					   buf);
		for (i = 0; i < SP_DP_LT_80BIT_PATTERN_REG_NUM; i++) {
			sp_reg_write(anx78xx, TX_P1,
				     SP_DP_LT_80BIT_PATTERN0_REG + i,
				     buf[0]);
		}
		sp_reg_write(anx78xx, TX_P0, SP_DP_TRAINING_PATTERN_SET_REG,
			     0x30);
		break;
	case 5:
		sp_reg_write(anx78xx, TX_P0, SP_DP_CEP_TRAINING_CTRL0_REG,
			     0x00);
		sp_reg_write(anx78xx, TX_P0, SP_DP_CEP_TRAINING_CTRL1_REG,
			     0x01);
		sp_reg_write(anx78xx, TX_P0, SP_DP_TRAINING_PATTERN_SET_REG,
			     0x14);
		break;
	}

	sp_dp_read_bytes_from_dpcd(anx78xx, DP_MAX_DOWNSPREAD, 1, buf);
	dev_dbg(dev, "DPCD: max downspread = %.2x\n", buf[0]);
	if (buf[0] & DP_PERCENT_DOWNSPREAD_0_5)
		sp_config_ssc(anx78xx, SSC_DEP_4000PPM);
	else
		sp_downspreading_enable(anx78xx, false);

	/* get swing and emphasis adjust request */
	sp_reg_read(anx78xx, TX_P0, SP_DP_LANE0_LT_CTRL_REG, &b_sw);

	sp_dp_read_bytes_from_dpcd(anx78xx, DP_ADJUST_REQUEST_LANE0_1, 1, buf);
	dev_dbg(dev, "DPCD: 0x00206 = %.2x\n", buf[0]);
	buf[0] &= DP_ADJUST_VOLTAGE_SWING_LANE0_MASK |
		  DP_ADJUST_PRE_EMPHASIS_LANE0_MASK;
	switch (buf[0]) {
	case 0x00:
	case 0x01:
	case 0x02:
	case 0x03:
		sp_reg_write(anx78xx, TX_P0, SP_DP_LANE0_LT_CTRL_REG,
			     (b_sw & ~SP_TX_SW_SET_MASK) | buf[0]);
		break;
	case 0x04:
	case 0x05:
	case 0x06:
		sp_reg_write(anx78xx, TX_P0, SP_DP_LANE0_LT_CTRL_REG,
			     (b_sw & ~SP_TX_SW_SET_MASK) |
			     (buf[0] + 4));
		break;
	case 0x08:
		sp_reg_write(anx78xx, TX_P0, SP_DP_LANE0_LT_CTRL_REG,
			     (b_sw & ~SP_TX_SW_SET_MASK) | 0x10);
		break;
	case 0x09:
		sp_reg_write(anx78xx, TX_P0, SP_DP_LANE0_LT_CTRL_REG,
			     (b_sw & ~SP_TX_SW_SET_MASK) | 0x11);
		break;
	case 0x0c:
		sp_reg_write(anx78xx, TX_P0, SP_DP_LANE0_LT_CTRL_REG,
			     (b_sw & ~SP_TX_SW_SET_MASK) | 0x18);
		break;
	default:
		break;
	}

	sp_dp_read_bytes_from_dpcd(anx78xx, DP_TEST_RESPONSE, 1, &response);
	response |= DP_TEST_ACK;

	return response;
}

static void sp_hpd_irq_process(struct anx78xx *anx78xx)
{
	struct device *dev = &anx78xx->client->dev;
	u8 val;
	u8 test_vector;
	u8 buf[6];

	sp_dp_read_bytes_from_dpcd(anx78xx, DP_SINK_COUNT, 6, buf);
	dev_dbg(dev, "got hpd irq %x\n", buf[1]);

	if (buf[1] != 0)
		sp_dp_write_bytes_to_dpcd(anx78xx, DP_DEVICE_SERVICE_IRQ_VECTOR,
					  1, &buf[1]);

	/* HDCP IRQ */
	if ((buf[1] & DP_CP_IRQ) &&
	    (sp.hdcp_state > HDCP_WAITING_FINISH ||
	     sp.tx_system_state >= STATE_HDCP_AUTH)) {
		sp_dp_read_bytes_from_dpcd(anx78xx, 0x068029, 1,
					   &val);
		if (val & 0x04) {
			if (!sp_hdcp_repeater_mode(anx78xx)) {
				sp_set_system_state(anx78xx, STATE_HDCP_AUTH);
				sp_clean_hdcp_status(anx78xx);
			} else {
				sp.repeater_state = HDCP_ERROR;
			}
			dev_dbg(dev, "CP_IRQ, HDCP sync lost.\n");
		}
	}

	/* PHY and Link CTS test */
	if (buf[1] & DP_AUTOMATED_TEST_REQUEST) {
		sp_dp_read_bytes_from_dpcd(anx78xx, DP_TEST_REQUEST, 1,
					   &test_vector);

		if (test_vector & DP_TEST_LINK_TRAINING) {
			dev_dbg(dev, "LINK_TRAINING test requested\n");
			sp.tx_test_lt = true;
			val = sp_dp_autotest_link_training(anx78xx);
			sp_dp_write_bytes_to_dpcd(anx78xx, DP_TEST_RESPONSE, 1,
						  &val);

			if (sp.tx_system_state >= STATE_LINK_TRAINING) {
				sp.tx_lt_state = LT_INIT;
				sp_set_system_state(anx78xx,
						    STATE_LINK_TRAINING);
			}
		}

		if (test_vector & DP_TEST_LINK_VIDEO_PATTERN) {
			dev_dbg(dev, "VIDEO_PATTERN test requested\n");
			sp_dp_read_bytes_from_dpcd(anx78xx, DP_TEST_RESPONSE, 1,
						   &val);
			val |= DP_TEST_ACK;
			sp_dp_write_bytes_to_dpcd(anx78xx, DP_TEST_RESPONSE, 1,
						  &val);
		}

		if (test_vector & DP_TEST_LINK_EDID_READ) {
			dev_dbg(dev, "EDID test requested\n");
			if (sp.tx_system_state > STATE_PARSE_EDID)
				sp_set_system_state(anx78xx, STATE_PARSE_EDID);
			sp.tx_test_edid = true;
		}

		if (test_vector & DP_TEST_LINK_PHY_TEST_PATTERN) {
			dev_dbg(dev, "PHY_PATTERN test requested\n");
			sp.tx_test_lt = true;
			val = sp_autotest_phy_pattern(anx78xx);
			sp_dp_write_bytes_to_dpcd(anx78xx, DP_TEST_RESPONSE, 1,
						  &val);
		}
	}

	if (sp.tx_system_state > STATE_LINK_TRAINING) {
		if ((sp.tx_system_state == STATE_HDCP_AUTH) &&
		    (buf[1] & DP_CP_IRQ)) {
			dev_dbg(dev, "CP IRQ!\n");
		} else if (!(buf[4] & DP_INTERLANE_ALIGN_DONE) || ((buf[2] &
			  (DP_LANE_CR_DONE | DP_LANE_SYMBOL_LOCKED)) !=
			  (DP_LANE_CR_DONE | DP_LANE_SYMBOL_LOCKED))) {
			sp_set_system_state(anx78xx, STATE_LINK_TRAINING);
			dev_dbg(dev, "IRQ: re-LT request!\n");
			return;
		}

		dev_dbg(dev, "lane align %x\n", buf[4]);
		dev_dbg(dev, "lane clock recovery %x\n", buf[2]);
	}
}

static void sp_auth_done_int_handler(struct anx78xx *anx78xx)
{
	struct device *dev = &anx78xx->client->dev;
	u8 buf[2];

	if (sp_hdcp_repeater_mode(anx78xx)) {
		sp_reg_read(anx78xx, TX_P0, SP_TX_HDCP_STATUS_REG, &buf[0]);
		if ((buf[0] & SP_AUTHEN_PASS) &&
		    (sp.repeater_state == HDCP_DOING))
			sp.repeater_state = HDCP_DONE;
		else
			sp.repeater_state = HDCP_ERROR;

		return;
	}

	if (sp.hdcp_state > HDCP_HW_ENABLE &&
	    sp.tx_system_state == STATE_HDCP_AUTH) {
		sp_reg_read(anx78xx, TX_P0, SP_HDCP_RX_BSTATUS0_REG, &buf[0]);
		sp_reg_read(anx78xx, TX_P0, SP_HDCP_RX_BSTATUS1_REG, &buf[1]);
		if ((buf[0] & 0x08) || (buf[1] & 0x80)) {
			dev_dbg(dev, "max cascade/devs exceeded!\n");
			sp_hdcp_encryption_disable(anx78xx);
			sp.hdcp_state = HDCP_FINISH;
		} else {
			sp_reg_read(anx78xx, TX_P0, SP_TX_HDCP_STATUS_REG,
				    buf);
		}

		if (buf[0] & SP_AUTHEN_PASS) {
			sp_dp_read_bytes_from_dpcd(anx78xx, 0x06802a, 2, buf);
			if ((buf[0] & 0x08) || (buf[1] & 0x80)) {
				dev_dbg(dev, "max cascade/devs exceeded!\n");
				sp_hdcp_encryption_disable(anx78xx);
			} else
				dev_dbg(dev, "%s\n",
					"authentication pass in Auth Done");

			sp.hdcp_state = HDCP_FINISH;
		} else {
			dev_err(dev, "authentication failed in Auth Done\n");
			sp_video_mute(anx78xx, true);
			sp_clean_hdcp_status(anx78xx);
			sp.hdcp_state = HDCP_FAILED;
		}
	}
}

static void sp_lt_done_int_handler(struct anx78xx *anx78xx)
{
	struct device *dev = &anx78xx->client->dev;
	u8 val;

	if (sp.tx_lt_state == LT_WAITING_FINISH &&
	    sp.tx_system_state == STATE_LINK_TRAINING) {
		sp_reg_read(anx78xx, TX_P0, SP_DP_LT_CTRL_REG, &val);
		if (val & SP_LT_ERROR_TYPE_MASK) {
			val = (val & SP_LT_ERROR_TYPE_MASK) >> 4;
			dev_dbg(dev, "LT failed in interrupt %.2x\n",
				val);
			sp.tx_lt_state = LT_ERROR;
		} else {
			dev_dbg(dev, "LT finish\n");
			sp.tx_lt_state = LT_FINISH;
		}
	}
}

static void sp_hdmi_clk_det_int(struct anx78xx *anx78xx)
{
	struct device *dev = &anx78xx->client->dev;

	dev_dbg(dev, "pixel clock change\n");
	if (sp.tx_system_state > STATE_VIDEO_OUTPUT) {
		sp_video_mute(anx78xx, true);
		sp_enable_audio_output(anx78xx, false);
		sp_set_system_state(anx78xx, STATE_VIDEO_OUTPUT);
	}
}

static void sp_hdmi_dvi_int(struct anx78xx *anx78xx)
{
	struct device *dev = &anx78xx->client->dev;
	u8 val;

	sp_reg_read(anx78xx, RX_P0, SP_HDMI_STATUS_REG, &val);
	if ((val & SP_HDMI_DET) == SP_DVI_MODE) {
		dev_dbg(dev, "detected DVI MODE -> mute audio\n");
		sp_hdmi_mute_audio(anx78xx, true);
		sp_set_system_state(anx78xx, STATE_LINK_TRAINING);
	}
}

static void sp_hdmi_new_avi_int(struct anx78xx *anx78xx)
{
	sp_avi_infoframe_init(anx78xx);
	sp_config_packets(anx78xx, HDMI_INFOFRAME_TYPE_AVI);
	sp_set_colorspace(anx78xx);
	sp_lvttl_bit_mapping(anx78xx);
}

static void sp_hdmi_new_vsi_int(struct anx78xx *anx78xx)
{
	struct device *dev = &anx78xx->client->dev;
	u8 v3d_struct;

	sp_reg_clear_bits(anx78xx, TX_P0, SP_DP_3D_VSC_CTRL_REG,
			  SP_INFO_FRAME_VSC_EN);

	sp_vsi_infoframe_init(anx78xx);
	sp_config_packets(anx78xx, HDMI_INFOFRAME_TYPE_VENDOR);

	switch (sp.frame.vendor.hdmi.s3d_struct) {
	case HDMI_3D_STRUCTURE_FRAME_PACKING:
		v3d_struct = 0x02;
		break;
	case HDMI_3D_STRUCTURE_LINE_ALTERNATIVE:
		v3d_struct = 0x03;
		break;
	case HDMI_3D_STRUCTURE_SIDE_BY_SIDE_FULL:
		v3d_struct = 0x04;
		break;
	case HDMI_3D_STRUCTURE_INVALID:
	default:
		v3d_struct = 0x00;
		dev_dbg(dev, "3D structure not supported\n");
		break;
	}
	sp_reg_write(anx78xx, TX_P0, SP_DP_VSC_DB1_REG, v3d_struct);

	sp_reg_set_bits(anx78xx, TX_P0, SP_DP_3D_VSC_CTRL_REG,
			SP_INFO_FRAME_VSC_EN);
	sp_reg_clear_bits(anx78xx, TX_P0, SP_PACKET_SEND_CTRL_REG,
			  SP_SPD_IF_EN);
	sp_reg_set_bits(anx78xx, TX_P0, SP_PACKET_SEND_CTRL_REG, SP_SPD_IF_UD);
	sp_reg_set_bits(anx78xx, TX_P0, SP_PACKET_SEND_CTRL_REG, SP_SPD_IF_EN);
}

static void sp_hdmi_no_vsi_int(struct anx78xx *anx78xx)
{
	struct device *dev = &anx78xx->client->dev;
	u8 val;

	sp_reg_read(anx78xx, TX_P0, SP_DP_3D_VSC_CTRL_REG, &val);
	if (val & SP_INFO_FRAME_VSC_EN) {
		dev_dbg(dev, "no new VSI is received, disable VSC packet\n");
		val &= ~SP_INFO_FRAME_VSC_EN;
		sp_reg_write(anx78xx, TX_P0, SP_DP_3D_VSC_CTRL_REG, val);
		sp_mpeg_infoframe_init(anx78xx);
		sp_config_packets(anx78xx, HDMI_INFOFRAME_TYPE_MPEG);
	}
}

static inline void sp_hdmi_restart_audio_chk(struct anx78xx *anx78xx)
{
	sp_set_system_state(anx78xx, STATE_AUDIO_OUTPUT);
}

static void sp_hdmi_cts_rcv_int(struct anx78xx *anx78xx)
{
	if (sp.tx_ao_state == AO_INIT)
		sp.tx_ao_state = AO_CTS_RCV_INT;
	else if (sp.tx_ao_state == AO_AUDIO_RCV_INT)
		sp.tx_ao_state = AO_RCV_INT_FINISH;
}

static void sp_hdmi_audio_rcv_int(struct anx78xx *anx78xx)
{
	if (sp.tx_ao_state == AO_INIT)
		sp.tx_ao_state = AO_AUDIO_RCV_INT;
	else if (sp.tx_ao_state == AO_CTS_RCV_INT)
		sp.tx_ao_state = AO_RCV_INT_FINISH;
}

static void sp_hdmi_audio_samplechg_int(struct anx78xx *anx78xx)
{
	u16 i;
	u8 val;

	/* transfer audio channel status from HDMI Rx to Slimport Tx */
	for (i = 0; i < SP_AUD_CH_STATUS_REG_NUM; i++) {
		sp_reg_read(anx78xx, RX_P0, SP_AUD_SPDIF_CH_STATUS_BASE + i,
			    &val);
		sp_reg_write(anx78xx, TX_P2, SP_AUD_CH_STATUS_BASE + i,
			     val);
	}
}

static void sp_hdmi_hdcp_error_int(struct anx78xx *anx78xx)
{
	struct device *dev = &anx78xx->client->dev;

	if (sp.hdcp_error_count >= 40) {
		sp.hdcp_error_count = 0;
		dev_dbg(dev, "lots of HDCP errors occurred!\n");
		sp_hdmi_mute_audio(anx78xx, true);
		sp_hdmi_mute_video(anx78xx, true);
		sp_hdmi_set_hpd(anx78xx, false);
		usleep_range(10000, 11000);
		sp_hdmi_set_hpd(anx78xx, true);
	} else {
		sp.hdcp_error_count++;
	}
}

static void sp_hdmi_new_gcp_int(struct anx78xx *anx78xx)
{
	u8 val;

	sp_reg_read(anx78xx, RX_P1, SP_GENERAL_CTRL_PACKET_REG, &val);
	if (val & SP_SET_AVMUTE) {
		sp_hdmi_mute_video(anx78xx, true);
		sp_hdmi_mute_audio(anx78xx, true);
	} else if (val & SP_CLEAR_AVMUTE) {
		sp_hdmi_mute_video(anx78xx, false);
		sp_hdmi_mute_audio(anx78xx, false);
	}
}

static void sp_hpd_int_handler(struct anx78xx *anx78xx, u8 hpd_source)
{
	u8 val;
	struct device *dev = &anx78xx->client->dev;

	switch (hpd_source) {
	case SP_HPD_LOST:
		dev_dbg(dev, "HPD: cable is pulled out\n");
		sp_hdmi_set_hpd(anx78xx, false);
		sp_set_system_state(anx78xx, STATE_CABLE_UNPLUGGED);
		break;
	case SP_HPD_CHG:
		dev_dbg(dev, "HPD: hotplug change detected\n");
		usleep_range(2000, 4000);
		if (sp.common_int[3] & SP_HPD_IRQ)
			sp_hpd_irq_process(anx78xx);

		sp_reg_read(anx78xx, TX_P0, SP_DP_SYSTEM_CTRL_BASE + 3, &val);
		if (val & SP_HPD_STATUS) {
			if (sp.common_int[3] & SP_HPD_IRQ)
				sp_hpd_irq_process(anx78xx);
		} else {
			sp_reg_read(anx78xx, TX_P0,
				    SP_DP_SYSTEM_CTRL_BASE + 3,
				    &val);
			if (val & SP_HPD_STATUS) {
				sp_hdmi_set_hpd(anx78xx, false);
				sp_set_system_state(anx78xx,
						    STATE_WAITING_CABLE_PLUG);
			}
		}
		break;
	default:
		break;
	}
}

static void sp_system_isr_handler(struct anx78xx *anx78xx)
{
	struct device *dev = &anx78xx->client->dev;

	if (sp.common_int[3] & SP_HPD_LOST)
		sp_hpd_int_handler(anx78xx, SP_HPD_LOST);
	else if (sp.common_int[3] & SP_HPD_CHG)
		sp_hpd_int_handler(anx78xx, SP_HPD_CHG);

	if (sp.common_int[0] & SP_PLL_LOCK_CHG)
		sp_pll_changed_int_handler(anx78xx);

	if (sp.common_int[1] & SP_HDCP_AUTH_DONE)
		sp_auth_done_int_handler(anx78xx);

	if ((sp.common_int[2] & SP_HDCP_LINK_CHECK_FAIL) &&
	    !sp_hdcp_repeater_mode(anx78xx)) {
		sp_set_system_state(anx78xx, STATE_LINK_TRAINING);
		dev_dbg(dev, "HDCP Sync Lost!\n");
	}

	if (sp.dp_int & SP_TRAINING_FINISH)
		sp_lt_done_int_handler(anx78xx);

	if (sp.tx_system_state > STATE_SINK_CONNECTION) {
		if (sp.sp_hdmi_int[5] & SP_NEW_AVI_PKT)
			sp_hdmi_new_avi_int(anx78xx);
	}

	if (sp.tx_system_state > STATE_VIDEO_OUTPUT) {
		if (sp.sp_hdmi_int[6] & SP_NEW_VS) {
			sp.sp_hdmi_int[6] &= ~SP_NO_VSI;
			sp_hdmi_new_vsi_int(anx78xx);
		}
		if (sp.sp_hdmi_int[6] & SP_NO_VSI)
			sp_hdmi_no_vsi_int(anx78xx);
	}

	if (sp.tx_system_state >= STATE_VIDEO_OUTPUT) {
		if (sp.sp_hdmi_int[0] & SP_CKDT_CHG)
			sp_hdmi_clk_det_int(anx78xx);

		if (sp.sp_hdmi_int[0] & SP_SCDT_CHG)
			dev_dbg(dev, "HDCP Sync Detected\n");

		if (sp.sp_hdmi_int[0] & SP_HDMI_DVI)
			sp_hdmi_dvi_int(anx78xx);

		if ((sp.sp_hdmi_int[5] & SP_NEW_AUD_PKT) ||
		    (sp.sp_hdmi_int[2] & SP_AUD_MODE_CHG))
			sp_hdmi_restart_audio_chk(anx78xx);

		if (sp.sp_hdmi_int[5] & SP_CTS_RCV)
			sp_hdmi_cts_rcv_int(anx78xx);

		if (sp.sp_hdmi_int[4] & SP_AUDIO_RCV)
			sp_hdmi_audio_rcv_int(anx78xx);

		if (sp.sp_hdmi_int[1] & SP_HDCP_ERR)
			sp_hdmi_hdcp_error_int(anx78xx);

		if (sp.sp_hdmi_int[5] & SP_NEW_CP_PKT)
			sp_hdmi_new_gcp_int(anx78xx);

		if (sp.sp_hdmi_int[1] & SP_AUDIO_SAMPLE_CHG)
			sp_hdmi_audio_samplechg_int(anx78xx);
	}
}

static void sp_show_information(struct anx78xx *anx78xx)
{
	struct device *dev = &anx78xx->client->dev;
	u8 val, val1;
	u16 h_res, h_act, v_res, v_act;
	u16 h_fp, h_sw, h_bp, v_fp, v_sw, v_bp;
	unsigned long refresh;
	unsigned long pclk;

	dev_dbg(dev, "\n************* SP Video Information **************\n");

	switch (sp_get_link_bandwidth(anx78xx)) {
	case DP_LINK_BW_1_62:
		dev_dbg(dev, "BW = 1.62G\n");
		break;
	case DP_LINK_BW_2_7:
		dev_dbg(dev, "BW = 2.7G\n");
		break;
	case DP_LINK_BW_5_4:
		dev_dbg(dev, "BW = 5.4G\n");
		break;
	case DP_LINK_BW_6_75:
		dev_dbg(dev, "BW = 6.75G\n");
		break;
	default:
		break;
	}

	pclk = sp_pclk_calc(anx78xx);
	pclk = pclk / 10;

	sp_reg_read(anx78xx, TX_P2, SP_TOTAL_LINE_STAL_REG, &val);
	sp_reg_read(anx78xx, TX_P2, SP_TOTAL_LINE_STAH_REG, &val1);

	v_res = val1;
	v_res = v_res << 8;
	v_res = v_res + val;

	sp_reg_read(anx78xx, TX_P2, SP_ACT_LINE_STAL_REG, &val);
	sp_reg_read(anx78xx, TX_P2, SP_ACT_LINE_STAH_REG, &val1);

	v_act = val1;
	v_act = v_act << 8;
	v_act = v_act + val;

	sp_reg_read(anx78xx, TX_P2, SP_TOTAL_PIXEL_STAL_REG, &val);
	sp_reg_read(anx78xx, TX_P2, SP_TOTAL_PIXEL_STAH_REG, &val1);

	h_res = val1;
	h_res = h_res << 8;
	h_res = h_res + val;

	sp_reg_read(anx78xx, TX_P2, SP_ACT_PIXEL_STAL_REG, &val);
	sp_reg_read(anx78xx, TX_P2, SP_ACT_PIXEL_STAH_REG, &val1);

	h_act = val1;
	h_act = h_act << 8;
	h_act = h_act + val;

	sp_reg_read(anx78xx, TX_P2, SP_H_F_PORCH_STAL_REG, &val);
	sp_reg_read(anx78xx, TX_P2, SP_H_F_PORCH_STAH_REG, &val1);

	h_fp = val1;
	h_fp = h_fp << 8;
	h_fp = h_fp + val;

	sp_reg_read(anx78xx, TX_P2, SP_H_SYNC_STAL_REG, &val);
	sp_reg_read(anx78xx, TX_P2, SP_H_SYNC_STAH_REG, &val1);

	h_sw = val1;
	h_sw = h_sw << 8;
	h_sw = h_sw + val;

	sp_reg_read(anx78xx, TX_P2, SP_H_B_PORCH_STAL_REG, &val);
	sp_reg_read(anx78xx, TX_P2, SP_H_B_PORCH_STAH_REG, &val1);

	h_bp = val1;
	h_bp = h_bp << 8;
	h_bp = h_bp + val;

	sp_reg_read(anx78xx, TX_P2, SP_V_F_PORCH_STA_REG, &val);
	v_fp = val;

	sp_reg_read(anx78xx, TX_P2, SP_V_SYNC_STA_REG, &val);
	v_sw = val;

	sp_reg_read(anx78xx, TX_P2, SP_V_B_PORCH_STA_REG, &val);
	v_bp = val;

	dev_dbg(dev, "total resolution is %d * %d\n", h_res, v_res);

	dev_dbg(dev, "HF=%d, HSW=%d, HBP=%d\n", h_fp, h_sw, h_bp);
	dev_dbg(dev, "VF=%d, VSW=%d, VBP=%d\n", v_fp, v_sw, v_bp);

	if (h_res == 0 || v_res == 0) {
		refresh = 0;
	} else {
		refresh = pclk * 1000000;
		refresh = DIV_ROUND_CLOSEST(refresh, h_res);
		refresh = DIV_ROUND_CLOSEST(refresh, v_res);
	}

	dev_dbg(dev, "active resolution is %d * %d @ %ldHz\n", h_act, v_act,
		refresh);

	sp_reg_read(anx78xx, TX_P0, SP_DP_VIDEO_CTRL_REG, &val);

	val &= SP_COLOR_F_MASK;
	val >>= SP_COLOR_F_SHIFT;
	if (val == HDMI_COLORSPACE_RGB)
		dev_dbg(dev, "colorspace: RGB");
	else if (val == HDMI_COLORSPACE_YUV422)
		dev_dbg(dev, "colorspace: YCbCr422");
	else if (val == HDMI_COLORSPACE_YUV444)
		dev_dbg(dev, "colorspace: YCbCr444");

	sp_reg_read(anx78xx, TX_P0, SP_DP_VIDEO_CTRL_REG, &val);

	val &= SP_BPC_MASK;
	val >>= SP_BPC_SHIFT;
	if (val  == SP_BPC_6BITS)
		dev_dbg(dev, "6 BPC\n");
	else if (val == SP_BPC_8BITS)
		dev_dbg(dev, "8 BPC\n");
	else if (val == SP_BPC_10BITS)
		dev_dbg(dev, "10 BPC\n");
	else if (val == SP_BPC_12BITS)
		dev_dbg(dev, "12 BPC\n");

	if (is_anx_dongle(anx78xx)) {
		/*
		 * 00503h - 005FFh: RESERVED for Branch Device vendor-specific
		 * usage
		 */
		sp_dp_read_bytes_from_dpcd(anx78xx, 0x0523, 1, &val);
		dev_dbg(dev, "Analogix Dongle FW Ver %.2x\n", val & 0x7f);
	}

	dev_dbg(dev, "\n**************************************************\n");
}

static void sp_hdcp_repeater_reauth(struct anx78xx *anx78xx)
{
	u8 val, ctrl, status;
	struct device *dev = &anx78xx->client->dev;

	msleep(50);
	sp_reg_read(anx78xx, RX_P1, SP_RX_HDCP_STATUS_REG, &val);

	if (val & SP_AUTH_EN) {
		sp_reg_read(anx78xx, TX_P0, SP_HDCP_CTRL0_REG, &ctrl);
		if (ctrl & SP_HARD_AUTH_EN) {
			sp_reg_read(anx78xx, TX_P0, SP_TX_HDCP_STATUS_REG,
				    &status);
			if (!(status & SP_AUTHEN_PASS) &&
			    (status & SP_AUTH_FAIL)) {
				dev_dbg(dev, "clean HDCP and re-auth\n");
				sp.repeater_state = HDCP_ERROR;
			}
		} else {
			dev_dbg(dev, "repeater mode, enable HW HDCP\n");
			sp.repeater_state = HDCP_ERROR;
		}
	}

	sp_reg_read(anx78xx, TX_P0, SP_HDCP_CTRL0_REG, &ctrl);
	sp_reg_read(anx78xx, TX_P0, SP_TX_HDCP_STATUS_REG, &status);

	if ((ctrl == SP_HDCP_FUNCTION_ENABLED) && (status & SP_AUTH_FAIL)) {
		dev_dbg(dev, "HDCP encryption failure 0x%02x\n", status);
		sp.repeater_state = HDCP_ERROR;
	}

	if (sp.repeater_state == HDCP_ERROR) {
		sp_clean_hdcp_status(anx78xx);
		msleep(50);
		/* Clear HDCP AUTH interrupt */
		sp_reg_set_bits(anx78xx, TX_P2, SP_COMMON_INT_STATUS_BASE + 2,
				SP_HDCP_AUTH_DONE);
		sp_hw_hdcp_enable(anx78xx);
		sp.repeater_state = HDCP_DOING;
	}
}

/**
 * sp_main_process(): SlimPort Main Process
 *
 * SlimPort Main Process States:
 * 0. Cable pulled out.
 *    - Power down device.
 * 1. SlimPort plug
 *    - Power on device
 * 2. SlimPort initialization
 *    - Enable the power supply for downstream
 *    - Power on the register access
 *    - Initialize the related registers
 * 3. Sink connection
 *     - Get the cable type (HDMI, VGA or MyDP)
 *     - Check the connection with downstream
 * 4. Read EDID
 *    - Read partial EDID data to decide whether to re-read entire EDID
 *    - EDID read
 *    - Parse EDID to get the video bandwidth
 * 5. Link training
 *    - Check the downstream bandwidth
 *    - Hardware link training
 * 6. Video output
 *    - Verify that input video is stable
 *    - Order by the input video to calculate the bandwidth
 *    - Set AVI packet, bit-mapping, color depth, etc.
 * 7. HDCP authentication
 *    - Verify that HDCP is supported
 *    - Enable hardware HDCP
 * 8. Audio output
 *    - Automatic audio M valu adjustment
 *    - Configure audio multichannel
 *    - Set audio packet
 * 9. Playback
 *    - The normal system working state
 *
 */
bool sp_main_process(struct anx78xx *anx78xx)
{
	/*
	 * Process the interrupts
	 */
	if (sp.tx_system_state > STATE_WAITING_CABLE_PLUG) {
		/* Interrupt receiver */
		sp_int_receiver(anx78xx);

		/* Task handler */
		sp_system_isr_handler(anx78xx);

		/* If device; supports HDCP repeater function re-auth */
		if (sp_hdcp_repeater_mode(anx78xx))
			sp_hdcp_repeater_reauth(anx78xx);
/*
		if (!sp.hdcp_enabled) {
			sp_set_system_state(anx78xx, STATE_HDCP_AUTH);
			sp.hdcp_enabled = true;
		}
*/
	}

	/*
	 * SlimPort State Process
	 */
	switch (sp.tx_system_state) {
	case STATE_CABLE_UNPLUGGED:
		sp_variable_init(anx78xx);
		anx78xx_poweroff(anx78xx);
		return false;
	case STATE_WAITING_CABLE_PLUG:
		sp_variable_init(anx78xx);
		anx78xx_poweron(anx78xx);
		sp.tx_system_state = STATE_SP_INITIALIZED;
		sp_print_system_state(anx78xx, sp.tx_system_state);
	/* fallthrough */
	case STATE_SP_INITIALIZED:
		sp_initialization(anx78xx);
		sp.tx_system_state = STATE_SINK_CONNECTION;
		sp_print_system_state(anx78xx, sp.tx_system_state);
	/* fallthrough */
	case STATE_SINK_CONNECTION:
		if (sp_get_dp_connection(anx78xx)) {
			sp.tx_system_state = STATE_PARSE_EDID;
			sp_print_system_state(anx78xx, sp.tx_system_state);
		} else {
			break;
		}
	/* fallthrough */
	case STATE_PARSE_EDID:
		if (sp_edid_process(anx78xx)) {
			sp.tx_system_state = STATE_LINK_TRAINING;
			sp_print_system_state(anx78xx, sp.tx_system_state);
		} else {
			break;
		}
	/* fallthrough */
	case STATE_LINK_TRAINING:
		if (sp_link_training(anx78xx)) {
			sp.tx_system_state = STATE_VIDEO_OUTPUT;
			sp_print_system_state(anx78xx, sp.tx_system_state);
		} else {
			break;
		}
	/* fallthrough */
	case STATE_VIDEO_OUTPUT:
		if (sp_config_video_output(anx78xx)) {
			sp.tx_system_state = STATE_HDCP_AUTH;
			sp_print_system_state(anx78xx, sp.tx_system_state);
		} else {
			break;
		}
	/* fallthrough */
	case STATE_HDCP_AUTH:
		if (!sp_hdcp_repeater_mode(anx78xx)) {
			if (sp_hdcp_process(anx78xx)) {
				sp.tx_system_state = STATE_AUDIO_OUTPUT;
				sp_print_system_state(anx78xx,
						      sp.tx_system_state);
			} else {
				break;
			}
		} else {
			sp.tx_system_state = STATE_AUDIO_OUTPUT;
		}
	/* fallthrough */
	case STATE_AUDIO_OUTPUT:
		if (sp_config_audio_output(anx78xx)) {
			sp.tx_system_state = STATE_PLAY_BACK;
			sp_print_system_state(anx78xx, sp.tx_system_state);
		} else {
			break;
		}
	/* fallthrough */
	case STATE_PLAY_BACK:
	default:
		break;
	}

	return true;
}

/**
 * sp_system_init(): System initialization
 *
 * @anx78xx: SlimPort device.
 *
 * A value of zero will be returned on success, a negative errno will
 * be returned in error cases.
 */
int sp_system_init(struct anx78xx *anx78xx)
{
	struct device *dev = &anx78xx->client->dev;
	u16 id;
	u8 idh = 0, idl = 0;
	int i;

	sp_variable_init(anx78xx);

	anx78xx_poweron(anx78xx);

	/* check chip id */
	sp_reg_read(anx78xx, TX_P2, SP_DEVICE_IDL_REG, &idl);
	sp_reg_read(anx78xx, TX_P2, SP_DEVICE_IDH_REG, &idh);
	id = idl | (idh << 8);

	for (i = 0; i < ARRAY_SIZE(chipid_list); i++) {
		if (id == chipid_list[i])
			return 0;
	}

	anx78xx_poweroff(anx78xx);

	dev_err(dev, "failed to detect ANX%x\n", id);

	return -ENODEV;
}
