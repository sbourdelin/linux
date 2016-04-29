/*
 * mt2701-afe-common.h  --  Mediatek 2701 audio driver definitions
 *
 * Copyright (c) 2016 MediaTek Inc.
 * Author: Garlic Tseng <garlic.tseng@mediatek.com>
 *             Koro Chen <koro.chen@mediatek.com>
 *             Sascha Hauer <s.hauer@pengutronix.de>
 *             Hidalgo Huang <hidalgo.huang@mediatek.com>
 *             Ir Lian <ir.lian@mediatek.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#ifndef _MT_2701_AFE_COMMON_H_
#define _MT_2701_AFE_COMMON_H_
#include <sound/soc.h>
#include <linux/clk.h>
#include <linux/regmap.h>
#include "mt2701-reg.h"

#define MT2701_STREAM_DIR_NUM (SNDRV_PCM_STREAM_LAST + 1)
#define MT2701_PLL_DOMAIN_0_RATE	98304000
#define MT2701_PLL_DOMAIN_1_RATE	90316800
#define MT2701_AUD_TOP_AUD_MUX1_DIV_RATE (MT2701_PLL_DOMAIN_0_RATE / 2)
#define MT2701_AUD_TOP_AUD_MUX2_DIV_RATE (MT2701_PLL_DOMAIN_1_RATE / 2)

enum {
	MT2701_I2S_1,
	MT2701_I2S_2,
	MT2701_I2S_3,
	MT2701_I2S_4,
	MT2701_I2S_NUM,
};

enum {
	MT2701_MEMIF_1,
	MT2701_MEMIF_2,
	MT2701_MEMIF_3,
	MT2701_MEMIF_4,
	MT2701_MEMIF_5,
	MT2701_MEMIF_SINGLE_NUM,
	MT2701_MEMIF_M = MT2701_MEMIF_SINGLE_NUM,
	MT2701_MEMIF_BT,
	MT2701_MEMIF_NUM,
	MT2701_IO_I2S = MT2701_MEMIF_NUM,
	MT2701_IO_2ND_I2S,
	MT2701_IO_3RD_I2S,
	MT2701_IO_4TH_I2S,
	MT2701_IO_5TH_I2S,
	MT2701_IO_6TH_I2S,
	MT2701_IO_MRG,
};

enum {
	MT2701_IRQ_ASYS_START,
	MT2701_IRQ_ASYS_IRQ1 = MT2701_IRQ_ASYS_START,
	MT2701_IRQ_ASYS_IRQ2,
	MT2701_IRQ_ASYS_IRQ3,
	MT2701_IRQ_ASYS_END,
};

enum {
	DIV_ID_MCLK_TO_BCK,
	DIV_ID_BCK_TO_LRCK,
};

/*2701 clock def*/
enum audio_system_clock_type {
	MT2701_AUD_INFRA_SYS_AUDIO,
	MT2701_AUD_TOP_AUD_MUX1_SEL,
	MT2701_AUD_TOP_AUD_MUX2_SEL,
	MT2701_AUD_TOP_AUD_MUX1_DIV,
	MT2701_AUD_TOP_AUD_MUX2_DIV,
	MT2701_AUD_TOP_AUD_48K_TIMING,
	MT2701_AUD_TOP_AUD_44K_TIMING,
	MT2701_AUD_TOP_AUDPLL_MUX_SEL,
	MT2701_AUD_TOP_APLL_SEL,
	MT2701_AUD_TOP_AUD1PLL_98M,
	MT2701_AUD_TOP_AUD2PLL_90M,
	MT2701_AUD_TOP_HADDS2PLL_98M,
	MT2701_AUD_TOP_HADDS2PLL_294M,
	MT2701_AUD_TOP_AUDPLL,
	MT2701_AUD_TOP_AUDPLL_D4,
	MT2701_AUD_TOP_AUDPLL_D8,
	MT2701_AUD_TOP_AUDPLL_D16,
	MT2701_AUD_TOP_AUDPLL_D24,
	MT2701_AUD_TOP_AUDINTBUS,
	MT2701_AUD_CLK_26M,
	MT2701_AUD_TOP_SYSPLL1_D4,
	MT2701_AUD_TOP_AUD_K1_SRC_SEL,
	MT2701_AUD_TOP_AUD_K2_SRC_SEL,
	MT2701_AUD_TOP_AUD_K3_SRC_SEL,
	MT2701_AUD_TOP_AUD_K4_SRC_SEL,
	MT2701_AUD_TOP_AUD_K5_SRC_SEL,
	MT2701_AUD_TOP_AUD_K6_SRC_SEL,
	MT2701_AUD_TOP_AUD_K1_SRC_DIV,
	MT2701_AUD_TOP_AUD_K2_SRC_DIV,
	MT2701_AUD_TOP_AUD_K3_SRC_DIV,
	MT2701_AUD_TOP_AUD_K4_SRC_DIV,
	MT2701_AUD_TOP_AUD_K5_SRC_DIV,
	MT2701_AUD_TOP_AUD_K6_SRC_DIV,
	MT2701_AUD_TOP_AUD_I2S1_MCLK,
	MT2701_AUD_TOP_AUD_I2S2_MCLK,
	MT2701_AUD_TOP_AUD_I2S3_MCLK,
	MT2701_AUD_TOP_AUD_I2S4_MCLK,
	MT2701_AUD_TOP_AUD_I2S5_MCLK,
	MT2701_AUD_TOP_AUD_I2S6_MCLK,
	MT2701_AUD_TOP_ASM_M_SEL,
	MT2701_AUD_TOP_ASM_H_SEL,
	MT2701_AUD_TOP_UNIVPLL2_D4,
	MT2701_AUD_TOP_UNIVPLL2_D2,
	MT2701_AUD_TOP_SYSPLL_D5,
	MT2701_CLOCK_NUM
};

static const unsigned int mt2701_afe_backup_list[] = {
	AUDIO_TOP_CON0,
	AUDIO_TOP_CON4,
	AUDIO_TOP_CON5,
	ASYS_TOP_CON,
	AFE_CONN0,
	AFE_CONN1,
	AFE_CONN2,
	AFE_CONN3,
	AFE_CONN15,
	AFE_CONN16,
	AFE_CONN17,
	AFE_CONN18,
	AFE_CONN19,
	AFE_CONN20,
	AFE_CONN21,
	AFE_CONN22,
	AFE_DAC_CON0,
	AFE_MEMIF_PBUF_SIZE,
};

struct mt2701_afe;
struct snd_pcm_substream;

struct mt2701_afe_memif_data {
	int id;
	const char *name;
	int reg_ofs_base;
	int reg_ofs_cur;
	int fs_reg;
	int fs_shift;
	int mono_reg;
	int mono_shift;
	int enable_shift;
	int hd_reg;
	int hd_shift;
	int agent_disable_shift;
};

struct mt2701_afe_irq_data {
	int irq_id;
	int irq_cnt_reg;
	int irq_cnt_shift;
	int irq_cnt_maskbit;
	int irq_fs_reg;
	int irq_fs_shift;
	int irq_fs_maskbit;
	int irq_en_reg;
	int irq_en_shift;
	int irq_occupy;
};

struct mt2701_afe_irq {
	const struct mt2701_afe_irq_data *irq_data;
	int irq_occupyed;
	struct mt2701_afe_memif *memif;
	void (*isr)(struct mt2701_afe *afe, struct mt2701_afe_memif *memif);
};

struct mt2701_afe_memif {
	unsigned int phys_buf_addr;
	int buffer_size;
	struct snd_pcm_substream *substream;
	const struct mt2701_afe_memif_data *data;
	struct mt2701_afe_irq *irq;
};

struct mt2701_i2s_data {
	int i2s_ctrl_reg;
	int i2s_pwn_shift;
	int i2s_asrc_fs_shift;
	int i2s_asrc_fs_mask;
};

enum mt2701_i2s_dir {
	I2S_OUT,
	I2S_IN,
	I2S_DIR_NUM,
};

struct mt2701_i2s_path {
	int dai_id;
	int mclk_rate;
	int div_mclk_to_bck;
	int div_bck_to_lrck;
	int format;
	snd_pcm_format_t stream_fmt;
	int on[I2S_DIR_NUM];
	int occupied[I2S_DIR_NUM];
	const struct mt2701_i2s_data *i2s_data[2];
};

struct mt2701_afe {
	void __iomem *base_addr;
	struct device *dev;
	struct regmap *regmap;
	struct mt2701_afe_memif memif[MT2701_MEMIF_NUM][MT2701_STREAM_DIR_NUM];
	struct clk *clocks[MT2701_CLOCK_NUM];
	struct mt2701_afe_irq irqs[MT2701_IRQ_ASYS_END];
	struct mt2701_i2s_path i2s_path[MT2701_I2S_NUM];
	bool mrg_enable[MT2701_STREAM_DIR_NUM];
	unsigned int backup_regs[ARRAY_SIZE(mt2701_afe_backup_list)];
	bool suspended;
};

#endif
