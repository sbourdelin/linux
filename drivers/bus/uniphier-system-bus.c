/*
 * Copyright (C) 2015 Masahiro Yamada <yamada.masahiro@socionext.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/io.h>
#include <linux/log2.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/sort.h>

#define UNIPHIER_SBC_NR_BANKS	8	/* number of banks (chip select) */

#define UNIPHIER_SBC_BASE	0x100	/* base address of bank0 space */
#define    UNIPHIER_SBC_BASE_BE		BIT(0)	/* bank_enable */
#define UNIPHIER_SBC_CTRL0	0x200	/* timing parameter 0 of bank0 */
#define UNIPHIER_SBC_CTRL1	0x204	/* timing parameter 1 of bank0 */
#define UNIPHIER_SBC_CTRL2	0x208	/* timing parameter 2 of bank0 */
#define UNIPHIER_SBC_CTRL3	0x20c	/* timing parameter 3 of bank0 */
#define UNIPHIER_SBC_CTRL4	0x300	/* timing parameter 4 of bank0 */

#define UNIPHIER_SBC_STRIDE	0x10	/* register stride to next bank */

struct uniphier_sbc_bank {
	u32 base;
	u32 end;
};

struct uniphier_sbc_priv {
	struct device *dev;
	void __iomem *membase;
	struct uniphier_sbc_bank bank[UNIPHIER_SBC_NR_BANKS];
};

static void uniphier_sbc_set_reg(struct uniphier_sbc_priv *priv)
{
	void __iomem *base_reg = priv->membase + UNIPHIER_SBC_BASE;
	u32 base, end, mask, val;
	int i;

	for (i = 0; i < ARRAY_SIZE(priv->bank); i++) {
		base = priv->bank[i].base;
		end = priv->bank[i].end;

		if (base == end)
			continue;

		mask = base ^ (end - 1);

		val = base & 0xfffe0000;
		val |= (~mask >> 16) & 0xfffe;
		val |= UNIPHIER_SBC_BASE_BE;

		dev_dbg(priv->dev, "SBC_BASE[%d] = 0x%08x\n", i, val);

		writel(val, base_reg + UNIPHIER_SBC_STRIDE * i);
	}
}

static void uniphier_sbc_check_boot_swap(struct uniphier_sbc_priv *priv)
{
	void __iomem *base_reg = priv->membase + UNIPHIER_SBC_BASE;
	int is_swapped;

	is_swapped = !(readl(base_reg) & UNIPHIER_SBC_BASE_BE);

	dev_dbg(priv->dev, "Boot Swap: %s\n", is_swapped ? "on" : "off");

	if (is_swapped)
		swap(priv->bank[0], priv->bank[1]);
}

static int uniphier_sbc_get_cells(struct device_node *np, int *child_addrc,
				  int *addrc, int *sizec)
{
	u32 cells;
	int ret;

	*addrc = of_n_addr_cells(np);
	*sizec = of_n_size_cells(np);

	ret = of_property_read_u32(np, "#address-cells", &cells);
	if (ret)
		return ret;

	*child_addrc = cells;
	if (*child_addrc <= 1)
		return -EINVAL;

	ret = of_property_read_u32(np, "#size-cells", &cells);
	if (!ret && cells != *sizec)
		return -EINVAL;

	return 0;
}

static int uniphier_sbc_add_bank(struct uniphier_sbc_priv *priv, int bank,
				 u64 child_addr, u64 addr, u64 size)
{
	u64 end, mask;

	dev_dbg(priv->dev,
		"range found: bank = %d, caddr = %08llx, addr = %08llx, size = %08llx\n",
		bank, child_addr, addr, size);

	if (bank >= ARRAY_SIZE(priv->bank)) {
		dev_err(priv->dev, "unsupported bank number %d\n", bank);
		return -EINVAL;
	}

	if (priv->bank[bank].base || priv->bank[bank].end) {
		dev_err(priv->dev,
			"range for bank %d has already been specified\n", bank);
		return -EINVAL;
	}

	if (addr > U32_MAX) {
		dev_err(priv->dev, "base address %llx is too high\n", addr);
		return -EINVAL;
	}

	end = addr + size;

	if (child_addr > addr) {
		dev_err(priv->dev,
			"base %llx cannot be mapped to %llx of parent\n",
			child_addr, addr);
		return -EINVAL;
	}
	addr -= child_addr;

	addr = round_down(addr, 0x00020000);
	end = round_up(end, 0x00020000);

	if (end > U32_MAX) {
		dev_err(priv->dev, "end address %llx is too high\n", end);
		return -EINVAL;
	}
	mask = addr ^ (end - 1);
	mask = roundup_pow_of_two(mask);

	addr = round_down(addr, mask);
	end = round_up(end, mask);

	priv->bank[bank].base = addr;
	priv->bank[bank].end = end;

	dev_dbg(priv->dev, "range added: bank = %d, addr = %08x, end = %08x\n",
		bank, priv->bank[bank].base, priv->bank[bank].end);

	return 0;
}

static int uniphier_sbc_sort_cmp(const void *a, const void *b)
{
	return ((struct uniphier_sbc_bank *)a)->base
		- ((struct uniphier_sbc_bank *)b)->base;
}

static int uniphier_sbc_check_overlap(struct uniphier_sbc_priv tmp)
{
	int i;

	sort(&tmp.bank, ARRAY_SIZE(tmp.bank), sizeof(tmp.bank[0]),
	     uniphier_sbc_sort_cmp, NULL);

	for (i = 0; i < ARRAY_SIZE(tmp.bank) - 1; i++)
		if (tmp.bank[i].end > tmp.bank[i + 1].base) {
			dev_err(tmp.dev,
				"region overlap between %08x-%08x and %08x-%08x\n",
				tmp.bank[i].base, tmp.bank[i].end,
				tmp.bank[i + 1].base, tmp.bank[i + 1].end);
			return -EINVAL;
		}

	return 0;
}

static int uniphier_sbc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct uniphier_sbc_priv *priv;
	struct resource *regs;
	struct device_node *bus_np;
	int child_addrc, addrc, sizec, bank;
	u64 child_addr, addr, size;
	const __be32 *ranges;
	int rlen, rone, ret;

	bus_np = of_find_compatible_node(NULL, NULL,
					 "socionext,uniphier-system-bus");
	if (!bus_np)
		return 0;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv) {
		ret = -ENOMEM;
		goto out;
	}

	regs = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	priv->membase = devm_ioremap_resource(dev, regs);
	if (IS_ERR(priv->membase)) {
		ret = PTR_ERR(priv->membase);
		goto out;
	}

	priv->dev = dev;

	ret = uniphier_sbc_get_cells(bus_np, &child_addrc, &addrc, &sizec);
	if (ret) {
		dev_err(dev, "wrong #address-cells or #size-cells for bus\n");
		goto out;
	}

	ranges = of_get_property(bus_np, "ranges", &rlen);
	if (!ranges) {
		ret = -ENOENT;
		goto out;
	}

	rlen /= sizeof(*ranges);
	rone = child_addrc + addrc + sizec;

	for (; rlen >= rone; rlen -= rone, ranges += rone) {
		bank = be32_to_cpup(ranges);
		child_addr = of_read_number(ranges + 1, child_addrc - 1);
		addr = of_translate_address(bus_np, ranges + child_addrc);
		if (addr == OF_BAD_ADDR) {
			ret = -EINVAL;
			goto out;
		}
		size = of_read_number(ranges + child_addrc + addrc, sizec);

		ret = uniphier_sbc_add_bank(priv, bank, child_addr, addr, size);
		if (ret)
			goto out;
	}

	ret = uniphier_sbc_check_overlap(*priv);
	if (ret)
		goto out;

	uniphier_sbc_check_boot_swap(priv);

	uniphier_sbc_set_reg(priv);

out:
	of_node_put(bus_np);

	return ret;
}



static const struct of_device_id uniphier_sbc_match[] = {
	{ .compatible = "socionext,uniphier-system-bus-controller" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, uniphier_sbc_match);

static struct platform_driver uniphier_sbc_driver = {
	.probe		= uniphier_sbc_probe,
	.driver = {
		.name	= "system-bus-controller",
		.of_match_table = uniphier_sbc_match,
	},
};
module_platform_driver(uniphier_sbc_driver);

MODULE_AUTHOR("Masahiro Yamada <yamada.masahiro@socionext.com>");
MODULE_DESCRIPTION("UniPhier System Bus Controller driver");
MODULE_LICENSE("GPL");
