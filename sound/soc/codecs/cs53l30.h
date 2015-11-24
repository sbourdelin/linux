/*
 * ALSA SoC CS53L30 codec driver
 *
 * Copyright 2015 Cirrus Logic, Inc.
 *
 * Author: Paul Handrigan <Paul.Handrigan@cirrus.com>,
 *         Tim Howe <Tim.Howe@cirrus.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

#ifndef __CS53L30_H__
#define __CS53L30_H__

/* I2C Registers */
#define CS53L30_DEVID_AB	0x01	 /* Device ID A & B [RO]. */
#define CS53L30_DEVID_CD	0x02     /* Device ID C & D [RO]. */
#define CS53L30_DEVID_E		0x03     /* Device ID E [RO]. */
#define CS53L30_REVID		0x05     /* Revision ID [RO]. */
#define CS53L30_PWRCTL		0x06     /* Power Control. */
#define CS53L30_MCLKCTL		0x07     /* MCLK Control. */
#define CS53L30_INT_SR_CTL	0x08     /* Internal Sample Rate Control. */
#define CS53L30_MICBIAS_CTL	0x0A     /* Mic Bias Control. */
#define CS53L30_ASPCFG_CTL	0x0C     /* ASP Config Control. */
#define CS53L30_ASP1_CTL	0x0D     /* ASP1 Control. */
#define CS53L30_ASP1_TDMTX_CTL1	0x0E     /* ASP1 TDM TX Control 1 */
#define CS53L30_ASP1_TDMTX_CTL2	0x0F     /* ASP1 TDM TX Control 2 */
#define CS53L30_ASP1_TDMTX_CTL3	0x10     /* ASP1 TDM TX Control 3 */
#define CS53L30_ASP1_TDMTX_CTL4	0x11     /* ASP1 TDM TX Control 4 */
#define CS53L30_ASP1_TDMTX_EN1	0x12     /* ASP1 TDM TX Enable 1 */
#define CS53L30_ASP1_TDMTX_EN2	0x13     /* ASP1 TDM TX Enable 2 */
#define CS53L30_ASP1_TDMTX_EN3	0x14     /* ASP1 TDM TX Enable 3 */
#define CS53L30_ASP1_TDMTX_EN4	0x15     /* ASP1 TDM TX Enable 4 */
#define CS53L30_ASP1_TDMTX_EN5	0x16     /* ASP1 TDM TX Enable 5 */
#define CS53L30_ASP1_TDMTX_EN6	0x17     /* ASP1 TDM TX Enable 6 */
#define CS53L30_ASP2_CTL	0x18     /* ASP2 Control. */
#define CS53L30_SFT_RAMP	0x1A     /* Soft Ramp Control. */
#define CS53L30_LRCLK_CTL1	0x1B     /* LRCLK Control 1. */
#define CS53L30_LRCLK_CTL2	0x1C     /* LRCLK Control 2. */
#define CS53L30_MUTEP_CTL1	0x1F     /* Mute Pin Control 1. */
#define CS53L30_MUTEP_CTL2	0x20     /* Mute Pin Control 2. */
#define CS53L30_INBIAS_CTL1	0x21     /* Input Bias Control 1. */
#define CS53L30_INBIAS_CTL2	0x22     /* Input Bias Control 2. */
#define CS53L30_DMIC1_STR_CTL   0x23     /* DMIC1 Stereo Control. */
#define CS53L30_DMIC2_STR_CTL   0x24     /* DMIC2 Stereo Control. */
#define CS53L30_ADCDMIC1_CTL1   0x25     /* ADC1/DMIC1 Control 1. */
#define CS53L30_ADCDMIC1_CTL2   0x26     /* ADC1/DMIC1 Control 2. */
#define CS53L30_ADC1_CTL3	0x27     /* ADC1 Control 3. */
#define CS53L30_ADC1_NG_CTL	0x28     /* ADC1 Noise Gate Control. */
#define CS53L30_ADC1A_AFE_CTL	0x29     /* ADC1A AFE Control. */
#define CS53L30_ADC1B_AFE_CTL	0x2A     /* ADC1B AFE Control. */
#define CS53L30_ADC1A_DIG_VOL	0x2B     /* ADC1A Digital Volume. */
#define CS53L30_ADC1B_DIG_VOL	0x2C     /* ADC1B Digital Volume. */
#define CS53L30_ADCDMIC2_CTL1   0x2D     /* ADC2/DMIC2 Control 1. */
#define CS53L30_ADCDMIC2_CTL2   0x2E     /* ADC2/DMIC2 Control 2. */
#define CS53L30_ADC2_CTL3	0x2F     /* ADC2 Control 3. */
#define CS53L30_ADC2_NG_CTL	0x30     /* ADC2 Noise Gate Control. */
#define CS53L30_ADC2A_AFE_CTL	0x31     /* ADC2A AFE Control. */
#define CS53L30_ADC2B_AFE_CTL	0x32     /* ADC2B AFE Control. */
#define CS53L30_ADC2A_DIG_VOL	0x33     /* ADC2A Digital Volume. */
#define CS53L30_ADC2B_DIG_VOL	0x34     /* ADC2B Digital Volume. */
#define CS53L30_INT_MASK	0x35     /* Interrupt Mask. */
#define CS53L30_IS		0x36     /* Interrupt Status. */
#define CS53L30_MAX_REGISTER	0x36

/* Device ID */
#define CS53L30_DEVID		0x53A30

/* PDN_DONE Poll Maximum
 * If soft ramp is set it will take much longer to power down
 * the system.
 */
#define PDN_POLL_MAX		900

/* Bitfield Definitions */

/* CS53L30_PWRCTL */
#define PDN_ULP			(1 << 7)
#define PDN_LP			(1 << 6)
#define DISCHARGE_FILT		(1 << 5)
#define THMS_PDN		(1 << 4)

/* CS53L30_MCLKCTL */
#define MCLK_DIS		(1 << 7)
#define MCLK_INT_SCALE		(1 << 6)
#define DMIC_DRIVE		(1 << 5)
#define MCLK_DIV		(3 << 2)
#define MCLK_DIV_DFLT		(1 << 2)
#define SYNC_EN			(1 << 1)

/* CS53L30_INT_SR_CTL */
#define INTERNAL_FS_RATIO	(1 << 4)
#define INTERNAL_FS_DFLT	(7 << 2)
#define MCLK_19MHZ_EN		(1 << 0)

/* CS53L30_MICBIAS_CTL */
#define MIC4_BIAS_PDN		(1 << 7)
#define MIC3_BIAS_PDN		(1 << 6)
#define MIC2_BIAS_PDN		(1 << 5)
#define MIC1_BIAS_PDN		(1 << 4)
#define VP_MIN			(1 << 2)
#define MIC_BIAS_CTRL		(3 << 0)
#define MIC_BIAS_ALL_PDN	0xF0
#define MIC_BIAS_DFLT		(MIC_BIAS_ALL_PDN | VP_MIN)

/* CS53L30_ASPCFG_CTL */
#define ASP_MS			(1 << 7)
#define ASP_SCLK_INV		(1 << 4)
#define ASP_RATE_48K		(3 << 2)
#define ASP_RATE		0x0F
#define ASP_CNFG_MASK		0xF0

/* CS53L30_ASP1_CTL  */
#define ASP1_TDM_PDN		(1 << 7)
#define ASP1_SDOUT_PDN		(1 << 6)
#define ASP1_3ST		(1 << 5)
#define SHIFT_LEFT		(1 << 4)
#define ASP1_DRIVE		(1 << 0)
#define ASP1_3ST_VAL(x)		((x) << 5)

/* CS53L30_ASP1_TDMTX_CTL */
#define ASP1_CHX_TX_STATE	(1 << 7)
#define ASP1_CHX_TX_LOC		0x3F
#define ASP1_CHX_TX_DFLT_SLT47	0x2F
#define ASP_TX_DISABLED		0x00

/* CS53L30_ASP2_CTL  */
#define ASP2_SDOUT_PDN		(1 << 6)
#define ASP2_DRIVE		(1 << 0)
#define ASP2_CTRL_DFLT		0x00

/* CS53L30_SFT_RAMP  */
#define DIGSFT			(1 << 5)
#define SFT_RMP_DFLT		0x00

/* CS53L30_LRCLK_CTL2  */
#define LRCK_50_NPW		(1 << 3)
#define LRCK_TPWH		(7 << 0)
#define LRCK_CTLX_DFLT		0x00

/* CS53L30_MUTEP_CTL  */
#define MUTE_PDN_ULP		(1 << 7)
#define MUTE_PDN_LP		(1 << 6)
#define MUTE_M4B_PDN		(1 << 4)
#define MUTE_M3B_PDN		(1 << 3)
#define MUTE_M2B_PDN		(1 << 2)
#define MUTE_M1B_PDN		(1 << 1)
#define MUTE_MB_ALL_PDN		(1 << 0)
#define MUTEP_CTRL1_DFLT	0x00

/* CS53L30_MUTEP_CTL2  */
#define MUTE_PIN_POLARITY	(1 << 7)
#define MUTE_ASP_TDM_PDN	(1 << 6)
#define MUTE_ASP_SDOUT2_PDN	(1 << 5)
#define MUTE_ASP_SDOUT1_PDN	(1 << 4)
#define MUTE_ADC2B_PDN		(1 << 3)
#define MUTE_ADC2A_PDN		(1 << 2)
#define MUTE_ADC1B_PDN		(1 << 1)
#define MUTE_ADC1A_PDN		(1 << 0)

/* CS53L30_INBIAS_CTL1 */
#define IN4M_BIAS		(3 << 6)
#define IN4P_BIAS		(3 << 4)
#define IN3M_BIAS		(3 << 2)
#define IN3P_BIAS		(3 << 0)

/* CS53L30_INBIAS_CTL2 */
#define IN2M_BIAS		(3 << 6)
#define IN2P_BIAS		(3 << 4)
#define IN1M_BIAS		(3 << 2)
#define IN1P_BIAS		(3 << 0)
#define INBIAS_CTLX_DFLT	0xAA

/* CS53L30_DMIC1_STR_CTL */
#define DMIC1_STEREO_ENB	(1 << 5)
#define DMIC1_STEREO_DFLT	0xA8

/* CS53L30_DMIC2_STR_CTL */
#define DMIC2_STEREO_EN		(1 << 5)
#define DMIC2_STEREO_DFLT	0xEC

/* CS53L30_ADCDMIC1_CTL1  */
#define ADC1B_PDN		(1 << 7)
#define ADC1A_PDN		(1 << 6)
#define DMIC1_PDN		(1 << 2)
#define DMIC1_SCLK_DIV		(1 << 1)
#define CH_TYPE			(1 << 0)
#define DMIC1_ON_CH_AB_IN	(CH_TYPE)
#define DMIC1_ON_CH_A_IN	(ADC1B_PDN | CH_TYPE)
#define DMIC1_ON_CH_B_IN	(ADC1A_PDN | CH_TYPE)
#define ADC1_ON_CH_AB_IN	(DMIC1_PDN)
#define ADC1_ON_CH_A_IN		(ADC1B_PDN | DMIC1_PDN)
#define ADC1_ON_CH_B_IN		(ADC1A_PDN | DMIC1_PDN)
#define DMIC1_OFF_ADC1_OFF	(ADC1A_PDN | ADC1B_PDN | DMIC1_PDN)
#define ADC1_DMIC1_PDN_MASK	0xFF

/* CS53L30_ADCDMIC1_CTL2  */
#define ADC1_NOTCH_DIS		(1 << 7)
#define ADC1B_INV		(1 << 5)
#define ADC1A_INV		(1 << 4)
#define ADC1B_DIG_BOOST		(1 << 1)
#define ADC1A_DIG_BOOST		(1 << 0)
#define ADC1_DMIC1_CTL2_DFLT	0x00

/* CS53L30_ADC1_CTL3  */
#define ADC1_HPF_EN		(1 << 3)
#define ADC1_HPF_CF		(3 << 1)
#define ADC1_NG_ALL		(1 << 0)

/* CS53L30_ADC1_NG_CTL  */
#define ADC1B_NG		(1 << 7)
#define ADC1A_NG		(1 << 6)
#define ADC1_NG_BOOST		(1 << 5)
#define ADC1_NG_THRESH		(7 << 2)
#define ADC1_NG_DELAY		(3 << 0)
#define ADCX_ZERO_DFLT		0x00

/* CS53L30_ADC1A_AFE_CTL */
#define ADC1A_PREAMP		(3 << 6)
#define ADC1A_PGA_VOL		0x3F

/* CS53L30_ADC1B_AFE_CTL */
#define ADC1B_PREAMP		(3 << 6)
#define ADC1B_PGA_VOL		0x3F

/* CS53L30_ADCXX_DIG_VOL */
#define MUTE_DIG_OUT		(1 << 7)

/* CS53L30_ADCDMIC2_CTL1  */
#define ADC2B_PDN		(1 << 7)
#define ADC2A_PDN		(1 << 6)
#define DMIC2_PDN		(1 << 2)
#define DMIC2_CLKDIV		(1 << 1)
#define DMIC2_ON_CH_AB_IN	0x00 /* CH_TYPE must = 1 */
#define DMIC2_ON_CH_A_IN	(ADC2B_PDN) /* CH_TYPE must = 1 */
#define DMIC2_ON_CH_B_IN	(ADC2A_PDN) /* CH_TYPE must = 1 */
#define ADC2_ON_CH_AB_IN	(DMIC2_PDN) /* CH_TYPE must = 0 */
#define ADC2_ON_CH_A_IN		(ADC2B_PDN | DMIC2_PDN) /* CH_TYPE must = 0 */
#define ADC2_ON_CH_B_IN		(ADC2A_PDN | DMIC2_PDN) /* CH_TYPE must = 0 */
#define DMIC2_OFF_ADC2_OFF	(ADC2A_PDN | ADC2B_PDN | DMIC2_PDN)

/* CS53L30_ADCDMIC2_CTL2  */
#define ADC2_NOTCH_DIS		(1 << 7)
#define ADC2B_INV		(1 << 5)
#define ADC2A_INV		(1 << 4)
#define ADC2B_DIG_BOOST		(1 << 1)
#define ADC2A_DIG_BOOST		(1 << 0)

/* CS53L30_ADC2_CTL3  */
#define ADC2_HPF_EN		(1 << 3)
#define ADC2_HPF_CF		(3 << 1)
#define ADC2_NG_ALL		(1 << 0)

/* CS53L30_INT */
#define PDN_DONE		(1 << 7)
#define THMS_TRIP		(1 << 6)
#define SYNC_DONE		(1 << 5)
#define ADC2B_OVFL		(1 << 4)
#define ADC2A_OVFL		(1 << 3)
#define ADC1B_OVFL		(1 << 2)
#define ADC1A_OVFL		(1 << 1)
#define MUTE_PIN		(1 << 0)
#define DEVICE_INT_MASK		0xFF

/* Serial Ports */
#define CS53L30_ASP1		0
#define CS53L30_ASP2		1

#endif	/* __CS53L30_H__ */
