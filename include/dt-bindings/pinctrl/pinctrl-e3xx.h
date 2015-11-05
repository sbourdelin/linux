/*
 * Copyright (C) 2015 National Instruments Corp
 *
 * This header provides constants for USRP E3xx pinctrl bindings
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */


#ifndef _DT_BINDINGS_PINCTRL_E3XX_H
#define _DT_BINDINGS_PINCTRL_E3XX_H

/* Pin names for the E31x usecase */
#define E31X_TX_BANDSEL_2	"DB_1"
#define E31X_RX1B_BANDSEL_0	"DB_3"
#define E31X_RX1B_BANDSEL_1	"DB_5"
#define E31X_VCTXRX2_V2		"DB_7"
#define E31X_TX_ENABLE1A	"DB_9"
#define E31X_TX_ENABLE2A	"DB_11"
#define E31X_TX_BANDSEL_0	"DB_12"
#define E31X_TX_ENABLE1B	"DB_13"
#define E31X_TX_ENABLE2B	"DB_15"
#define E31X_RX1C_BANDSEL_0	"DB_17"
#define E31X_RX1C_BANDSEL_1	"DB_19"
#define E31X_VCTXRX2_V1		"DB_21"
#define E31X_VCTXRX1_V2		"DB_23"
#define E31X_VCTXRX1_V1		"DB_25"
#define E31X_VCRX1_V1		"DB_27"
#define E31X_VCRX1_V2		"DB_29"
#define E31X_VCRX2_V1		"DB_31"
#define E31X_VCRX2_V2		"DB_33"
#define E31X_CAT_CTRL_IN2	"DB_35"
#define E31X_CAT_CTRL_IN3	"DB_37"
#define E31X_CAT_CTRL_OUT0	"DB_39"
#define E31X_CAT_CTRL_OUT1	"DB_41"
#define E31X_CAT_CTRL_OUT2	"DB_43"
#define E31X_CAT_CTRL_OUT3	"DB_45"
/* pin 47 is nc */
/* pin 49 is nc */
/* pin 51 is nc */
/* pin 53 is nc */
/* pin 55 is nc */
/* pin 57 is nc */
/* pin 59 is nc */
/* pin 61 is nc */
/* pin 63 is nc */
/* pin 65 is nc */
#define E31X_CAT_TXNRX		"DB_67"
#define E31X_CAT_ENABLE		"DB_69"
#define E31X_CAT_ENAGC		"DB_71"
/* pin 73 is nc */
#define E31X_CAT_P0_D5		"DB_75"
#define E31X_CAT_P0_D7		"DB_77"
#define E31X_CAT_P0_D3		"DB_79"
#define E31X_CAT_P0_D9		"DB_81"
#define E31X_CAT_P0_D1		"DB_83"
#define E31X_CAT_P0_D6		"DB_85"
#define E31X_CAT_P0_D0		"DB_87"
#define E31X_CAT_P0_D2		"DB_89"
#define E31X_CAT_P0_D4		"DB_91"
#define E31X_CAT_P0_D11		"DB_93"
#define E31X_CAT_P0_D10		"DB_95"
#define E31X_CAT_P0_D8		"DB_97"
#define E31X_CAT_RX_FRAME	"DB_99"
#define E31X_CAT_DATA_CLK	"DB_101"
/* pin 103 is nc */
/* pin 105 is nc */
#define E31X_RX2_BANDSEL_2	"DB_107"
#define E31X_RX2_BANDSEL_1	"DB_109"
#define E31X_RX2_BANDSEL_0	"DB_111"
#define E31X_RX2C_BANDSEL_1	"DB_113"
#define E31X_RX2C_BANDSEL_0	"DB_115"
#define E31X_RX2B_BANDSEL_0	"DB_117"
#define E31X_RX2B_BANDSEL_1	"DB_119"

/* pin 2 is nc */
/* pin 4 is nc */
#define E31X_RX1_BANDSEL_0	"DB_6"
#define E31X_RX1_BANDSEL_1	"DB_8"
#define E31X_RX1_BANDSEL_2	"DB_10"
#define E31X_TX_BANDSEL_1	"DB_14"
#define E31X_DB_SCL		"DB_16"
#define E31X_DB_SCA		"DB_18"
#define E31X_TUNE_DAC_SYNC_N	"DB_20"
#define E31X_TUNE_DAC_SCLK	"DB_22"
#define E31X_TUNE_DAC_SDIN	"DB_24"
/* pin 26 is nc */
/* pin 28 is nc */
#define E31X_VCTCXO_TO_MB	"DB_30"
/* pin 32 is nc */
/* pin 34 is nc */
/* pin 36 is nc */
/* pin 38 is nc */
#define E31X_CAT_CTRL_OUT4	"DB_40"
#define E31X_CAT_CTRL_OUT5	"DB_42"
#define E31X_CAT_CTRL_OUT6	"DB_44"
#define E31X_CAT_CTRL_OUT7	"DB_46"
#define E31X_CAT_RESET		"DB_48"
#define E31X_CAT_CS		"DB_50"
#define E31X_CAT_SCLK		"DB_52"
#define E31X_CAT_MOSI		"DB_54"
#define E31X_CAT_MISO		"DB_56"
#define E31X_CAT_CTRL_IN0	"DB_58"
#define E31X_CAT_CTRL_IN1	"DB_60"
/* pin 62 is nc */
/* pin 64 is nc */
/* pin 66 is nc */
/* pin 68 is nc */
#define E31X_CAT_BBCLK_OUT	"DB_70"
/* pin 72 is nc */
#define E31X_CAT_SYNC		"DB_74"
#define E31X_CAT_P1_D11		"DB_78"
#define E31X_CAT_P1_D1		"DB_80"
#define E31X_CAT_P1_D3		"DB_82"
#define E31X_CAT_P1_D0		"DB_84"
#define E31X_CAT_P1_D5		"DB_86"
#define E31X_CAT_P1_D2		"DB_88"
#define E31X_CAT_P1_D4		"DB_90"
#define E31X_CAT_P1_D7		"DB_92"
#define E31X_CAT_P1_D6		"DB_94"
#define E31X_CAT_P1_D9		"DB_96"
#define E31X_CAT_P1_D8		"DB_98"
#define E31X_CAT_P1_D10		"DB_100"
#define E31X_CAT_TX_FRAME	"DB_102"
#define E31X_CAT_FB_CLK		"DB_104"
/* pin 106 is nc */
#define E31X_LED_TXRX1_TX	"DB_108"
#define E31X_LED_TXRX1_RX	"DB_110"
#define E31X_LED_RX1_RX		"DB_112"
#define E31X_LED_TXRX2_TX	"DB_114"
#define E31X_LED_TXRX2_RX	"DB_116"
#define E31X_LED_RX2_RX		"DB_118"
/* pin 120 is nc */

#endif /* _DT_BINDINGS_PINCTRL_E3XX_H */
