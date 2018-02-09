/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _TI_ADS1015_H_
#define _TI_ADS1015_H_

#define ADS1015_DRV_NAME "ads1015"

#define ADS1015_CONV_REG	0x00
#define ADS1015_CFG_REG		0x01
#define ADS1015_LO_THRESH_REG	0x02
#define ADS1015_HI_THRESH_REG	0x03

#define ADS1015_CFG_COMP_QUE_SHIFT	0
#define ADS1015_CFG_COMP_LAT_SHIFT	2
#define ADS1015_CFG_COMP_POL_SHIFT	3
#define ADS1015_CFG_COMP_MODE_SHIFT	4
#define ADS1015_CFG_DR_SHIFT	5
#define ADS1015_CFG_MOD_SHIFT	8
#define ADS1015_CFG_PGA_SHIFT	9
#define ADS1015_CFG_MUX_SHIFT	12

#define ADS1015_CFG_COMP_QUE_MASK	GENMASK(1, 0)
#define ADS1015_CFG_COMP_LAT_MASK	BIT(2)
#define ADS1015_CFG_COMP_POL_MASK	BIT(3)
#define ADS1015_CFG_COMP_MODE_MASK	BIT(4)
#define ADS1015_CFG_DR_MASK	GENMASK(7, 5)
#define ADS1015_CFG_MOD_MASK	BIT(8)
#define ADS1015_CFG_PGA_MASK	GENMASK(11, 9)
#define ADS1015_CFG_MUX_MASK	GENMASK(14, 12)

/* Comparator queue and disable field */
#define ADS1015_CFG_COMP_DISABLE	3

/* Comparator polarity field */
#define ADS1015_CFG_COMP_POL_LOW	0
#define ADS1015_CFG_COMP_POL_HIGH	1

/* Comparator mode field */
#define ADS1015_CFG_COMP_MODE_TRAD	0
#define ADS1015_CFG_COMP_MODE_WINDOW	1

/* device operating modes */
#define ADS1015_CONTINUOUS	0
#define ADS1015_SINGLESHOT	1

#define ADS1015_SLEEP_DELAY_MS		2000
#define ADS1015_DEFAULT_PGA		21
#define ADS1015_DEFAULT_DATA_RATE	4
#define ADS1015_DEFAULT_CHAN		0

enum chip_ids {
	ADS1015,
	ADS1115,
};

extern const struct regmap_config ads1015_regmap_config;

int ads1015_core_probe(struct device *dev, struct regmap *regmap,
		       const char *name, int irq, unsigned int chip);
int ads1015_core_remove(struct device *dev);

extern const struct dev_pm_ops ads1015_pm_ops;

#endif /* _TI_ADS1015_H_ */
