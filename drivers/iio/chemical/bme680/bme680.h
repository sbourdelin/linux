// SPDX-License-Identifier: GPL-2.0
#ifndef BME680_H_
#define BME680_H_

#define BME680_REG_CHIP_I2C_ID			0xD0
#define BME680_REG_CHIP_SPI_ID			0x50
#define BME680_CHIP_ID_VAL			0x61
#define BME680_REG_SOFT_RESET			0xE0
#define BME680_CMD_SOFTRESET			0xB6
#define BME680_REG_STATUS			0x73
#define   BME680_SPI_MEM_PAGE_BIT		BIT(4)
#define   BME680_SPI_MEM_PAGE_1_VAL		1

#define BME680_OSRS_TEMP_X(osrs_t)		((osrs_t) << 5)
#define BME680_OSRS_PRESS_X(osrs_p)		((osrs_p) << 2)
#define BME680_OSRS_HUMID_X(osrs_h)		((osrs_h) << 0)

#define BME680_REG_TEMP_MSB			0x22
#define BME680_REG_PRESS_MSB			0x1F
#define BM6880_REG_HUMIDITY_MSB			0x25
#define BME680_REG_GAS_MSB			0x2A
#define BME680_REG_GAS_R_LSB			0x2B
#define   BME680_GAS_STAB_BIT			BIT(4)

#define BME680_REG_CTRL_HUMIDITY		0x72
#define   BME680_OSRS_HUMIDITY_MASK		GENMASK(2, 0)

#define BME680_REG_CTRL_MEAS			0x74
#define   BME680_OSRS_TEMP_MASK			GENMASK(7, 5)
#define   BME680_OSRS_PRESS_MASK		GENMASK(4, 2)
#define   BME680_MODE_MASK			GENMASK(1, 0)

#define BME680_MODE_FORCED			BIT(0)
#define BME680_MODE_SLEEP			0

#define BME680_REG_CONFIG			0x75
#define   BME680_FILTER_MASK			GENMASK(4, 2)
#define   BME680_FILTER_COEFF			BIT(1)

/* TEMP/PRESS/HUMID reading skipped */
#define BME680_MEAS_SKIPPED			0x8000

/* Calibration coefficient's address */
#define BME680_COEFF_ADDR1			0x89
#define BME680_COEFF_ADDR1_LEN			25
#define BME680_COEFF_ADDR2			0xE1
#define BME680_COEFF_ADDR2_LEN			16
#define BME680_COEFF_SIZE			41

#define BME680_MAX_OVERFLOW_VAL			0x40000000
#define BME680_HUM_REG_SHIFT_VAL		4
#define BME680_BIT_H1_DATA_MSK			0x0F

#define BME680_REG_RES_HEAT_RANGE		0x02
#define BME680_RHRANGE_MSK			0x30
#define BME680_REG_RES_HEAT_VAL			0x00
#define BME680_REG_RANGE_SW_ERR			0x04
#define BME680_RSERROR_MSK			0xF0
#define BME680_REG_RES_HEAT_0			0x5A
#define BME680_REG_GAS_WAIT_0			0x64
#define BME680_GAS_RANGE_MASK			0x0F
#define BME680_ADC_GAS_RES_SHIFT		6
#define BME680_AMB_TEMP				25

#define BME680_REG_CTRL_GAS_1			0x71
#define   BME680_RUN_GAS_MASK			BIT(4)
#define   BME680_NB_CONV_MASK			GENMASK(3, 0)
#define	BME680_RUN_GAS_EN			BIT(4)
#define BME680_NB_CONV_0			0

#define BME680_REG_MEAS_STAT_0			0x1D
#define   BME680_GAS_MEAS_BIT			BIT(6)

/* Macro to combine two 8 bit data's to form a 16 bit data */
#define BME680_CONCAT_BYTES(MSB, LSB)   (((uint16_t)MSB << 8) | (uint16_t)LSB)

/* Array Index to Field data mapping for Calibration Data*/
#define BME680_T2_LSB_REG	1
#define BME680_T2_MSB_REG	2
#define BME680_T3_REG		3
#define BME680_P1_LSB_REG	5
#define BME680_P1_MSB_REG	6
#define BME680_P2_LSB_REG	7
#define BME680_P2_MSB_REG	8
#define BME680_P3_REG		9
#define BME680_P4_LSB_REG	11
#define BME680_P4_MSB_REG	12
#define BME680_P5_LSB_REG	13
#define BME680_P5_MSB_REG	14
#define BME680_P7_REG		15
#define BME680_P6_REG		16
#define BME680_P8_LSB_REG	19
#define BME680_P8_MSB_REG	20
#define BME680_P9_LSB_REG	21
#define BME680_P9_MSB_REG	22
#define BME680_P10_REG		23
#define BME680_H2_MSB_REG	25
#define BME680_H2_LSB_REG	26
#define BME680_H1_LSB_REG	26
#define BME680_H1_MSB_REG	27
#define BME680_H3_REG		28
#define BME680_H4_REG		29
#define BME680_H5_REG		30
#define BME680_H6_REG		31
#define BME680_H7_REG		32
#define BME680_T1_LSB_REG	33
#define BME680_T1_MSB_REG	34
#define BME680_GH2_LSB_REG	35
#define BME680_GH2_MSB_REG	36
#define BME680_GH1_REG		37
#define BME680_GH3_REG		38

extern const struct regmap_config bme680_regmap_config;

int bme680_core_probe(struct device *dev, struct regmap *regmap,
		      const char *name);

#endif  /* BME680_H_ */
