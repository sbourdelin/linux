/*
 * Copyright (c) 2015 HiSilicon Technologies Co., Ltd.
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
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/of_address.h>
#include <dt-bindings/clock/hi3519-clock.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include "clk.h"
#include "reset.h"

static struct hisi_fixed_rate_clock hi3519_fixed_rate_clks[] __initdata = {
	{ HI3519_FIXED_2376M, "2376m", NULL, CLK_IS_ROOT, 2376000000UL, },
	{ HI3519_FIXED_1188M, "1188m", NULL, CLK_IS_ROOT, 1188000000, },
	{ HI3519_FIXED_594M, "594m", NULL, CLK_IS_ROOT, 594000000, },
	{ HI3519_FIXED_297M, "297m", NULL, CLK_IS_ROOT, 297000000, },
	{ HI3519_FIXED_148P5M, "148p5m", NULL, CLK_IS_ROOT, 148500000, },
	{ HI3519_FIXED_74P25M, "74p25m", NULL, CLK_IS_ROOT, 74250000, },
	{ HI3519_FIXED_792M, "792m", NULL, CLK_IS_ROOT, 792000000, },
	{ HI3519_FIXED_475M, "475m", NULL, CLK_IS_ROOT, 475000000, },
	{ HI3519_FIXED_340M, "340m", NULL, CLK_IS_ROOT, 340000000, },
	{ HI3519_FIXED_72M, "72m", NULL, CLK_IS_ROOT, 72000000, },
	{ HI3519_FIXED_400M, "400m", NULL, CLK_IS_ROOT, 400000000, },
	{ HI3519_FIXED_200M, "200m", NULL, CLK_IS_ROOT, 200000000, },
	{ HI3519_FIXED_54M, "54m", NULL, CLK_IS_ROOT, 54000000, },
	{ HI3519_FIXED_27M, "27m", NULL, CLK_IS_ROOT, 1188000000, },
	{ HI3519_FIXED_37P125M, "37p125m", NULL, CLK_IS_ROOT, 37125000, },
	{ HI3519_FIXED_3000M, "3000m", NULL, CLK_IS_ROOT, 3000000000UL, },
	{ HI3519_FIXED_1500M, "1500m", NULL, CLK_IS_ROOT, 1500000000, },
	{ HI3519_FIXED_500M, "500m", NULL, CLK_IS_ROOT, 500000000, },
	{ HI3519_FIXED_250M, "250m", NULL, CLK_IS_ROOT, 250000000, },
	{ HI3519_FIXED_125M, "125m", NULL, CLK_IS_ROOT, 125000000, },
	{ HI3519_FIXED_1000M, "1000m", NULL, CLK_IS_ROOT, 1000000000, },
	{ HI3519_FIXED_600M, "600m", NULL, CLK_IS_ROOT, 600000000, },
	{ HI3519_FIXED_750M, "750m", NULL, CLK_IS_ROOT, 750000000, },
	{ HI3519_FIXED_150M, "150m", NULL, CLK_IS_ROOT, 150000000, },
	{ HI3519_FIXED_75M, "75m", NULL, CLK_IS_ROOT, 75000000, },
	{ HI3519_FIXED_300M, "300m", NULL, CLK_IS_ROOT, 300000000, },
	{ HI3519_FIXED_60M, "60m", NULL, CLK_IS_ROOT, 60000000, },
	{ HI3519_FIXED_214M, "214m", NULL, CLK_IS_ROOT, 214000000, },
	{ HI3519_FIXED_107M, "107m", NULL, CLK_IS_ROOT, 107000000, },
	{ HI3519_FIXED_100M, "100m", NULL, CLK_IS_ROOT, 100000000, },
	{ HI3519_FIXED_50M, "50m", NULL, CLK_IS_ROOT, 50000000, },
	{ HI3519_FIXED_25M, "25m", NULL, CLK_IS_ROOT, 25000000, },
	{ HI3519_FIXED_24M, "24m", NULL, CLK_IS_ROOT, 24000000, },
	{ HI3519_FIXED_3M, "3m", NULL, CLK_IS_ROOT, 3000000, },
};

static const char *sysaxi_mux_p[] __initconst = {"24m", "200m", };
static u32 sysaxi_mux_table[] = {0, 1};

static const char *fmc_mux_p[] __initconst = {
		"24m", "75m", "125m", "150m", "200m", "250m", "300m", "400m", };
static u32 fmc_mux_table[] = {0, 1, 2, 3, 4, 5, 6, 7};

static const char *i2c_mux_p[] __initconst = {"clk_sysapb", "50m"};
static u32 i2c_mux_table[] = {0, 1};

static struct hisi_mux_clock hi3519_mux_clks[] __initdata = {
	{ HI3519_SYSAXI_MUX, "sysaxi_mux", sysaxi_mux_p,
		ARRAY_SIZE(sysaxi_mux_p),
		CLK_SET_RATE_PARENT, 0x34, 12, 2, 0, sysaxi_mux_table, },
	{ HI3519_FMC_MUX, "fmc_mux", fmc_mux_p, ARRAY_SIZE(fmc_mux_p),
		CLK_SET_RATE_PARENT, 0xc0, 2, 3, 0, fmc_mux_table, },
	{ HI3519_I2C_MUX, "i2c_mux", i2c_mux_p, ARRAY_SIZE(i2c_mux_p),
		CLK_SET_RATE_PARENT, 0xe4, 26, 1, 0, i2c_mux_table, },
};

static struct hisi_fixed_factor_clock hi3519_fixed_factor_clks[] __initdata = {
	{ HI3519_SYSAPB_CLK, "clk_sysapb", "sysaxi_mux", 1, 4,
		CLK_SET_RATE_PARENT},
};

static struct hisi_gate_clock hi3519_gate_clks[] __initdata = {
	/* fmc */
	{ HI3519_FMC_CLK, "clk_fmc", "fmc_mux",
		CLK_SET_RATE_PARENT, 0xc0, 1, 0, },
	/* uart */
	{ HI3519_UART0_CLK, "clk_uart0", "24m",
		CLK_SET_RATE_PARENT, 0xe4, 20, 0, },
	{ HI3519_UART1_CLK, "clk_uart1", "24m",
		CLK_SET_RATE_PARENT, 0xe4, 21, 0, },
	{ HI3519_UART2_CLK, "clk_uart2", "24m",
		CLK_SET_RATE_PARENT, 0xe4, 22, 0, },
	{ HI3519_UART3_CLK, "clk_uart3", "24m",
		CLK_SET_RATE_PARENT, 0xe4, 23, 0, },
	{ HI3519_UART4_CLK, "clk_uart4", "24m",
		CLK_SET_RATE_PARENT, 0xe4, 24, 0, },
	/* ethernet mac */
	{ HI3519_ETH_CLK, "clk_eth", NULL,
		CLK_IS_ROOT, 0xcc, 1, 0, },
	{ HI3519_ETH_MACIF_CLK, "clk_eth_macif", NULL,
		CLK_IS_ROOT, 0xcc, 3, 0, },
};

static void __init hi3519_clk_init(struct device_node *np)
{
	struct hisi_clock_data *clk_data;

	clk_data = hisi_clk_init(np, HI3519_NR_CLKS);
	if (!clk_data)
		return;
	if (IS_ENABLED(CONFIG_RESET_CONTROLLER))
		hisi_reset_init(np, HI3519_NR_RSTS);

	hisi_clk_register_fixed_rate(hi3519_fixed_rate_clks,
				     ARRAY_SIZE(hi3519_fixed_rate_clks),
				     clk_data);
	hisi_clk_register_mux(hi3519_mux_clks, ARRAY_SIZE(hi3519_mux_clks),
					clk_data);
	hisi_clk_register_fixed_factor(hi3519_fixed_factor_clks,
			ARRAY_SIZE(hi3519_fixed_factor_clks), clk_data);
	hisi_clk_register_gate(hi3519_gate_clks,
			ARRAY_SIZE(hi3519_gate_clks), clk_data);
}

CLK_OF_DECLARE(hi3519_clk, "hisilicon,hi3519-clock", hi3519_clk_init);
