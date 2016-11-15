/*
 * Copyright © 2014 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Author: Shobhit Kumar <shobhit.kumar@intel.com>
 *
 */

#include <drm/drmP.h>
#include <drm/drm_crtc.h>
#include <drm/drm_edid.h>
#include <drm/i915_drm.h>
#include <drm/drm_panel.h>
#include <linux/gpio.h>
#include <linux/slab.h>
#include <video/mipi_display.h>
#include <asm/intel-mid.h>
#include <video/mipi_display.h>
#include "i915_drv.h"
#include "intel_drv.h"
#include "intel_dsi.h"

struct vbt_panel {
	struct drm_panel panel;
	struct intel_dsi *intel_dsi;
};

static inline struct vbt_panel *to_vbt_panel(struct drm_panel *panel)
{
	return container_of(panel, struct vbt_panel, panel);
}

#define MIPI_TRANSFER_MODE_SHIFT	0
#define MIPI_VIRTUAL_CHANNEL_SHIFT	1
#define MIPI_PORT_SHIFT			3

#define PREPARE_CNT_MAX		0x3F
#define EXIT_ZERO_CNT_MAX	0x3F
#define CLK_ZERO_CNT_MAX	0xFF
#define TRAIL_CNT_MAX		0x1F

#define NS_KHZ_RATIO 1000000

/* base offsets for gpio pads */
#define VLV_GPIO_NC_0_HV_DDI0_HPD	0x4130
#define VLV_GPIO_NC_1_HV_DDI0_DDC_SDA	0x4120
#define VLV_GPIO_NC_2_HV_DDI0_DDC_SCL	0x4110
#define VLV_GPIO_NC_3_PANEL0_VDDEN	0x4140
#define VLV_GPIO_NC_4_PANEL0_BKLTEN	0x4150
#define VLV_GPIO_NC_5_PANEL0_BKLTCTL	0x4160
#define VLV_GPIO_NC_6_HV_DDI1_HPD	0x4180
#define VLV_GPIO_NC_7_HV_DDI1_DDC_SDA	0x4190
#define VLV_GPIO_NC_8_HV_DDI1_DDC_SCL	0x4170
#define VLV_GPIO_NC_9_PANEL1_VDDEN	0x4100
#define VLV_GPIO_NC_10_PANEL1_BKLTEN	0x40E0
#define VLV_GPIO_NC_11_PANEL1_BKLTCTL	0x40F0

#define VLV_GPIO_PCONF0(base_offset)	(base_offset)
#define VLV_GPIO_PAD_VAL(base_offset)	((base_offset) + 8)

struct gpio_map {
	u16 base_offset;
	bool init;
};

static struct gpio_map vlv_gpio_table[] = {
	{ VLV_GPIO_NC_0_HV_DDI0_HPD },
	{ VLV_GPIO_NC_1_HV_DDI0_DDC_SDA },
	{ VLV_GPIO_NC_2_HV_DDI0_DDC_SCL },
	{ VLV_GPIO_NC_3_PANEL0_VDDEN },
	{ VLV_GPIO_NC_4_PANEL0_BKLTEN },
	{ VLV_GPIO_NC_5_PANEL0_BKLTCTL },
	{ VLV_GPIO_NC_6_HV_DDI1_HPD },
	{ VLV_GPIO_NC_7_HV_DDI1_DDC_SDA },
	{ VLV_GPIO_NC_8_HV_DDI1_DDC_SCL },
	{ VLV_GPIO_NC_9_PANEL1_VDDEN },
	{ VLV_GPIO_NC_10_PANEL1_BKLTEN },
	{ VLV_GPIO_NC_11_PANEL1_BKLTCTL },
};

#define CHV_GPIO_IDX_START_N		0
#define CHV_GPIO_IDX_START_E		73
#define CHV_GPIO_IDX_START_SW		100
#define CHV_GPIO_IDX_START_SE		198

#define CHV_VBT_MAX_PINS_PER_FMLY	15

#define CHV_GPIO_PAD_CFG0(f, i)		(0x4400 + (f) * 0x400 + (i) * 8)
#define  CHV_GPIO_GPIOEN		(1 << 15)
#define  CHV_GPIO_GPIOCFG_GPIO		(0 << 8)
#define  CHV_GPIO_GPIOCFG_GPO		(1 << 8)
#define  CHV_GPIO_GPIOCFG_GPI		(2 << 8)
#define  CHV_GPIO_GPIOCFG_HIZ		(3 << 8)
#define  CHV_GPIO_GPIOTXSTATE(state)	((!!(state)) << 1)

#define CHV_GPIO_PAD_CFG1(f, i)		(0x4400 + (f) * 0x400 + (i) * 8 + 4)
#define  CHV_GPIO_CFGLOCK		(1 << 31)

#define BXT_HV_DDI0_DDC_SDA_PIN		187
#define BXT_HV_DDI0_DDC_SCL_PIN		188
#define BXT_HV_DDI1_DDC_SDA_PIN		189
#define BXT_HV_DDI1_DDC_SCL_PIN		190
#define BXT_DBI_SDA_PIN			191
#define BXT_DBI_SCL_PIN			192
#define BXT_PANEL0_VDDEN_PIN		193
#define BXT_PANEL0_BKLTEN_PIN		194
#define BXT_PANEL0_BKLTCTL_PIN		195
#define BXT_PANEL1_VDDEN_PIN		196
#define BXT_PANEL1_BKLTEN_PIN		197
#define BXT_PANEL1_BKLTCTL_PIN		198
#define BXT_DBI_CSX_PIN			199
#define BXT_DBI_RESX_PIN		200
#define BXT_GP_INTD_DSI_TE1_PIN		201
#define BXT_GP_INTD_DSI_TE2_PIN		202
#define BXT_USB_OC0_B_PIN		203
#define BXT_USB_OC1_B_PIN		204
#define BXT_MEX_WAKE0_B_PIN		205
#define BXT_MEX_WAKE1_B_PIN		206
#define BXT_EMMC0_CLK_PIN		156
#define BXT_EMMC0_D0_PIN		157
#define BXT_EMMC0_D1_PIN		158
#define BXT_EMMC0_D2_PIN		159
#define BXT_EMMC0_D3_PIN		160
#define BXT_EMMC0_D4_PIN		161
#define BXT_EMMC0_D5_PIN		162
#define BXT_EMMC0_D6_PIN		163
#define BXT_EMMC0_D7_PIN		164
#define BXT_EMMC0_CMD_PIN		165
#define BXT_SDIO_CLK_PIN		166
#define BXT_SDIO_D0_PIN			167
#define BXT_SDIO_D1_PIN			168
#define BXT_SDIO_D2_PIN			169
#define BXT_SDIO_D3_PIN			170
#define BXT_SDIO_CMD_PIN		171
#define BXT_SDCARD_CLK_PIN		172
#define BXT_SDCARD_D0_PIN		173
#define BXT_SDCARD_D1_PIN		174
#define BXT_SDCARD_D2_PIN		175
#define BXT_SDCARD_D3_PIN		176
#define BXT_SDCARD_CD_B_PIN		177
#define BXT_SDCARD_CMD_PIN		178
#define BXT_SDCARD_LVL_CLK_FB_PIN	179
#define BXT_SDCARD_LVL_CMD_DIR_PIN	180
#define BXT_SDCARD_LVL_DAT_DIR_PIN	181
#define BXT_EMMC0_STROBE_PIN		182
#define BXT_SDIO_PWR_DOWN_B_PIN		183
#define BXT_SDCARD_PWR_DOWN_B_PIN	184
#define BXT_SDCARD_LVL_SEL_PIN		185
#define BXT_SDCARD_LVL_WP_PIN		186
#define BXT_LPSS_I2C0_SDA_PIN		124
#define BXT_LPSS_I2C0_SCL_PIN		125
#define BXT_LPSS_I2C1_SDA_PIN		126
#define BXT_LPSS_I2C1_SCL_PIN		127
#define BXT_LPSS_I2C2_SDA_PIN		128
#define BXT_LPSS_I2C2_SCL_PIN		129
#define BXT_LPSS_I2C3_SDA_PIN		130
#define BXT_LPSS_I2C3_SCL_PIN		131
#define BXT_LPSS_I2C4_SDA_PIN		132
#define BXT_LPSS_I2C4_SCL_PIN		133
#define BXT_LPSS_I2C5_SDA_PIN		134
#define BXT_LPSS_I2C5_SCL_PIN		135
#define BXT_LPSS_I2C6_SDA_PIN		136
#define BXT_LPSS_I2C6_SCL_PIN		137
#define BXT_LPSS_I2C7_SDA_PIN		138
#define BXT_LPSS_I2C7_SCL_PIN		139
#define BXT_ISH_I2C0_SDA_PIN		140
#define BXT_ISH_I2C0_SCL_PIN		141
#define BXT_ISH_I2C1_SDA_PIN		142
#define BXT_ISH_I2C1_SCL_PIN		143
#define BXT_ISH_I2C2_SDA_PIN		144
#define BXT_ISH_I2C2_SCL_PIN		145
#define BXT_ISH_GPIO_0_PIN		146
#define BXT_ISH_GPIO_1_PIN		147
#define BXT_ISH_GPIO_2_PIN		148
#define BXT_ISH_GPIO_3_PIN		149
#define BXT_ISH_GPIO_4_PIN		150
#define BXT_ISH_GPIO_5_PIN		151
#define BXT_ISH_GPIO_6_PIN		152
#define BXT_ISH_GPIO_7_PIN		153
#define BXT_ISH_GPIO_8_PIN		154
#define BXT_ISH_GPIO_9_PIN		155
#define BXT_AVS_I2S1_MCLK_PIN		74
#define BXT_AVS_I2S1_BCLK_PIN		75
#define BXT_AVS_I2S1_WS_SYNC_PIN	76
#define BXT_AVS_I2S1_SDI_PIN		77
#define BXT_AVS_I2S1_SDO_PIN		78
#define BXT_AVS_M_CLK_A1_PIN		79
#define BXT_AVS_M_CLK_B1_PIN		80
#define BXT_AVS_M_DATA_1_PIN		81
#define BXT_AVS_M_CLK_AB2_PIN		82
#define BXT_AVS_M_DATA_2_PIN		83
#define BXT_AVS_I2S2_MCLK_PIN		84
#define BXT_AVS_I2S2_BCLK_PIN		85
#define BXT_AVS_I2S2_WS_SYNC_PIN	86
#define BXT_AVS_I2S2_SDI_PIN		87
#define BXT_AVS_I2S2_SDO_PIN		88
#define BXT_AVS_I2S3_BCLK_PIN		89
#define BXT_AVS_I2S3_WS_SYNC_PIN	90
#define BXT_AVS_I2S3_SDI_PIN		91
#define BXT_AVS_I2S3_SDO_PIN		92
#define BXT_AVS_I2S4_BCLK_PIN		93
#define BXT_AVS_I2S4_WS_SYNC_PIN	94
#define BXT_AVS_I2S4_SDI_PIN		95
#define BXT_AVS_I2S4_SDO_PIN		96
#define BXT_FST_SPI_CS0_B_PIN		97
#define BXT_FST_SPI_CS1_B_PIN		98
#define BXT_FST_SPI_MOSI_IO0_PIN	99
#define BXT_FST_SPI_MISO_IO1_PIN	100
#define BXT_FST_SPI_IO2_PIN		101
#define BXT_FST_SPI_IO3_PIN		102
#define BXT_FST_SPI_CLK_PIN		103
#define BXT_GP_SSP_0_CLK_PIN		104
#define BXT_GP_SSP_0_FS0_PIN		105
#define BXT_GP_SSP_0_FS1_PIN		106
#define BXT_GP_SSP_0_FS2_PIN		107
#define BXT_GP_SSP_0_RXD_PIN		109
#define BXT_GP_SSP_0_TXD_PIN		110
#define BXT_GP_SSP_1_CLK_PIN		111
#define BXT_GP_SSP_1_FS0_PIN		112
#define BXT_GP_SSP_1_FS1_PIN		113
#define BXT_GP_SSP_1_FS2_PIN		114
#define BXT_GP_SSP_1_FS3_PIN		115
#define BXT_GP_SSP_1_RXD_PIN		116
#define BXT_GP_SSP_1_TXD_PIN		117
#define BXT_GP_SSP_2_CLK_PIN		118
#define BXT_GP_SSP_2_FS0_PIN		119
#define BXT_GP_SSP_2_FS1_PIN		120
#define BXT_GP_SSP_2_FS2_PIN		121
#define BXT_GP_SSP_2_RXD_PIN		122
#define BXT_GP_SSP_2_TXD_PIN		123
#define BXT_TRACE_0_CLK_VNN_PIN		0
#define BXT_TRACE_0_DATA0_VNN_PIN	1
#define BXT_TRACE_0_DATA1_VNN_PIN	2
#define BXT_TRACE_0_DATA2_VNN_PIN	3
#define BXT_TRACE_0_DATA3_VNN_PIN	4
#define BXT_TRACE_0_DATA4_VNN_PIN	5
#define BXT_TRACE_0_DATA5_VNN_PIN	6
#define BXT_TRACE_0_DATA6_VNN_PIN	7
#define BXT_TRACE_0_DATA7_VNN_PIN	8
#define BXT_TRACE_1_CLK_VNN_PIN		9
#define BXT_TRACE_1_DATA0_VNN_PIN	10
#define BXT_TRACE_1_DATA1_VNN_PIN	11
#define BXT_TRACE_1_DATA2_VNN_PIN	12
#define BXT_TRACE_1_DATA3_VNN_PIN	13
#define BXT_TRACE_1_DATA4_VNN_PIN	14
#define BXT_TRACE_1_DATA5_VNN_PIN	15
#define BXT_TRACE_1_DATA6_VNN_PIN	16
#define BXT_TRACE_1_DATA7_VNN_PIN	17
#define BXT_TRACE_2_CLK_VNN_PIN		18
#define BXT_TRACE_2_DATA0_VNN_PIN	19
#define BXT_TRACE_2_DATA1_VNN_PIN	20
#define BXT_TRACE_2_DATA2_VNN_PIN	21
#define BXT_TRACE_2_DATA3_VNN_PIN	22
#define BXT_TRACE_2_DATA4_VNN_PIN	23
#define BXT_TRACE_2_DATA5_VNN_PIN	24
#define BXT_TRACE_2_DATA6_VNN_PIN	25
#define BXT_TRACE_2_DATA7_VNN_PIN	26
#define BXT_TRIGOUT_0_PIN		27
#define BXT_TRIGOUT_1_PIN		28
#define BXT_TRIGIN_0_PIN		29
#define BXT_SEC_TCK_PIN			30
#define BXT_SEC_TDI_PIN			31
#define BXT_SEC_TMS_PIN			32
#define BXT_SEC_TDO_PIN			33
#define BXT_PWM0_PIN			34
#define BXT_PWM1_PIN			35
#define BXT_PWM2_PIN			36
#define BXT_PWM3_PIN			37
#define BXT_LPSS_UART0_RXD_PIN		38
#define BXT_LPSS_UART0_TXD_PIN		39
#define BXT_LPSS_UART0_RTS_B_PIN	40
#define BXT_LPSS_UART0_CTS_B_PIN	41
#define BXT_LPSS_UART1_RXD_PIN		42
#define BXT_LPSS_UART1_TXD_PIN		43
#define BXT_LPSS_UART1_RTS_B_PIN	44
#define BXT_LPSS_UART1_CTS_B_PIN	45
#define BXT_LPSS_UART2_RXD_PIN		46
#define BXT_LPSS_UART2_TXD_PIN		47
#define BXT_LPSS_UART2_RTS_B_PIN	48
#define BXT_LPSS_UART2_CTS_B_PIN	49
#define BXT_ISH_UART0_RXD_PIN		50
#define BXT_ISH_UART0_TXD_PIN		51
#define BXT_ISH_UART0_RTS_B_PIN		52
#define BXT_ISH_UART0_CTS_B_PIN		53
#define BXT_ISH_UART1_RXD_PIN		54
#define BXT_ISH_UART1_TXD_PIN		55
#define BXT_ISH_UART1_RTS_B_PIN		56
#define BXT_ISH_UART1_CTS_B_PIN		57
#define BXT_ISH_UART2_RXD_PIN		58
#define BXT_ISH_UART2_TXD_PIN		59
#define BXT_ISH_UART2_RTS_B_PIN		60
#define BXT_ISH_UART2_CTS_B_PIN		61
#define BXT_GP_CAMERASB00_PIN		62
#define BXT_GP_CAMERASB01_PIN		63
#define BXT_GP_CAMERASB02_PIN		64
#define BXT_GP_CAMERASB03_PIN		65
#define BXT_GP_CAMERASB04_PIN		66
#define BXT_GP_CAMERASB05_PIN		67
#define BXT_GP_CAMERASB06_PIN		68
#define BXT_GP_CAMERASB07_PIN		69
#define BXT_GP_CAMERASB08_PIN		70
#define BXT_GP_CAMERASB09_PIN		71
#define BXT_GP_CAMERASB10_PIN		72
#define BXT_GP_CAMERASB11_PIN		73

#define BXT_HV_DDI0_DDC_SDA_OFFSET	264
#define BXT_HV_DDI0_DDC_SCL_OFFSET	265
#define BXT_HV_DDI1_DDC_SDA_OFFSET	266
#define BXT_HV_DDI1_DDC_SCL_OFFSET	267
#define BXT_DBI_SDA_OFFSET		268
#define BXT_DBI_SCL_OFFSET		269
#define BXT_PANEL0_VDDEN_OFFSET		270
#define BXT_PANEL0_BKLTEN_OFFSET	271
#define BXT_PANEL0_BKLTCTL_OFFSET	272
#define BXT_PANEL1_VDDEN_OFFSET		273
#define BXT_PANEL1_BKLTEN_OFFSET	274
#define BXT_PANEL1_BKLTCTL_OFFSET	275
#define BXT_DBI_CSX_OFFSET		276
#define BXT_DBI_RESX_OFFSET		277
#define BXT_GP_INTD_DSI_TE1_OFFSET	278
#define BXT_GP_INTD_DSI_TE2_OFFSET	279
#define BXT_USB_OC0_B_OFFSET		280
#define BXT_USB_OC1_B_OFFSET		281
#define BXT_MEX_WAKE0_B_OFFSET		282
#define BXT_MEX_WAKE1_B_OFFSET		283
#define BXT_EMMC0_CLK_OFFSET		284
#define BXT_EMMC0_D0_OFFSET		285
#define BXT_EMMC0_D1_OFFSET		286
#define BXT_EMMC0_D2_OFFSET		287
#define BXT_EMMC0_D3_OFFSET		288
#define BXT_EMMC0_D4_OFFSET		289
#define BXT_EMMC0_D5_OFFSET		290
#define BXT_EMMC0_D6_OFFSET		291
#define BXT_EMMC0_D7_OFFSET		292
#define BXT_EMMC0_CMD_OFFSET		293
#define BXT_SDIO_CLK_OFFSET		294
#define BXT_SDIO_D0_OFFSET		295
#define BXT_SDIO_D1_OFFSET		296
#define BXT_SDIO_D2_OFFSET		297
#define BXT_SDIO_D3_OFFSET		298
#define BXT_SDIO_CMD_OFFSET		299
#define BXT_SDCARD_CLK_OFFSET		300
#define BXT_SDCARD_D0_OFFSET		301
#define BXT_SDCARD_D1_OFFSET		302
#define BXT_SDCARD_D2_OFFSET		303
#define BXT_SDCARD_D3_OFFSET		304
#define BXT_SDCARD_CD_B_OFFSET		305
#define BXT_SDCARD_CMD_OFFSET		306
#define BXT_SDCARD_LVL_CLK_FB_OFFSET	307
#define BXT_SDCARD_LVL_CMD_DIR_OFFSET	308
#define BXT_SDCARD_LVL_DAT_DIR_OFFSET	309
#define BXT_EMMC0_STROBE_OFFSET		310
#define BXT_SDIO_PWR_DOWN_B_OFFSET	311
#define BXT_SDCARD_PWR_DOWN_B_OFFSET	312
#define BXT_SDCARD_LVL_SEL_OFFSET	313
#define BXT_SDCARD_LVL_WP_OFFSET	314
#define BXT_LPSS_I2C0_SDA_OFFSET	315
#define BXT_LPSS_I2C0_SCL_OFFSET	316
#define BXT_LPSS_I2C1_SDA_OFFSET	317
#define BXT_LPSS_I2C1_SCL_OFFSET	318
#define BXT_LPSS_I2C2_SDA_OFFSET	319
#define BXT_LPSS_I2C2_SCL_OFFSET	320
#define BXT_LPSS_I2C3_SDA_OFFSET	321
#define BXT_LPSS_I2C3_SCL_OFFSET	322
#define BXT_LPSS_I2C4_SDA_OFFSET	323
#define BXT_LPSS_I2C4_SCL_OFFSET	324
#define BXT_LPSS_I2C5_SDA_OFFSET	325
#define BXT_LPSS_I2C5_SCL_OFFSET	326
#define BXT_LPSS_I2C6_SDA_OFFSET	327
#define BXT_LPSS_I2C6_SCL_OFFSET	328
#define BXT_LPSS_I2C7_SDA_OFFSET	329
#define BXT_LPSS_I2C7_SCL_OFFSET	330
#define BXT_ISH_I2C0_SDA_OFFSET		331
#define BXT_ISH_I2C0_SCL_OFFSET		332
#define BXT_ISH_I2C1_SDA_OFFSET		333
#define BXT_ISH_I2C1_SCL_OFFSET		334
#define BXT_ISH_I2C2_SDA_OFFSET		335
#define BXT_ISH_I2C2_SCL_OFFSET		336
#define BXT_ISH_GPIO_0_OFFSET		337
#define BXT_ISH_GPIO_1_OFFSET		338
#define BXT_ISH_GPIO_2_OFFSET		339
#define BXT_ISH_GPIO_3_OFFSET		340
#define BXT_ISH_GPIO_4_OFFSET		341
#define BXT_ISH_GPIO_5_OFFSET		342
#define BXT_ISH_GPIO_6_OFFSET		343
#define BXT_ISH_GPIO_7_OFFSET		344
#define BXT_ISH_GPIO_8_OFFSET		345
#define BXT_ISH_GPIO_9_OFFSET		346
#define BXT_AVS_I2S1_MCLK_OFFSET	378
#define BXT_AVS_I2S1_BCLK_OFFSET	379
#define BXT_AVS_I2S1_WS_SYNC_OFFSET	380
#define BXT_AVS_I2S1_SDI_OFFSET		381
#define BXT_AVS_I2S1_SDO_OFFSET		382
#define BXT_AVS_M_CLK_A1_OFFSET		383
#define BXT_AVS_M_CLK_B1_OFFSET		384
#define BXT_AVS_M_DATA_1_OFFSET		385
#define BXT_AVS_M_CLK_AB2_OFFSET	386
#define BXT_AVS_M_DATA_2_OFFSET		387
#define BXT_AVS_I2S2_MCLK_OFFSET	388
#define BXT_AVS_I2S2_BCLK_OFFSET	389
#define BXT_AVS_I2S2_WS_SYNC_OFFSET	390
#define BXT_AVS_I2S2_SDI_OFFSET		391
#define BXT_AVS_I2S2_SDO_OFFSET		392
#define BXT_AVS_I2S3_BCLK_OFFSET	393
#define BXT_AVS_I2S3_WS_SYNC_OFFSET	394
#define BXT_AVS_I2S3_SDI_OFFSET		395
#define BXT_AVS_I2S3_SDO_OFFSET		396
#define BXT_AVS_I2S4_BCLK_OFFSET	397
#define BXT_AVS_I2S4_WS_SYNC_OFFSET	398
#define BXT_AVS_I2S4_SDI_OFFSET		399
#define BXT_AVS_I2S4_SDO_OFFSET		400
#define BXT_FST_SPI_CS0_B_OFFSET	402
#define BXT_FST_SPI_CS1_B_OFFSET	403
#define BXT_FST_SPI_MOSI_IO0_OFFSET	404
#define BXT_FST_SPI_MISO_IO1_OFFSET	405
#define BXT_FST_SPI_IO2_OFFSET		406
#define BXT_FST_SPI_IO3_OFFSET		407
#define BXT_FST_SPI_CLK_OFFSET		408
#define BXT_GP_SSP_0_CLK_OFFSET		410
#define BXT_GP_SSP_0_FS0_OFFSET		411
#define BXT_GP_SSP_0_FS1_OFFSET		412
#define BXT_GP_SSP_0_FS2_OFFSET		413
#define BXT_GP_SSP_0_RXD_OFFSET		414
#define BXT_GP_SSP_0_TXD_OFFSET		415
#define BXT_GP_SSP_1_CLK_OFFSET		416
#define BXT_GP_SSP_1_FS0_OFFSET		417
#define BXT_GP_SSP_1_FS1_OFFSET		418
#define BXT_GP_SSP_1_FS2_OFFSET		419
#define BXT_GP_SSP_1_FS3_OFFSET		420
#define BXT_GP_SSP_1_RXD_OFFSET		421
#define BXT_GP_SSP_1_TXD_OFFSET		422
#define BXT_GP_SSP_2_CLK_OFFSET		423
#define BXT_GP_SSP_2_FS0_OFFSET		424
#define BXT_GP_SSP_2_FS1_OFFSET		425
#define BXT_GP_SSP_2_FS2_OFFSET		426
#define BXT_GP_SSP_2_RXD_OFFSET		427
#define BXT_GP_SSP_2_TXD_OFFSET		428
#define BXT_TRACE_0_CLK_VNN_OFFSET	429
#define BXT_TRACE_0_DATA0_VNN_OFFSET	430
#define BXT_TRACE_0_DATA1_VNN_OFFSET	431
#define BXT_TRACE_0_DATA2_VNN_OFFSET	432
#define BXT_TRACE_0_DATA3_VNN_OFFSET	433
#define BXT_TRACE_0_DATA4_VNN_OFFSET	434
#define BXT_TRACE_0_DATA5_VNN_OFFSET	435
#define BXT_TRACE_0_DATA6_VNN_OFFSET	436
#define BXT_TRACE_0_DATA7_VNN_OFFSET	437
#define BXT_TRACE_1_CLK_VNN_OFFSET	438
#define BXT_TRACE_1_DATA0_VNN_OFFSET	439
#define BXT_TRACE_1_DATA1_VNN_OFFSET	440
#define BXT_TRACE_1_DATA2_VNN_OFFSET	441
#define BXT_TRACE_1_DATA3_VNN_OFFSET	442
#define BXT_TRACE_1_DATA4_VNN_OFFSET	443
#define BXT_TRACE_1_DATA5_VNN_OFFSET	444
#define BXT_TRACE_1_DATA6_VNN_OFFSET	445
#define BXT_TRACE_1_DATA7_VNN_OFFSET	446
#define BXT_TRACE_2_CLK_VNN_OFFSET	447
#define BXT_TRACE_2_DATA0_VNN_OFFSET	448
#define BXT_TRACE_2_DATA1_VNN_OFFSET	449
#define BXT_TRACE_2_DATA2_VNN_OFFSET	450
#define BXT_TRACE_2_DATA3_VNN_OFFSET	451
#define BXT_TRACE_2_DATA4_VNN_OFFSET	452
#define BXT_TRACE_2_DATA5_VNN_OFFSET	453
#define BXT_TRACE_2_DATA6_VNN_OFFSET	454
#define BXT_TRACE_2_DATA7_VNN_OFFSET	455
#define BXT_TRIGOUT_0_OFFSET		456
#define BXT_TRIGOUT_1_OFFSET		457
#define BXT_TRIGIN_0_OFFSET		458
#define BXT_SEC_TCK_OFFSET		459
#define BXT_SEC_TDI_OFFSET		460
#define BXT_SEC_TMS_OFFSET		461
#define BXT_SEC_TDO_OFFSET		462
#define BXT_PWM0_OFFSET			463
#define BXT_PWM1_OFFSET			464
#define BXT_PWM2_OFFSET			465
#define BXT_PWM3_OFFSET			466
#define BXT_LPSS_UART0_RXD_OFFSET	467
#define BXT_LPSS_UART0_TXD_OFFSET	468
#define BXT_LPSS_UART0_RTS_B_OFFSET	469
#define BXT_LPSS_UART0_CTS_B_OFFSET	470
#define BXT_LPSS_UART1_RXD_OFFSET	471
#define BXT_LPSS_UART1_TXD_OFFSET	472
#define BXT_LPSS_UART1_RTS_B_OFFSET	473
#define BXT_LPSS_UART1_CTS_B_OFFSET	474
#define BXT_LPSS_UART2_RXD_OFFSET	475
#define BXT_LPSS_UART2_TXD_OFFSET	476
#define BXT_LPSS_UART2_RTS_B_OFFSET	477
#define BXT_LPSS_UART2_CTS_B_OFFSET	478
#define BXT_ISH_UART0_RXD_OFFSET	479
#define BXT_ISH_UART0_TXD_OFFSET	480
#define BXT_ISH_UART0_RTS_B_OFFSET	481
#define BXT_ISH_UART0_CTS_B_OFFSET	482
#define BXT_ISH_UART1_RXD_OFFSET	483
#define BXT_ISH_UART1_TXD_OFFSET	484
#define BXT_ISH_UART1_RTS_B_OFFSET	485
#define BXT_ISH_UART1_CTS_B_OFFSET	486
#define BXT_ISH_UART2_RXD_OFFSET	487
#define BXT_ISH_UART2_TXD_OFFSET	488
#define BXT_ISH_UART2_RTS_B_OFFSET	489
#define BXT_ISH_UART2_CTS_B_OFFSET	490
#define BXT_GP_CAMERASB00_OFFSET	491
#define BXT_GP_CAMERASB01_OFFSET	492
#define BXT_GP_CAMERASB02_OFFSET	493
#define BXT_GP_CAMERASB03_OFFSET	494
#define BXT_GP_CAMERASB04_OFFSET	495
#define BXT_GP_CAMERASB05_OFFSET	496
#define BXT_GP_CAMERASB06_OFFSET	497
#define BXT_GP_CAMERASB07_OFFSET	498
#define BXT_GP_CAMERASB08_OFFSET	499
#define BXT_GP_CAMERASB09_OFFSET	500
#define BXT_GP_CAMERASB10_OFFSET	501
#define BXT_GP_CAMERASB11_OFFSET	502

struct bxt_gpio_map {
	u8 gpio_index;
	u16 gpio_number;
	bool requested;
};

/* XXX: take out everything that is not related to DSI display */
static struct bxt_gpio_map bxt_gpio_table[] = {
	{ BXT_HV_DDI0_DDC_SDA_PIN, BXT_HV_DDI0_DDC_SDA_OFFSET },
	{ BXT_HV_DDI0_DDC_SCL_PIN, BXT_HV_DDI0_DDC_SCL_OFFSET },
	{ BXT_HV_DDI1_DDC_SDA_PIN, BXT_HV_DDI1_DDC_SDA_OFFSET },
	{ BXT_HV_DDI1_DDC_SCL_PIN, BXT_HV_DDI1_DDC_SCL_OFFSET },
	{ BXT_DBI_SDA_PIN, BXT_DBI_SDA_OFFSET },
	{ BXT_DBI_SCL_PIN, BXT_DBI_SCL_OFFSET },
	{ BXT_PANEL0_VDDEN_PIN, BXT_PANEL0_VDDEN_OFFSET },
	{ BXT_PANEL0_BKLTEN_PIN, BXT_PANEL0_BKLTEN_OFFSET },
	{ BXT_PANEL0_BKLTCTL_PIN, BXT_PANEL0_BKLTCTL_OFFSET },
	{ BXT_PANEL1_VDDEN_PIN, BXT_PANEL1_VDDEN_OFFSET },
	{ BXT_PANEL1_BKLTEN_PIN, BXT_PANEL1_BKLTEN_OFFSET },
	{ BXT_PANEL1_BKLTCTL_PIN, BXT_PANEL1_BKLTCTL_OFFSET },
	{ BXT_DBI_CSX_PIN, BXT_DBI_CSX_OFFSET },
	{ BXT_DBI_RESX_PIN, BXT_DBI_RESX_OFFSET },
	{ BXT_GP_INTD_DSI_TE1_PIN, BXT_GP_INTD_DSI_TE1_OFFSET },
	{ BXT_GP_INTD_DSI_TE2_PIN, BXT_GP_INTD_DSI_TE2_OFFSET },
	{ BXT_USB_OC0_B_PIN, BXT_USB_OC0_B_OFFSET },
	{ BXT_USB_OC1_B_PIN, BXT_USB_OC1_B_OFFSET },
	{ BXT_MEX_WAKE0_B_PIN, BXT_MEX_WAKE0_B_OFFSET },
	{ BXT_MEX_WAKE1_B_PIN, BXT_MEX_WAKE1_B_OFFSET },
	{ BXT_EMMC0_CLK_PIN, BXT_EMMC0_CLK_OFFSET },
	{ BXT_EMMC0_D0_PIN, BXT_EMMC0_D0_OFFSET },
	{ BXT_EMMC0_D1_PIN, BXT_EMMC0_D1_OFFSET },
	{ BXT_EMMC0_D2_PIN, BXT_EMMC0_D2_OFFSET },
	{ BXT_EMMC0_D3_PIN, BXT_EMMC0_D3_OFFSET },
	{ BXT_EMMC0_D4_PIN, BXT_EMMC0_D4_OFFSET },
	{ BXT_EMMC0_D5_PIN, BXT_EMMC0_D5_OFFSET },
	{ BXT_EMMC0_D6_PIN, BXT_EMMC0_D6_OFFSET },
	{ BXT_EMMC0_D7_PIN, BXT_EMMC0_D7_OFFSET },
	{ BXT_EMMC0_CMD_PIN, BXT_EMMC0_CMD_OFFSET },
	{ BXT_SDIO_CLK_PIN, BXT_SDIO_CLK_OFFSET },
	{ BXT_SDIO_D0_PIN, BXT_SDIO_D0_OFFSET },
	{ BXT_SDIO_D1_PIN, BXT_SDIO_D1_OFFSET },
	{ BXT_SDIO_D2_PIN, BXT_SDIO_D2_OFFSET },
	{ BXT_SDIO_D3_PIN, BXT_SDIO_D3_OFFSET },
	{ BXT_SDIO_CMD_PIN, BXT_SDIO_CMD_OFFSET },
	{ BXT_SDCARD_CLK_PIN, BXT_SDCARD_CLK_OFFSET },
	{ BXT_SDCARD_D0_PIN, BXT_SDCARD_D0_OFFSET },
	{ BXT_SDCARD_D1_PIN, BXT_SDCARD_D1_OFFSET },
	{ BXT_SDCARD_D2_PIN, BXT_SDCARD_D2_OFFSET },
	{ BXT_SDCARD_D3_PIN, BXT_SDCARD_D3_OFFSET },
	{ BXT_SDCARD_CD_B_PIN, BXT_SDCARD_CD_B_OFFSET },
	{ BXT_SDCARD_CMD_PIN, BXT_SDCARD_CMD_OFFSET },
	{ BXT_SDCARD_LVL_CLK_FB_PIN, BXT_SDCARD_LVL_CLK_FB_OFFSET },
	{ BXT_SDCARD_LVL_CMD_DIR_PIN, BXT_SDCARD_LVL_CMD_DIR_OFFSET },
	{ BXT_SDCARD_LVL_DAT_DIR_PIN, BXT_SDCARD_LVL_DAT_DIR_OFFSET },
	{ BXT_EMMC0_STROBE_PIN, BXT_EMMC0_STROBE_OFFSET },
	{ BXT_SDIO_PWR_DOWN_B_PIN, BXT_SDIO_PWR_DOWN_B_OFFSET },
	{ BXT_SDCARD_PWR_DOWN_B_PIN, BXT_SDCARD_PWR_DOWN_B_OFFSET },
	{ BXT_SDCARD_LVL_SEL_PIN, BXT_SDCARD_LVL_SEL_OFFSET },
	{ BXT_SDCARD_LVL_WP_PIN, BXT_SDCARD_LVL_WP_OFFSET },
	{ BXT_LPSS_I2C0_SDA_PIN, BXT_LPSS_I2C0_SDA_OFFSET },
	{ BXT_LPSS_I2C0_SCL_PIN, BXT_LPSS_I2C0_SCL_OFFSET },
	{ BXT_LPSS_I2C1_SDA_PIN, BXT_LPSS_I2C1_SDA_OFFSET },
	{ BXT_LPSS_I2C1_SCL_PIN, BXT_LPSS_I2C1_SCL_OFFSET },
	{ BXT_LPSS_I2C2_SDA_PIN, BXT_LPSS_I2C2_SDA_OFFSET },
	{ BXT_LPSS_I2C2_SCL_PIN, BXT_LPSS_I2C2_SCL_OFFSET },
	{ BXT_LPSS_I2C3_SDA_PIN, BXT_LPSS_I2C3_SDA_OFFSET },
	{ BXT_LPSS_I2C3_SCL_PIN, BXT_LPSS_I2C3_SCL_OFFSET },
	{ BXT_LPSS_I2C4_SDA_PIN, BXT_LPSS_I2C4_SDA_OFFSET },
	{ BXT_LPSS_I2C4_SCL_PIN, BXT_LPSS_I2C4_SCL_OFFSET },
	{ BXT_LPSS_I2C5_SDA_PIN, BXT_LPSS_I2C5_SDA_OFFSET },
	{ BXT_LPSS_I2C5_SCL_PIN, BXT_LPSS_I2C5_SCL_OFFSET },
	{ BXT_LPSS_I2C6_SDA_PIN, BXT_LPSS_I2C6_SDA_OFFSET },
	{ BXT_LPSS_I2C6_SCL_PIN, BXT_LPSS_I2C6_SCL_OFFSET },
	{ BXT_LPSS_I2C7_SDA_PIN, BXT_LPSS_I2C7_SDA_OFFSET },
	{ BXT_LPSS_I2C7_SCL_PIN, BXT_LPSS_I2C7_SCL_OFFSET },
	{ BXT_ISH_I2C0_SDA_PIN, BXT_ISH_I2C0_SDA_OFFSET },
	{ BXT_ISH_I2C0_SCL_PIN, BXT_ISH_I2C0_SCL_OFFSET },
	{ BXT_ISH_I2C1_SDA_PIN, BXT_ISH_I2C1_SDA_OFFSET },
	{ BXT_ISH_I2C1_SCL_PIN, BXT_ISH_I2C1_SCL_OFFSET },
	{ BXT_ISH_I2C2_SDA_PIN, BXT_ISH_I2C2_SDA_OFFSET },
	{ BXT_ISH_I2C2_SCL_PIN, BXT_ISH_I2C2_SCL_OFFSET },
	{ BXT_ISH_GPIO_0_PIN, BXT_ISH_GPIO_0_OFFSET },
	{ BXT_ISH_GPIO_1_PIN, BXT_ISH_GPIO_1_OFFSET },
	{ BXT_ISH_GPIO_2_PIN, BXT_ISH_GPIO_2_OFFSET },
	{ BXT_ISH_GPIO_3_PIN, BXT_ISH_GPIO_3_OFFSET },
	{ BXT_ISH_GPIO_4_PIN, BXT_ISH_GPIO_4_OFFSET },
	{ BXT_ISH_GPIO_5_PIN, BXT_ISH_GPIO_5_OFFSET },
	{ BXT_ISH_GPIO_6_PIN, BXT_ISH_GPIO_6_OFFSET },
	{ BXT_ISH_GPIO_7_PIN, BXT_ISH_GPIO_7_OFFSET },
	{ BXT_ISH_GPIO_8_PIN, BXT_ISH_GPIO_8_OFFSET },
	{ BXT_ISH_GPIO_9_PIN, BXT_ISH_GPIO_9_OFFSET },
	{ BXT_AVS_I2S1_MCLK_PIN, BXT_AVS_I2S1_MCLK_OFFSET },
	{ BXT_AVS_I2S1_BCLK_PIN, BXT_AVS_I2S1_BCLK_OFFSET },
	{ BXT_AVS_I2S1_WS_SYNC_PIN, BXT_AVS_I2S1_WS_SYNC_OFFSET },
	{ BXT_AVS_I2S1_SDI_PIN, BXT_AVS_I2S1_SDI_OFFSET },
	{ BXT_AVS_I2S1_SDO_PIN, BXT_AVS_I2S1_SDO_OFFSET },
	{ BXT_AVS_M_CLK_A1_PIN, BXT_AVS_M_CLK_A1_OFFSET },
	{ BXT_AVS_M_CLK_B1_PIN, BXT_AVS_M_CLK_B1_OFFSET },
	{ BXT_AVS_M_DATA_1_PIN, BXT_AVS_M_DATA_1_OFFSET },
	{ BXT_AVS_M_CLK_AB2_PIN, BXT_AVS_M_CLK_AB2_OFFSET },
	{ BXT_AVS_M_DATA_2_PIN, BXT_AVS_M_DATA_2_OFFSET },
	{ BXT_AVS_I2S2_MCLK_PIN, BXT_AVS_I2S2_MCLK_OFFSET },
	{ BXT_AVS_I2S2_BCLK_PIN, BXT_AVS_I2S2_BCLK_OFFSET },
	{ BXT_AVS_I2S2_WS_SYNC_PIN, BXT_AVS_I2S2_WS_SYNC_OFFSET },
	{ BXT_AVS_I2S2_SDI_PIN, BXT_AVS_I2S2_SDI_OFFSET },
	{ BXT_AVS_I2S2_SDO_PIN, BXT_AVS_I2S2_SDO_OFFSET },
	{ BXT_AVS_I2S3_BCLK_PIN, BXT_AVS_I2S3_BCLK_OFFSET },
	{ BXT_AVS_I2S3_WS_SYNC_PIN, BXT_AVS_I2S3_WS_SYNC_OFFSET },
	{ BXT_AVS_I2S3_SDI_PIN, BXT_AVS_I2S3_SDI_OFFSET },
	{ BXT_AVS_I2S3_SDO_PIN, BXT_AVS_I2S3_SDO_OFFSET },
	{ BXT_AVS_I2S4_BCLK_PIN, BXT_AVS_I2S4_BCLK_OFFSET },
	{ BXT_AVS_I2S4_WS_SYNC_PIN, BXT_AVS_I2S4_WS_SYNC_OFFSET },
	{ BXT_AVS_I2S4_SDI_PIN, BXT_AVS_I2S4_SDI_OFFSET },
	{ BXT_AVS_I2S4_SDO_PIN, BXT_AVS_I2S4_SDO_OFFSET },
	{ BXT_FST_SPI_CS0_B_PIN, BXT_FST_SPI_CS0_B_OFFSET },
	{ BXT_FST_SPI_CS1_B_PIN, BXT_FST_SPI_CS1_B_OFFSET },
	{ BXT_FST_SPI_MOSI_IO0_PIN, BXT_FST_SPI_MOSI_IO0_OFFSET },
	{ BXT_FST_SPI_MISO_IO1_PIN, BXT_FST_SPI_MISO_IO1_OFFSET },
	{ BXT_FST_SPI_IO2_PIN, BXT_FST_SPI_IO2_OFFSET },
	{ BXT_FST_SPI_IO3_PIN, BXT_FST_SPI_IO3_OFFSET },
	{ BXT_FST_SPI_CLK_PIN, BXT_FST_SPI_CLK_OFFSET },
	{ BXT_GP_SSP_0_CLK_PIN, BXT_GP_SSP_0_CLK_OFFSET },
	{ BXT_GP_SSP_0_FS0_PIN, BXT_GP_SSP_0_FS0_OFFSET },
	{ BXT_GP_SSP_0_FS1_PIN, BXT_GP_SSP_0_FS1_OFFSET },
	{ BXT_GP_SSP_0_FS2_PIN, BXT_GP_SSP_0_FS2_OFFSET },
	{ BXT_GP_SSP_0_RXD_PIN, BXT_GP_SSP_0_RXD_OFFSET },
	{ BXT_GP_SSP_0_TXD_PIN, BXT_GP_SSP_0_TXD_OFFSET },
	{ BXT_GP_SSP_1_CLK_PIN, BXT_GP_SSP_1_CLK_OFFSET },
	{ BXT_GP_SSP_1_FS0_PIN, BXT_GP_SSP_1_FS0_OFFSET },
	{ BXT_GP_SSP_1_FS1_PIN, BXT_GP_SSP_1_FS1_OFFSET },
	{ BXT_GP_SSP_1_FS2_PIN, BXT_GP_SSP_1_FS2_OFFSET },
	{ BXT_GP_SSP_1_FS3_PIN, BXT_GP_SSP_1_FS3_OFFSET },
	{ BXT_GP_SSP_1_RXD_PIN, BXT_GP_SSP_1_RXD_OFFSET },
	{ BXT_GP_SSP_1_TXD_PIN, BXT_GP_SSP_1_TXD_OFFSET },
	{ BXT_GP_SSP_2_CLK_PIN, BXT_GP_SSP_2_CLK_OFFSET },
	{ BXT_GP_SSP_2_FS0_PIN, BXT_GP_SSP_2_FS0_OFFSET },
	{ BXT_GP_SSP_2_FS1_PIN, BXT_GP_SSP_2_FS1_OFFSET },
	{ BXT_GP_SSP_2_FS2_PIN, BXT_GP_SSP_2_FS2_OFFSET },
	{ BXT_GP_SSP_2_RXD_PIN, BXT_GP_SSP_2_RXD_OFFSET },
	{ BXT_GP_SSP_2_TXD_PIN, BXT_GP_SSP_2_TXD_OFFSET },
	{ BXT_TRACE_0_CLK_VNN_PIN, BXT_TRACE_0_CLK_VNN_OFFSET },
	{ BXT_TRACE_0_DATA0_VNN_PIN, BXT_TRACE_0_DATA0_VNN_OFFSET },
	{ BXT_TRACE_0_DATA1_VNN_PIN, BXT_TRACE_0_DATA1_VNN_OFFSET },
	{ BXT_TRACE_0_DATA2_VNN_PIN, BXT_TRACE_0_DATA2_VNN_OFFSET },
	{ BXT_TRACE_0_DATA3_VNN_PIN, BXT_TRACE_0_DATA3_VNN_OFFSET },
	{ BXT_TRACE_0_DATA4_VNN_PIN, BXT_TRACE_0_DATA4_VNN_OFFSET },
	{ BXT_TRACE_0_DATA5_VNN_PIN, BXT_TRACE_0_DATA5_VNN_OFFSET },
	{ BXT_TRACE_0_DATA6_VNN_PIN, BXT_TRACE_0_DATA6_VNN_OFFSET },
	{ BXT_TRACE_0_DATA7_VNN_PIN, BXT_TRACE_0_DATA7_VNN_OFFSET },
	{ BXT_TRACE_1_CLK_VNN_PIN, BXT_TRACE_1_CLK_VNN_OFFSET },
	{ BXT_TRACE_1_DATA0_VNN_PIN, BXT_TRACE_1_DATA0_VNN_OFFSET },
	{ BXT_TRACE_1_DATA1_VNN_PIN, BXT_TRACE_1_DATA1_VNN_OFFSET },
	{ BXT_TRACE_1_DATA2_VNN_PIN, BXT_TRACE_1_DATA2_VNN_OFFSET },
	{ BXT_TRACE_1_DATA3_VNN_PIN, BXT_TRACE_1_DATA3_VNN_OFFSET },
	{ BXT_TRACE_1_DATA4_VNN_PIN, BXT_TRACE_1_DATA4_VNN_OFFSET },
	{ BXT_TRACE_1_DATA5_VNN_PIN, BXT_TRACE_1_DATA5_VNN_OFFSET },
	{ BXT_TRACE_1_DATA6_VNN_PIN, BXT_TRACE_1_DATA6_VNN_OFFSET },
	{ BXT_TRACE_1_DATA7_VNN_PIN, BXT_TRACE_1_DATA7_VNN_OFFSET },
	{ BXT_TRACE_2_CLK_VNN_PIN, BXT_TRACE_2_CLK_VNN_OFFSET },
	{ BXT_TRACE_2_DATA0_VNN_PIN, BXT_TRACE_2_DATA0_VNN_OFFSET },
	{ BXT_TRACE_2_DATA1_VNN_PIN, BXT_TRACE_2_DATA1_VNN_OFFSET },
	{ BXT_TRACE_2_DATA2_VNN_PIN, BXT_TRACE_2_DATA2_VNN_OFFSET },
	{ BXT_TRACE_2_DATA3_VNN_PIN, BXT_TRACE_2_DATA3_VNN_OFFSET },
	{ BXT_TRACE_2_DATA4_VNN_PIN, BXT_TRACE_2_DATA4_VNN_OFFSET },
	{ BXT_TRACE_2_DATA5_VNN_PIN, BXT_TRACE_2_DATA5_VNN_OFFSET },
	{ BXT_TRACE_2_DATA6_VNN_PIN, BXT_TRACE_2_DATA6_VNN_OFFSET },
	{ BXT_TRACE_2_DATA7_VNN_PIN, BXT_TRACE_2_DATA7_VNN_OFFSET },
	{ BXT_TRIGOUT_0_PIN, BXT_TRIGOUT_0_OFFSET },
	{ BXT_TRIGOUT_1_PIN, BXT_TRIGOUT_1_OFFSET },
	{ BXT_TRIGIN_0_PIN, BXT_TRIGIN_0_OFFSET },
	{ BXT_SEC_TCK_PIN, BXT_SEC_TCK_OFFSET },
	{ BXT_SEC_TDI_PIN, BXT_SEC_TDI_OFFSET },
	{ BXT_SEC_TMS_PIN, BXT_SEC_TMS_OFFSET },
	{ BXT_SEC_TDO_PIN, BXT_SEC_TDO_OFFSET },
	{ BXT_PWM0_PIN, BXT_PWM0_OFFSET },
	{ BXT_PWM1_PIN, BXT_PWM1_OFFSET },
	{ BXT_PWM2_PIN, BXT_PWM2_OFFSET },
	{ BXT_PWM3_PIN, BXT_PWM3_OFFSET },
	{ BXT_LPSS_UART0_RXD_PIN, BXT_LPSS_UART0_RXD_OFFSET },
	{ BXT_LPSS_UART0_TXD_PIN, BXT_LPSS_UART0_TXD_OFFSET },
	{ BXT_LPSS_UART0_RTS_B_PIN, BXT_LPSS_UART0_RTS_B_OFFSET },
	{ BXT_LPSS_UART0_CTS_B_PIN, BXT_LPSS_UART0_CTS_B_OFFSET },
	{ BXT_LPSS_UART1_RXD_PIN, BXT_LPSS_UART1_RXD_OFFSET },
	{ BXT_LPSS_UART1_TXD_PIN, BXT_LPSS_UART1_TXD_OFFSET },
	{ BXT_LPSS_UART1_RTS_B_PIN, BXT_LPSS_UART1_RTS_B_OFFSET },
	{ BXT_LPSS_UART1_CTS_B_PIN, BXT_LPSS_UART1_CTS_B_OFFSET },
	{ BXT_LPSS_UART2_RXD_PIN, BXT_LPSS_UART2_RXD_OFFSET },
	{ BXT_LPSS_UART2_TXD_PIN, BXT_LPSS_UART2_TXD_OFFSET },
	{ BXT_LPSS_UART2_RTS_B_PIN, BXT_LPSS_UART2_RTS_B_OFFSET },
	{ BXT_LPSS_UART2_CTS_B_PIN, BXT_LPSS_UART2_CTS_B_OFFSET },
	{ BXT_ISH_UART0_RXD_PIN, BXT_ISH_UART0_RXD_OFFSET },
	{ BXT_ISH_UART0_TXD_PIN, BXT_ISH_UART0_TXD_OFFSET },
	{ BXT_ISH_UART0_RTS_B_PIN, BXT_ISH_UART0_RTS_B_OFFSET },
	{ BXT_ISH_UART0_CTS_B_PIN, BXT_ISH_UART0_CTS_B_OFFSET },
	{ BXT_ISH_UART1_RXD_PIN, BXT_ISH_UART1_RXD_OFFSET },
	{ BXT_ISH_UART1_TXD_PIN, BXT_ISH_UART1_TXD_OFFSET },
	{ BXT_ISH_UART1_RTS_B_PIN, BXT_ISH_UART1_RTS_B_OFFSET },
	{ BXT_ISH_UART1_CTS_B_PIN, BXT_ISH_UART1_CTS_B_OFFSET },
	{ BXT_ISH_UART2_RXD_PIN, BXT_ISH_UART2_RXD_OFFSET },
	{ BXT_ISH_UART2_TXD_PIN, BXT_ISH_UART2_TXD_OFFSET },
	{ BXT_ISH_UART2_RTS_B_PIN, BXT_ISH_UART2_RTS_B_OFFSET },
	{ BXT_ISH_UART2_CTS_B_PIN, BXT_ISH_UART2_CTS_B_OFFSET },
	{ BXT_GP_CAMERASB00_PIN, BXT_GP_CAMERASB00_OFFSET },
	{ BXT_GP_CAMERASB01_PIN, BXT_GP_CAMERASB01_OFFSET },
	{ BXT_GP_CAMERASB02_PIN, BXT_GP_CAMERASB02_OFFSET },
	{ BXT_GP_CAMERASB03_PIN, BXT_GP_CAMERASB03_OFFSET },
	{ BXT_GP_CAMERASB04_PIN, BXT_GP_CAMERASB04_OFFSET },
	{ BXT_GP_CAMERASB05_PIN, BXT_GP_CAMERASB05_OFFSET },
	{ BXT_GP_CAMERASB06_PIN, BXT_GP_CAMERASB06_OFFSET },
	{ BXT_GP_CAMERASB07_PIN, BXT_GP_CAMERASB07_OFFSET },
	{ BXT_GP_CAMERASB08_PIN, BXT_GP_CAMERASB08_OFFSET },
	{ BXT_GP_CAMERASB09_PIN, BXT_GP_CAMERASB09_OFFSET },
	{ BXT_GP_CAMERASB10_PIN, BXT_GP_CAMERASB10_OFFSET },
	{ BXT_GP_CAMERASB11_PIN, BXT_GP_CAMERASB11_OFFSET },
};

static inline enum port intel_dsi_seq_port_to_port(u8 port)
{
	return port ? PORT_C : PORT_A;
}

static const u8 *mipi_exec_send_packet(struct intel_dsi *intel_dsi,
				       const u8 *data)
{
	struct mipi_dsi_device *dsi_device;
	u8 type, flags, seq_port;
	u16 len;
	enum port port;

	DRM_DEBUG_KMS("\n");

	flags = *data++;
	type = *data++;

	len = *((u16 *) data);
	data += 2;

	seq_port = (flags >> MIPI_PORT_SHIFT) & 3;

	/* For DSI single link on Port A & C, the seq_port value which is
	 * parsed from Sequence Block#53 of VBT has been set to 0
	 * Now, read/write of packets for the DSI single link on Port A and
	 * Port C will based on the DVO port from VBT block 2.
	 */
	if (intel_dsi->ports == (1 << PORT_C))
		port = PORT_C;
	else
		port = intel_dsi_seq_port_to_port(seq_port);

	dsi_device = intel_dsi->dsi_hosts[port]->device;
	if (!dsi_device) {
		DRM_DEBUG_KMS("no dsi device for port %c\n", port_name(port));
		goto out;
	}

	if ((flags >> MIPI_TRANSFER_MODE_SHIFT) & 1)
		dsi_device->mode_flags &= ~MIPI_DSI_MODE_LPM;
	else
		dsi_device->mode_flags |= MIPI_DSI_MODE_LPM;

	dsi_device->channel = (flags >> MIPI_VIRTUAL_CHANNEL_SHIFT) & 3;

	switch (type) {
	case MIPI_DSI_GENERIC_SHORT_WRITE_0_PARAM:
		mipi_dsi_generic_write(dsi_device, NULL, 0);
		break;
	case MIPI_DSI_GENERIC_SHORT_WRITE_1_PARAM:
		mipi_dsi_generic_write(dsi_device, data, 1);
		break;
	case MIPI_DSI_GENERIC_SHORT_WRITE_2_PARAM:
		mipi_dsi_generic_write(dsi_device, data, 2);
		break;
	case MIPI_DSI_GENERIC_READ_REQUEST_0_PARAM:
	case MIPI_DSI_GENERIC_READ_REQUEST_1_PARAM:
	case MIPI_DSI_GENERIC_READ_REQUEST_2_PARAM:
		DRM_DEBUG_DRIVER("Generic Read not yet implemented or used\n");
		break;
	case MIPI_DSI_GENERIC_LONG_WRITE:
		mipi_dsi_generic_write(dsi_device, data, len);
		break;
	case MIPI_DSI_DCS_SHORT_WRITE:
		mipi_dsi_dcs_write_buffer(dsi_device, data, 1);
		break;
	case MIPI_DSI_DCS_SHORT_WRITE_PARAM:
		mipi_dsi_dcs_write_buffer(dsi_device, data, 2);
		break;
	case MIPI_DSI_DCS_READ:
		DRM_DEBUG_DRIVER("DCS Read not yet implemented or used\n");
		break;
	case MIPI_DSI_DCS_LONG_WRITE:
		mipi_dsi_dcs_write_buffer(dsi_device, data, len);
		break;
	}

out:
	data += len;

	return data;
}

static const u8 *mipi_exec_delay(struct intel_dsi *intel_dsi, const u8 *data)
{
	u32 delay = *((const u32 *) data);

	DRM_DEBUG_KMS("\n");

	usleep_range(delay, delay + 10);
	data += 4;

	return data;
}

static void vlv_exec_gpio(struct drm_i915_private *dev_priv,
			  u8 gpio_source, u8 gpio_index, bool value)
{
	struct gpio_map *map;
	u16 pconf0, padval;
	u32 tmp;
	u8 port;

	if (gpio_index >= ARRAY_SIZE(vlv_gpio_table)) {
		DRM_DEBUG_KMS("unknown gpio index %u\n", gpio_index);
		return;
	}

	map = &vlv_gpio_table[gpio_index];

	if (dev_priv->vbt.dsi.seq_version >= 3) {
		/* XXX: this assumes vlv_gpio_table only has NC GPIOs. */
		port = IOSF_PORT_GPIO_NC;
	} else {
		if (gpio_source == 0) {
			port = IOSF_PORT_GPIO_NC;
		} else if (gpio_source == 1) {
			DRM_DEBUG_KMS("SC gpio not supported\n");
			return;
		} else {
			DRM_DEBUG_KMS("unknown gpio source %u\n", gpio_source);
			return;
		}
	}

	pconf0 = VLV_GPIO_PCONF0(map->base_offset);
	padval = VLV_GPIO_PAD_VAL(map->base_offset);

	mutex_lock(&dev_priv->sb_lock);
	if (!map->init) {
		/* FIXME: remove constant below */
		vlv_iosf_sb_write(dev_priv, port, pconf0, 0x2000CC00);
		map->init = true;
	}

	tmp = 0x4 | value;
	vlv_iosf_sb_write(dev_priv, port, padval, tmp);
	mutex_unlock(&dev_priv->sb_lock);
}

static void chv_exec_gpio(struct drm_i915_private *dev_priv,
			  u8 gpio_source, u8 gpio_index, bool value)
{
	u16 cfg0, cfg1;
	u16 family_num;
	u8 port;

	if (dev_priv->vbt.dsi.seq_version >= 3) {
		if (gpio_index >= CHV_GPIO_IDX_START_SE) {
			/* XXX: it's unclear whether 255->57 is part of SE. */
			gpio_index -= CHV_GPIO_IDX_START_SE;
			port = CHV_IOSF_PORT_GPIO_SE;
		} else if (gpio_index >= CHV_GPIO_IDX_START_SW) {
			gpio_index -= CHV_GPIO_IDX_START_SW;
			port = CHV_IOSF_PORT_GPIO_SW;
		} else if (gpio_index >= CHV_GPIO_IDX_START_E) {
			gpio_index -= CHV_GPIO_IDX_START_E;
			port = CHV_IOSF_PORT_GPIO_E;
		} else {
			port = CHV_IOSF_PORT_GPIO_N;
		}
	} else {
		/* XXX: The spec is unclear about CHV GPIO on seq v2 */
		if (gpio_source != 0) {
			DRM_DEBUG_KMS("unknown gpio source %u\n", gpio_source);
			return;
		}

		if (gpio_index >= CHV_GPIO_IDX_START_E) {
			DRM_DEBUG_KMS("invalid gpio index %u for GPIO N\n",
				      gpio_index);
			return;
		}

		port = CHV_IOSF_PORT_GPIO_N;
	}

	family_num = gpio_index / CHV_VBT_MAX_PINS_PER_FMLY;
	gpio_index = gpio_index % CHV_VBT_MAX_PINS_PER_FMLY;

	cfg0 = CHV_GPIO_PAD_CFG0(family_num, gpio_index);
	cfg1 = CHV_GPIO_PAD_CFG1(family_num, gpio_index);

	mutex_lock(&dev_priv->sb_lock);
	vlv_iosf_sb_write(dev_priv, port, cfg1, 0);
	vlv_iosf_sb_write(dev_priv, port, cfg0,
			  CHV_GPIO_GPIOCFG_GPO | CHV_GPIO_GPIOTXSTATE(value));
	mutex_unlock(&dev_priv->sb_lock);
}

static void bxt_exec_gpio(struct drm_i915_private *dev_priv,
			  u8 gpio_source, u8 gpio_index, bool value)
{
	struct bxt_gpio_map *map = NULL;
	unsigned int gpio;
	int i;

	for (i = 0; i < ARRAY_SIZE(bxt_gpio_table); i++) {
		if (gpio_index == bxt_gpio_table[i].gpio_index) {
			map = &bxt_gpio_table[i];
			break;
		}
	}

	if (!map) {
		DRM_DEBUG_KMS("invalid gpio index %u\n", gpio_index);
		return;
	}

	gpio = map->gpio_number;

	if (!map->requested) {
		int ret = devm_gpio_request_one(dev_priv->drm.dev, gpio,
						GPIOF_DIR_OUT, "MIPI DSI");
		if (ret) {
			DRM_ERROR("unable to request GPIO %u (%d)\n", gpio, ret);
			return;
		}
		map->requested = true;
	}

	gpio_set_value(gpio, value);
}

static const u8 *mipi_exec_gpio(struct intel_dsi *intel_dsi, const u8 *data)
{
	struct drm_device *dev = intel_dsi->base.base.dev;
	struct drm_i915_private *dev_priv = to_i915(dev);
	u8 gpio_source, gpio_index;
	bool value;

	DRM_DEBUG_KMS("\n");

	if (dev_priv->vbt.dsi.seq_version >= 3)
		data++;

	gpio_index = *data++;

	/* gpio source in sequence v2 only */
	if (dev_priv->vbt.dsi.seq_version == 2)
		gpio_source = (*data >> 1) & 3;
	else
		gpio_source = 0;

	/* pull up/down */
	value = *data++ & 1;

	if (IS_VALLEYVIEW(dev_priv))
		vlv_exec_gpio(dev_priv, gpio_source, gpio_index, value);
	else if (IS_CHERRYVIEW(dev_priv))
		chv_exec_gpio(dev_priv, gpio_source, gpio_index, value);
	else
		bxt_exec_gpio(dev_priv, gpio_source, gpio_index, value);

	return data;
}

static const u8 *mipi_exec_i2c(struct intel_dsi *intel_dsi, const u8 *data)
{
	DRM_DEBUG_KMS("Skipping I2C element execution\n");

	return data + *(data + 6) + 7;
}

static const u8 *mipi_exec_spi(struct intel_dsi *intel_dsi, const u8 *data)
{
	DRM_DEBUG_KMS("Skipping SPI element execution\n");

	return data + *(data + 5) + 6;
}

static const u8 *mipi_exec_pmic(struct intel_dsi *intel_dsi, const u8 *data)
{
	DRM_DEBUG_KMS("Skipping PMIC element execution\n");

	return data + 15;
}

typedef const u8 * (*fn_mipi_elem_exec)(struct intel_dsi *intel_dsi,
					const u8 *data);
static const fn_mipi_elem_exec exec_elem[] = {
	[MIPI_SEQ_ELEM_SEND_PKT] = mipi_exec_send_packet,
	[MIPI_SEQ_ELEM_DELAY] = mipi_exec_delay,
	[MIPI_SEQ_ELEM_GPIO] = mipi_exec_gpio,
	[MIPI_SEQ_ELEM_I2C] = mipi_exec_i2c,
	[MIPI_SEQ_ELEM_SPI] = mipi_exec_spi,
	[MIPI_SEQ_ELEM_PMIC] = mipi_exec_pmic,
};

/*
 * MIPI Sequence from VBT #53 parsing logic
 * We have already separated each seqence during bios parsing
 * Following is generic execution function for any sequence
 */

static const char * const seq_name[] = {
	[MIPI_SEQ_ASSERT_RESET] = "MIPI_SEQ_ASSERT_RESET",
	[MIPI_SEQ_INIT_OTP] = "MIPI_SEQ_INIT_OTP",
	[MIPI_SEQ_DISPLAY_ON] = "MIPI_SEQ_DISPLAY_ON",
	[MIPI_SEQ_DISPLAY_OFF]  = "MIPI_SEQ_DISPLAY_OFF",
	[MIPI_SEQ_DEASSERT_RESET] = "MIPI_SEQ_DEASSERT_RESET",
	[MIPI_SEQ_BACKLIGHT_ON] = "MIPI_SEQ_BACKLIGHT_ON",
	[MIPI_SEQ_BACKLIGHT_OFF] = "MIPI_SEQ_BACKLIGHT_OFF",
	[MIPI_SEQ_TEAR_ON] = "MIPI_SEQ_TEAR_ON",
	[MIPI_SEQ_TEAR_OFF] = "MIPI_SEQ_TEAR_OFF",
	[MIPI_SEQ_POWER_ON] = "MIPI_SEQ_POWER_ON",
	[MIPI_SEQ_POWER_OFF] = "MIPI_SEQ_POWER_OFF",
};

static const char *sequence_name(enum mipi_seq seq_id)
{
	if (seq_id < ARRAY_SIZE(seq_name) && seq_name[seq_id])
		return seq_name[seq_id];
	else
		return "(unknown)";
}

static void generic_exec_sequence(struct drm_panel *panel, enum mipi_seq seq_id)
{
	struct vbt_panel *vbt_panel = to_vbt_panel(panel);
	struct intel_dsi *intel_dsi = vbt_panel->intel_dsi;
	struct drm_i915_private *dev_priv = to_i915(intel_dsi->base.base.dev);
	const u8 *data;
	fn_mipi_elem_exec mipi_elem_exec;

	if (WARN_ON(seq_id >= ARRAY_SIZE(dev_priv->vbt.dsi.sequence)))
		return;

	data = dev_priv->vbt.dsi.sequence[seq_id];
	if (!data)
		return;

	WARN_ON(*data != seq_id);

	DRM_DEBUG_KMS("Starting MIPI sequence %d - %s\n",
		      seq_id, sequence_name(seq_id));

	/* Skip Sequence Byte. */
	data++;

	/* Skip Size of Sequence. */
	if (dev_priv->vbt.dsi.seq_version >= 3)
		data += 4;

	while (1) {
		u8 operation_byte = *data++;
		u8 operation_size = 0;

		if (operation_byte == MIPI_SEQ_ELEM_END)
			break;

		if (operation_byte < ARRAY_SIZE(exec_elem))
			mipi_elem_exec = exec_elem[operation_byte];
		else
			mipi_elem_exec = NULL;

		/* Size of Operation. */
		if (dev_priv->vbt.dsi.seq_version >= 3)
			operation_size = *data++;

		if (mipi_elem_exec) {
			const u8 *next = data + operation_size;

			data = mipi_elem_exec(intel_dsi, data);

			/* Consistency check if we have size. */
			if (operation_size && data != next) {
				DRM_ERROR("Inconsistent operation size\n");
				return;
			}
		} else if (operation_size) {
			/* We have size, skip. */
			DRM_DEBUG_KMS("Unsupported MIPI operation byte %u\n",
				      operation_byte);
			data += operation_size;
		} else {
			/* No size, can't skip without parsing. */
			DRM_ERROR("Unsupported MIPI operation byte %u\n",
				  operation_byte);
			return;
		}
	}
}

static int vbt_panel_prepare(struct drm_panel *panel)
{
	generic_exec_sequence(panel, MIPI_SEQ_ASSERT_RESET);
	generic_exec_sequence(panel, MIPI_SEQ_POWER_ON);
	generic_exec_sequence(panel, MIPI_SEQ_DEASSERT_RESET);
	generic_exec_sequence(panel, MIPI_SEQ_INIT_OTP);

	return 0;
}

static int vbt_panel_unprepare(struct drm_panel *panel)
{
	generic_exec_sequence(panel, MIPI_SEQ_ASSERT_RESET);
	generic_exec_sequence(panel, MIPI_SEQ_POWER_OFF);

	return 0;
}

static int vbt_panel_enable(struct drm_panel *panel)
{
	generic_exec_sequence(panel, MIPI_SEQ_DISPLAY_ON);
	generic_exec_sequence(panel, MIPI_SEQ_BACKLIGHT_ON);

	return 0;
}

static int vbt_panel_disable(struct drm_panel *panel)
{
	generic_exec_sequence(panel, MIPI_SEQ_BACKLIGHT_OFF);
	generic_exec_sequence(panel, MIPI_SEQ_DISPLAY_OFF);

	return 0;
}

static int vbt_panel_get_modes(struct drm_panel *panel)
{
	struct vbt_panel *vbt_panel = to_vbt_panel(panel);
	struct intel_dsi *intel_dsi = vbt_panel->intel_dsi;
	struct drm_device *dev = intel_dsi->base.base.dev;
	struct drm_i915_private *dev_priv = to_i915(dev);
	struct drm_display_mode *mode;

	if (!panel->connector)
		return 0;

	mode = drm_mode_duplicate(dev, dev_priv->vbt.lfp_lvds_vbt_mode);
	if (!mode)
		return 0;

	mode->type |= DRM_MODE_TYPE_PREFERRED;

	drm_mode_probed_add(panel->connector, mode);

	return 1;
}

static const struct drm_panel_funcs vbt_panel_funcs = {
	.disable = vbt_panel_disable,
	.unprepare = vbt_panel_unprepare,
	.prepare = vbt_panel_prepare,
	.enable = vbt_panel_enable,
	.get_modes = vbt_panel_get_modes,
};

struct drm_panel *vbt_panel_init(struct intel_dsi *intel_dsi, u16 panel_id)
{
	struct drm_device *dev = intel_dsi->base.base.dev;
	struct drm_i915_private *dev_priv = to_i915(dev);
	struct mipi_config *mipi_config = dev_priv->vbt.dsi.config;
	struct mipi_pps_data *pps = dev_priv->vbt.dsi.pps;
	struct drm_display_mode *mode = dev_priv->vbt.lfp_lvds_vbt_mode;
	struct vbt_panel *vbt_panel;
	u32 bpp;
	u32 tlpx_ns, extra_byte_count, bitrate, tlpx_ui;
	u32 ui_num, ui_den;
	u32 prepare_cnt, exit_zero_cnt, clk_zero_cnt, trail_cnt;
	u32 ths_prepare_ns, tclk_trail_ns;
	u32 tclk_prepare_clkzero, ths_prepare_hszero;
	u32 lp_to_hs_switch, hs_to_lp_switch;
	u32 pclk, computed_ddr;
	u16 burst_mode_ratio;
	enum port port;

	DRM_DEBUG_KMS("\n");

	intel_dsi->eotp_pkt = mipi_config->eot_pkt_disabled ? 0 : 1;
	intel_dsi->clock_stop = mipi_config->enable_clk_stop ? 1 : 0;
	intel_dsi->lane_count = mipi_config->lane_cnt + 1;
	intel_dsi->pixel_format =
			pixel_format_from_register_bits(
				mipi_config->videomode_color_format << 7);
	bpp = mipi_dsi_pixel_format_to_bpp(intel_dsi->pixel_format);

	intel_dsi->dual_link = mipi_config->dual_link;
	intel_dsi->pixel_overlap = mipi_config->pixel_overlap;
	intel_dsi->operation_mode = mipi_config->is_cmd_mode;
	intel_dsi->video_mode_format = mipi_config->video_transfer_mode;
	intel_dsi->escape_clk_div = mipi_config->byte_clk_sel;
	intel_dsi->lp_rx_timeout = mipi_config->lp_rx_timeout;
	intel_dsi->turn_arnd_val = mipi_config->turn_around_timeout;
	intel_dsi->rst_timer_val = mipi_config->device_reset_timer;
	intel_dsi->init_count = mipi_config->master_init_timer;
	intel_dsi->bw_timer = mipi_config->dbi_bw_timer;
	intel_dsi->video_frmt_cfg_bits =
		mipi_config->bta_enabled ? DISABLE_VIDEO_BTA : 0;

	pclk = mode->clock;

	/* In dual link mode each port needs half of pixel clock */
	if (intel_dsi->dual_link) {
		pclk = pclk / 2;

		/* we can enable pixel_overlap if needed by panel. In this
		 * case we need to increase the pixelclock for extra pixels
		 */
		if (intel_dsi->dual_link == DSI_DUAL_LINK_FRONT_BACK) {
			pclk += DIV_ROUND_UP(mode->vtotal *
						intel_dsi->pixel_overlap *
						60, 1000);
		}
	}

	/* Burst Mode Ratio
	 * Target ddr frequency from VBT / non burst ddr freq
	 * multiply by 100 to preserve remainder
	 */
	if (intel_dsi->video_mode_format == VIDEO_MODE_BURST) {
		if (mipi_config->target_burst_mode_freq) {
			computed_ddr = (pclk * bpp) / intel_dsi->lane_count;

			if (mipi_config->target_burst_mode_freq <
								computed_ddr) {
				DRM_ERROR("Burst mode freq is less than computed\n");
				return NULL;
			}

			burst_mode_ratio = DIV_ROUND_UP(
				mipi_config->target_burst_mode_freq * 100,
				computed_ddr);

			pclk = DIV_ROUND_UP(pclk * burst_mode_ratio, 100);
		} else {
			DRM_ERROR("Burst mode target is not set\n");
			return NULL;
		}
	} else
		burst_mode_ratio = 100;

	intel_dsi->burst_mode_ratio = burst_mode_ratio;
	intel_dsi->pclk = pclk;

	bitrate = (pclk * bpp) / intel_dsi->lane_count;

	switch (intel_dsi->escape_clk_div) {
	case 0:
		tlpx_ns = 50;
		break;
	case 1:
		tlpx_ns = 100;
		break;

	case 2:
		tlpx_ns = 200;
		break;
	default:
		tlpx_ns = 50;
		break;
	}

	switch (intel_dsi->lane_count) {
	case 1:
	case 2:
		extra_byte_count = 2;
		break;
	case 3:
		extra_byte_count = 4;
		break;
	case 4:
	default:
		extra_byte_count = 3;
		break;
	}

	/*
	 * ui(s) = 1/f [f in hz]
	 * ui(ns) = 10^9 / (f*10^6) [f in Mhz] -> 10^3/f(Mhz)
	 */

	/* in Kbps */
	ui_num = NS_KHZ_RATIO;
	ui_den = bitrate;

	tclk_prepare_clkzero = mipi_config->tclk_prepare_clkzero;
	ths_prepare_hszero = mipi_config->ths_prepare_hszero;

	/*
	 * B060
	 * LP byte clock = TLPX/ (8UI)
	 */
	intel_dsi->lp_byte_clk = DIV_ROUND_UP(tlpx_ns * ui_den, 8 * ui_num);

	/* count values in UI = (ns value) * (bitrate / (2 * 10^6))
	 *
	 * Since txddrclkhs_i is 2xUI, all the count values programmed in
	 * DPHY param register are divided by 2
	 *
	 * prepare count
	 */
	ths_prepare_ns = max(mipi_config->ths_prepare,
			     mipi_config->tclk_prepare);
	prepare_cnt = DIV_ROUND_UP(ths_prepare_ns * ui_den, ui_num * 2);

	/* exit zero count */
	exit_zero_cnt = DIV_ROUND_UP(
				(ths_prepare_hszero - ths_prepare_ns) * ui_den,
				ui_num * 2
				);

	/*
	 * Exit zero is unified val ths_zero and ths_exit
	 * minimum value for ths_exit = 110ns
	 * min (exit_zero_cnt * 2) = 110/UI
	 * exit_zero_cnt = 55/UI
	 */
	if (exit_zero_cnt < (55 * ui_den / ui_num) && (55 * ui_den) % ui_num)
		exit_zero_cnt += 1;

	/* clk zero count */
	clk_zero_cnt = DIV_ROUND_UP(
			(tclk_prepare_clkzero -	ths_prepare_ns)
			* ui_den, 2 * ui_num);

	/* trail count */
	tclk_trail_ns = max(mipi_config->tclk_trail, mipi_config->ths_trail);
	trail_cnt = DIV_ROUND_UP(tclk_trail_ns * ui_den, 2 * ui_num);

	if (prepare_cnt > PREPARE_CNT_MAX ||
		exit_zero_cnt > EXIT_ZERO_CNT_MAX ||
		clk_zero_cnt > CLK_ZERO_CNT_MAX ||
		trail_cnt > TRAIL_CNT_MAX)
		DRM_DEBUG_DRIVER("Values crossing maximum limits, restricting to max values\n");

	if (prepare_cnt > PREPARE_CNT_MAX)
		prepare_cnt = PREPARE_CNT_MAX;

	if (exit_zero_cnt > EXIT_ZERO_CNT_MAX)
		exit_zero_cnt = EXIT_ZERO_CNT_MAX;

	if (clk_zero_cnt > CLK_ZERO_CNT_MAX)
		clk_zero_cnt = CLK_ZERO_CNT_MAX;

	if (trail_cnt > TRAIL_CNT_MAX)
		trail_cnt = TRAIL_CNT_MAX;

	/* B080 */
	intel_dsi->dphy_reg = exit_zero_cnt << 24 | trail_cnt << 16 |
						clk_zero_cnt << 8 | prepare_cnt;

	/*
	 * LP to HS switch count = 4TLPX + PREP_COUNT * 2 + EXIT_ZERO_COUNT * 2
	 *					+ 10UI + Extra Byte Count
	 *
	 * HS to LP switch count = THS-TRAIL + 2TLPX + Extra Byte Count
	 * Extra Byte Count is calculated according to number of lanes.
	 * High Low Switch Count is the Max of LP to HS and
	 * HS to LP switch count
	 *
	 */
	tlpx_ui = DIV_ROUND_UP(tlpx_ns * ui_den, ui_num);

	/* B044 */
	/* FIXME:
	 * The comment above does not match with the code */
	lp_to_hs_switch = DIV_ROUND_UP(4 * tlpx_ui + prepare_cnt * 2 +
						exit_zero_cnt * 2 + 10, 8);

	hs_to_lp_switch = DIV_ROUND_UP(mipi_config->ths_trail + 2 * tlpx_ui, 8);

	intel_dsi->hs_to_lp_count = max(lp_to_hs_switch, hs_to_lp_switch);
	intel_dsi->hs_to_lp_count += extra_byte_count;

	/* B088 */
	/* LP -> HS for clock lanes
	 * LP clk sync + LP11 + LP01 + tclk_prepare + tclk_zero +
	 *						extra byte count
	 * 2TPLX + 1TLPX + 1 TPLX(in ns) + prepare_cnt * 2 + clk_zero_cnt *
	 *					2(in UI) + extra byte count
	 * In byteclks = (4TLPX + prepare_cnt * 2 + clk_zero_cnt *2 (in UI)) /
	 *					8 + extra byte count
	 */
	intel_dsi->clk_lp_to_hs_count =
		DIV_ROUND_UP(
			4 * tlpx_ui + prepare_cnt * 2 +
			clk_zero_cnt * 2,
			8);

	intel_dsi->clk_lp_to_hs_count += extra_byte_count;

	/* HS->LP for Clock Lanes
	 * Low Power clock synchronisations + 1Tx byteclk + tclk_trail +
	 *						Extra byte count
	 * 2TLPX + 8UI + (trail_count*2)(in UI) + Extra byte count
	 * In byteclks = (2*TLpx(in UI) + trail_count*2 +8)(in UI)/8 +
	 *						Extra byte count
	 */
	intel_dsi->clk_hs_to_lp_count =
		DIV_ROUND_UP(2 * tlpx_ui + trail_cnt * 2 + 8,
			8);
	intel_dsi->clk_hs_to_lp_count += extra_byte_count;

	DRM_DEBUG_KMS("Eot %s\n", intel_dsi->eotp_pkt ? "enabled" : "disabled");
	DRM_DEBUG_KMS("Clockstop %s\n", intel_dsi->clock_stop ?
						"disabled" : "enabled");
	DRM_DEBUG_KMS("Mode %s\n", intel_dsi->operation_mode ? "command" : "video");
	if (intel_dsi->dual_link == DSI_DUAL_LINK_FRONT_BACK)
		DRM_DEBUG_KMS("Dual link: DSI_DUAL_LINK_FRONT_BACK\n");
	else if (intel_dsi->dual_link == DSI_DUAL_LINK_PIXEL_ALT)
		DRM_DEBUG_KMS("Dual link: DSI_DUAL_LINK_PIXEL_ALT\n");
	else
		DRM_DEBUG_KMS("Dual link: NONE\n");
	DRM_DEBUG_KMS("Pixel Format %d\n", intel_dsi->pixel_format);
	DRM_DEBUG_KMS("TLPX %d\n", intel_dsi->escape_clk_div);
	DRM_DEBUG_KMS("LP RX Timeout 0x%x\n", intel_dsi->lp_rx_timeout);
	DRM_DEBUG_KMS("Turnaround Timeout 0x%x\n", intel_dsi->turn_arnd_val);
	DRM_DEBUG_KMS("Init Count 0x%x\n", intel_dsi->init_count);
	DRM_DEBUG_KMS("HS to LP Count 0x%x\n", intel_dsi->hs_to_lp_count);
	DRM_DEBUG_KMS("LP Byte Clock %d\n", intel_dsi->lp_byte_clk);
	DRM_DEBUG_KMS("DBI BW Timer 0x%x\n", intel_dsi->bw_timer);
	DRM_DEBUG_KMS("LP to HS Clock Count 0x%x\n", intel_dsi->clk_lp_to_hs_count);
	DRM_DEBUG_KMS("HS to LP Clock Count 0x%x\n", intel_dsi->clk_hs_to_lp_count);
	DRM_DEBUG_KMS("BTA %s\n",
			intel_dsi->video_frmt_cfg_bits & DISABLE_VIDEO_BTA ?
			"disabled" : "enabled");

	/* delays in VBT are in unit of 100us, so need to convert
	 * here in ms
	 * Delay (100us) * 100 /1000 = Delay / 10 (ms) */
	intel_dsi->backlight_off_delay = pps->bl_disable_delay / 10;
	intel_dsi->backlight_on_delay = pps->bl_enable_delay / 10;
	intel_dsi->panel_on_delay = pps->panel_on_delay / 10;
	intel_dsi->panel_off_delay = pps->panel_off_delay / 10;
	intel_dsi->panel_pwr_cycle_delay = pps->panel_power_cycle_delay / 10;

	/* This is cheating a bit with the cleanup. */
	vbt_panel = devm_kzalloc(dev->dev, sizeof(*vbt_panel), GFP_KERNEL);
	if (!vbt_panel)
		return NULL;

	vbt_panel->intel_dsi = intel_dsi;
	drm_panel_init(&vbt_panel->panel);
	vbt_panel->panel.funcs = &vbt_panel_funcs;
	drm_panel_add(&vbt_panel->panel);

	/* a regular driver would get the device in probe */
	for_each_dsi_port(port, intel_dsi->ports) {
		mipi_dsi_attach(intel_dsi->dsi_hosts[port]->device);
	}

	return &vbt_panel->panel;
}
