/*
 * Copyright (c) 2015 National Instruments Corp.
 *
 * Pinctrl Driver for Ettus Research E3XX series daughterboards
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

#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/io.h>
#include <linux/pinctrl/pinctrl.h>
#include <linux/platform_device.h>
#include <linux/pinctrl/pinctrl.h>
#include <linux/pinctrl/pinconf.h>
#include <linux/pinctrl/pinconf-generic.h>
#include "pinctrl-utils.h"
#include "core.h"

#define E3XX_NUM_DB_PINS 120
#define E3XX_PINS_PER_REG 32

#define E3XX_DDR_OFFSET 0x00
#define E3XX_OUT_OFFSET 0x20

static const struct pinctrl_pin_desc e3xx_pins[] = {
	/* pin0 doesn't exist */
	PINCTRL_PIN(1, "DB_1"),
	PINCTRL_PIN(2, "DB_2"),
	PINCTRL_PIN(3, "DB_3"),
	PINCTRL_PIN(4, "DB_4"),
	PINCTRL_PIN(5, "DB_5"),
	PINCTRL_PIN(6, "DB_6"),
	PINCTRL_PIN(7, "DB_7"),
	PINCTRL_PIN(8, "DB_8"),
	PINCTRL_PIN(9, "DB_9"),
	PINCTRL_PIN(10, "DB_10"),
	PINCTRL_PIN(11, "DB_11"),
	PINCTRL_PIN(12, "DB_12"),
	PINCTRL_PIN(13, "DB_13"),
	PINCTRL_PIN(14, "DB_14"),
	PINCTRL_PIN(15, "DB_15"),
	PINCTRL_PIN(16, "DB_16"),
	PINCTRL_PIN(17, "DB_17"),
	PINCTRL_PIN(18, "DB_18"),
	PINCTRL_PIN(19, "DB_19"),
	PINCTRL_PIN(20, "DB_20"),
	PINCTRL_PIN(21, "DB_21"),
	PINCTRL_PIN(22, "DB_22"),
	PINCTRL_PIN(23, "DB_23"),
	PINCTRL_PIN(24, "DB_24"),
	PINCTRL_PIN(25, "DB_25"),
	PINCTRL_PIN(26, "DB_26"),
	PINCTRL_PIN(27, "DB_27"),
	PINCTRL_PIN(28, "DB_28"),
	PINCTRL_PIN(29, "DB_29"),
	PINCTRL_PIN(30, "DB_30"),
	PINCTRL_PIN(31, "DB_31"),
	PINCTRL_PIN(32, "DB_32"),
	PINCTRL_PIN(33, "DB_33"),
	PINCTRL_PIN(34, "DB_34"),
	PINCTRL_PIN(35, "DB_35"),
	PINCTRL_PIN(36, "DB_36"),
	PINCTRL_PIN(37, "DB_37"),
	PINCTRL_PIN(38, "DB_38"),
	PINCTRL_PIN(39, "DB_39"),
	PINCTRL_PIN(40, "DB_40"),
	PINCTRL_PIN(41, "DB_41"),
	PINCTRL_PIN(42, "DB_42"),
	PINCTRL_PIN(43, "DB_43"),
	PINCTRL_PIN(44, "DB_44"),
	PINCTRL_PIN(45, "DB_45"),
	PINCTRL_PIN(46, "DB_46"),
	PINCTRL_PIN(47, "DB_47"),
	PINCTRL_PIN(48, "DB_48"),
	PINCTRL_PIN(49, "DB_49"),
	PINCTRL_PIN(50, "DB_50"),
	PINCTRL_PIN(52, "DB_52"),
	PINCTRL_PIN(53, "DB_53"),
	PINCTRL_PIN(54, "DB_54"),
	PINCTRL_PIN(55, "DB_55"),
	PINCTRL_PIN(56, "DB_56"),
	PINCTRL_PIN(57, "DB_57"),
	PINCTRL_PIN(58, "DB_58"),
	PINCTRL_PIN(59, "DB_59"),
	PINCTRL_PIN(60, "DB_60"),
	PINCTRL_PIN(61, "DB_61"),
	PINCTRL_PIN(62, "DB_62"),
	PINCTRL_PIN(63, "DB_63"),
	PINCTRL_PIN(64, "DB_64"),
	PINCTRL_PIN(65, "DB_65"),
	PINCTRL_PIN(66, "DB_66"),
	PINCTRL_PIN(67, "DB_67"),
	PINCTRL_PIN(68, "DB_68"),
	PINCTRL_PIN(69, "DB_69"),
	PINCTRL_PIN(70, "DB_70"),
	PINCTRL_PIN(71, "DB_71"),
	PINCTRL_PIN(72, "DB_72"),
	PINCTRL_PIN(73, "DB_73"),
	PINCTRL_PIN(74, "DB_74"),
	PINCTRL_PIN(75, "DB_75"),
	PINCTRL_PIN(76, "DB_76"),
	PINCTRL_PIN(77, "DB_77"),
	PINCTRL_PIN(78, "DB_78"),
	PINCTRL_PIN(79, "DB_79"),
	PINCTRL_PIN(80, "DB_80"),
	PINCTRL_PIN(81, "DB_81"),
	PINCTRL_PIN(82, "DB_82"),
	PINCTRL_PIN(83, "DB_83"),
	PINCTRL_PIN(84, "DB_84"),
	PINCTRL_PIN(85, "DB_85"),
	PINCTRL_PIN(86, "DB_86"),
	PINCTRL_PIN(87, "DB_87"),
	PINCTRL_PIN(88, "DB_88"),
	PINCTRL_PIN(89, "DB_89"),
	PINCTRL_PIN(90, "DB_90"),
	PINCTRL_PIN(91, "DB_91"),
	PINCTRL_PIN(92, "DB_92"),
	PINCTRL_PIN(93, "DB_93"),
	PINCTRL_PIN(94, "DB_94"),
	PINCTRL_PIN(95, "DB_95"),
	PINCTRL_PIN(96, "DB_96"),
	PINCTRL_PIN(97, "DB_97"),
	PINCTRL_PIN(98, "DB_98"),
	PINCTRL_PIN(99, "DB_99"),
	PINCTRL_PIN(100, "DB_100"),
	PINCTRL_PIN(101, "DB_101"),
	PINCTRL_PIN(102, "DB_102"),
	PINCTRL_PIN(103, "DB_103"),
	PINCTRL_PIN(104, "DB_104"),
	PINCTRL_PIN(105, "DB_105"),
	PINCTRL_PIN(106, "DB_106"),
	PINCTRL_PIN(107, "DB_107"),
	PINCTRL_PIN(108, "DB_108"),
	PINCTRL_PIN(109, "DB_109"),
	PINCTRL_PIN(110, "DB_110"),
	PINCTRL_PIN(111, "DB_111"),
	PINCTRL_PIN(112, "DB_112"),
	PINCTRL_PIN(113, "DB_113"),
	PINCTRL_PIN(114, "DB_114"),
	PINCTRL_PIN(115, "DB_115"),
	PINCTRL_PIN(116, "DB_116"),
	PINCTRL_PIN(117, "DB_117"),
	PINCTRL_PIN(118, "DB_118"),
	PINCTRL_PIN(119, "DB_119"),
	PINCTRL_PIN(120, "DB_120"),
};

struct e3xx_pinctrl {
	struct clk *clk;
	struct device *dev;
	struct pinctrl_dev *pctl;

	void __iomem *io_base;

	const struct e3xx_pinctrl_group *groups;
};

static inline void e3xx_pinctrl_write(struct e3xx_pinctrl *pctl, u32 offset,
				      u32 val)
{
	writel_relaxed(val, pctl->io_base + offset);
}

static inline u32 e3xx_pinctrl_read(struct e3xx_pinctrl *pctl, u32 offset)
{
	return readl_relaxed(pctl->io_base + offset);
}

static int e3xx_pinctrl_get_groups_count(struct pinctrl_dev *pctldev)
{
	return 0;
}

static const char *e3xx_pinctrl_get_group_name(struct pinctrl_dev *pctldev,
					       unsigned selector)
{
	return NULL;
}

static int e3xx_pinctrl_get_group_pins(struct pinctrl_dev *pctldev,
				       unsigned selector,
				       const unsigned **pins,
				       unsigned *num_pins)
{
	return -ENOTSUPP;
}

static const struct pinctrl_ops e3xx_pctrl_ops = {
	.get_groups_count = e3xx_pinctrl_get_groups_count,
	.get_group_name = e3xx_pinctrl_get_group_name,
	.get_group_pins = e3xx_pinctrl_get_group_pins,
	.dt_node_to_map = pinconf_generic_dt_node_to_map_all,
	.dt_free_map = pinctrl_utils_dt_free_map,
};

static int e3xx_pinconf_cfg_get(struct pinctrl_dev *pctldev,
				unsigned pin,
				unsigned long *config)
{
	u32 reg, mask;
	int arg;
	struct e3xx_pinctrl *pctrl;
	unsigned int param;

	param = pinconf_to_config_param(*config);
	pctrl = pinctrl_dev_get_drvdata(pctldev);

	if (pin >= E3XX_NUM_DB_PINS)
		return -ENOTSUPP;

	mask = BIT(pin % E3XX_PINS_PER_REG);

	switch (param) {
	case PIN_CONFIG_OUTPUT:
		clk_enable(pctrl->clk);
		reg = e3xx_pinctrl_read(pctrl, E3XX_DDR_OFFSET +
					(pin / E3XX_PINS_PER_REG) * 4);

		clk_disable(pctrl->clk);
		arg = !!(reg & mask);
		break;
	default:
		dev_err(pctrl->dev, "requested illegal configuration");
		return -ENOTSUPP;
	};

	*config = pinconf_to_config_packed(param, arg);

	return 0;
}

static int e3xx_pinconf_cfg_set(struct pinctrl_dev *pctldev,
				unsigned pin,
				unsigned long *configs,
				unsigned num_configs)
{
	u32 reg, mask;
	int i;
	struct e3xx_pinctrl *pctrl;
	unsigned int param, arg;

	if (pin >= E3XX_NUM_DB_PINS)
		return -ENOTSUPP;
	mask = BIT(pin % E3XX_PINS_PER_REG);

	pctrl = pinctrl_dev_get_drvdata(pctldev);

	clk_enable(pctrl->clk);

	for (i = 0; i < num_configs; i++) {
		param = pinconf_to_config_param(configs[i]);
		arg = pinconf_to_config_argument(configs[i]);

		switch (param) {
		case PIN_CONFIG_OUTPUT:
			/* deal with value, set out bit if arg is 1 */
			reg = e3xx_pinctrl_read(pctrl, E3XX_OUT_OFFSET +
						((pin / E3XX_PINS_PER_REG) * 4))
				;
			reg &= ~mask;
			if (arg)
				reg |= mask;

			/* addresses need to be 4 byte aligned */
			e3xx_pinctrl_write(pctrl, E3XX_OUT_OFFSET +
					   ((pin / E3XX_PINS_PER_REG) * 4), reg)
				;

			/* set ddr bit to high for output */
			reg = e3xx_pinctrl_read(pctrl, E3XX_DDR_OFFSET +
						((pin / E3XX_PINS_PER_REG) * 4))
				;
			reg |= mask;

			/* addresses need to be 4 byte aligned */
			e3xx_pinctrl_write(pctrl, E3XX_DDR_OFFSET +
					  ((pin / E3XX_PINS_PER_REG) * 4), reg);
			break;

		default:
			clk_disable(pctrl->clk);
			return -ENOTSUPP;
		};
	}

	clk_disable(pctrl->clk);

	return 0;
}

static int e3xx_pinconf_group_set(struct pinctrl_dev *pctldev,
				  unsigned selector,
				  unsigned long *configs,
				  unsigned num_configs)
{
	return -EAGAIN;
}

static const struct pinconf_ops e3xx_pinconf_ops = {
	.is_generic = true,
	.pin_config_get = e3xx_pinconf_cfg_get,
	.pin_config_set = e3xx_pinconf_cfg_set,
	.pin_config_group_set = e3xx_pinconf_group_set,
};

static struct pinctrl_desc e3xx_desc = {
	.name = "e3xx_pinctrl",
	.pins = e3xx_pins,
	.npins = ARRAY_SIZE(e3xx_pins),
	.pctlops = &e3xx_pctrl_ops,
	.confops = &e3xx_pinconf_ops,
	.owner = THIS_MODULE,
};

static int e3xx_pinctrl_probe(struct platform_device *pdev)
{
	struct resource *res;
	struct e3xx_pinctrl *pctrl;
	int err;

	pctrl = devm_kzalloc(&pdev->dev, sizeof(*pctrl), GFP_KERNEL);
	if (!pctrl)
		return -ENOMEM;
	pctrl->dev = &pdev->dev;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		dev_err(&pdev->dev, "missing IO resource\n");
		return -ENODEV;
	}

	pctrl->io_base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(pctrl->io_base))
		return PTR_ERR(pctrl->io_base);

	pctrl->clk = devm_clk_get(&pdev->dev, NULL);
	if (IS_ERR(pctrl->clk)) {
		dev_err(&pdev->dev, "input clock not found");
		return PTR_ERR(pctrl->clk);
	}

	err = clk_prepare(pctrl->clk);
	if (err) {
		dev_err(&pdev->dev, "unable to prepare clock");
		return err;
	}

	pctrl->pctl = pinctrl_register(&e3xx_desc, &pdev->dev, pctrl);
	if (!pctrl->pctl)
		return -ENOMEM;

	platform_set_drvdata(pdev, pctrl);

	dev_info(&pdev->dev, "NI Ettus Research E3xx pinctrl initialized\n");

	return 0;
}

static int e3xx_pinctrl_remove(struct platform_device *pdev)
{
	struct e3xx_pinctrl *pctrl = platform_get_drvdata(pdev);

	pinctrl_unregister(pctrl->pctl);

	clk_unprepare(pctrl->clk);

	return 0;
}

#ifdef CONFIG_OF
static const struct of_device_id e3xx_pinctrl_of_match[] = {
	{ .compatible = "ettus,e3xx-pinctrl-1.0", },
	{},
};

MODULE_DEVICE_TABLE(of, e3xx_pinctrl_of_match);
#endif

static struct platform_driver e3xx_pinctrl_driver = {
	.probe = e3xx_pinctrl_probe,
	.remove = e3xx_pinctrl_remove,
	.driver = {
		.name = "e3xx_pinctrl",
		.of_match_table = of_match_ptr(e3xx_pinctrl_of_match),
	},
};

module_platform_driver(e3xx_pinctrl_driver);

MODULE_AUTHOR("Moritz Fischer <moritz.fischer@ettus.com>");
MODULE_DESCRIPTION("Ettus Research pinctrl driver");
MODULE_LICENSE("GPL v2");
