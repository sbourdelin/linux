/*
 * Copyright (C) 2012 Texas Instruments Incorporated - http://www.ti.com/
 *
 * Modified from mach-omap/omap2/board-generic.c
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/io.h>
#include <linux/of_irq.h>
#include <linux/of_platform.h>
#include <linux/irqdomain.h>

#include <asm/mach/arch.h>

#include <mach/common.h>
#include "cp_intc.h"
#include <mach/da8xx.h>

static struct of_dev_auxdata da850_auxdata_lookup[] __initdata = {
	OF_DEV_AUXDATA("ti,davinci-i2c", 0x01c22000, "i2c_davinci.1", NULL),
	OF_DEV_AUXDATA("ti,davinci-i2c", 0x01e28000, "i2c_davinci.2", NULL),
	OF_DEV_AUXDATA("ti,davinci-wdt", 0x01c21000, "davinci-wdt", NULL),
	OF_DEV_AUXDATA("ti,da830-mmc", 0x01c40000, "da830-mmc.0", NULL),
	OF_DEV_AUXDATA("ti,da850-ehrpwm", 0x01f00000, "ehrpwm", NULL),
	OF_DEV_AUXDATA("ti,da850-ehrpwm", 0x01f02000, "ehrpwm", NULL),
	OF_DEV_AUXDATA("ti,da850-ecap", 0x01f06000, "ecap", NULL),
	OF_DEV_AUXDATA("ti,da850-ecap", 0x01f07000, "ecap", NULL),
	OF_DEV_AUXDATA("ti,da850-ecap", 0x01f08000, "ecap", NULL),
	OF_DEV_AUXDATA("ti,da830-spi", 0x01c41000, "spi_davinci.0", NULL),
	OF_DEV_AUXDATA("ti,da830-spi", 0x01f0e000, "spi_davinci.1", NULL),
	OF_DEV_AUXDATA("ns16550a", 0x01c42000, "serial8250.0", NULL),
	OF_DEV_AUXDATA("ns16550a", 0x01d0c000, "serial8250.1", NULL),
	OF_DEV_AUXDATA("ns16550a", 0x01d0d000, "serial8250.2", NULL),
	OF_DEV_AUXDATA("ti,davinci_mdio", 0x01e24000, "davinci_mdio.0", NULL),
	OF_DEV_AUXDATA("ti,davinci-dm6467-emac", 0x01e20000, "davinci_emac.1",
		       NULL),
	OF_DEV_AUXDATA("ti,da830-mcasp-audio", 0x01d00000, "davinci-mcasp.0", NULL),
	OF_DEV_AUXDATA("ti,da850-aemif", 0x68000000, "ti-aemif", NULL),
	OF_DEV_AUXDATA("ti,am33xx-tilcdc", 0x01e13000, "da8xx_lcdc.0", NULL),
	{}
};

#ifdef CONFIG_ARCH_DAVINCI_DA850

/*
 * Adjust the default memory settings to cope with the LCDC
 *
 * REVISIT: This issue occurs on other davinci boards as well. Find
 * a proper system-wide fix.
 */
static void da850_lcdc_adjust_memory_bandwidth(void)
{
	void __iomem *cfg_mstpri1_base;
	void __iomem *cfg_mstpri2_base;
	void __iomem *emifb;
	u32 val;

	/*
	 * Default master priorities in reg 0 are all lower by default than LCD
	 * which is set below to 0. Hence don't need to change here.
	 */

	/* set EDMA30TC0 and TC1 to lower than LCDC (4 < 0) */
	cfg_mstpri1_base = DA8XX_SYSCFG0_VIRT(DA8XX_MSTPRI1_REG);
	val = __raw_readl(cfg_mstpri1_base);
	val &= 0xFFFF00FF;
	val |= 4 << 8;             /* 0-high, 7-low priority*/
	val |= 4 << 12;            /* 0-high, 7-low priority*/
	__raw_writel(val, cfg_mstpri1_base);

	/*
	 * Reconfigure the LCDC priority to the highest to ensure that
	 * the throughput/latency requirements for the LCDC are met.
	 */
	cfg_mstpri2_base = DA8XX_SYSCFG0_VIRT(DA8XX_MSTPRI2_REG);

	val = __raw_readl(cfg_mstpri2_base);
	val &= 0x0fffffff;
	__raw_writel(val, cfg_mstpri2_base);

	/* set BPRIO */
	emifb = ioremap(DA8XX_DDR_CTL_BASE, SZ_4K);
	__raw_writel(0x20, emifb + DA8XX_PBBPR_REG);
	iounmap(emifb);
}

static void __init da850_init_machine(void)
{
	of_platform_default_populate(NULL, da850_auxdata_lookup, NULL);
	da850_lcdc_adjust_memory_bandwidth();
}

static const char *const da850_boards_compat[] __initconst = {
	"enbw,cmc",
	"ti,da850-lcdk",
	"ti,da850-evm",
	"ti,da850",
	NULL,
};

DT_MACHINE_START(DA850_DT, "Generic DA850/OMAP-L138/AM18x")
	.map_io		= da850_init,
	.init_time	= davinci_timer_init,
	.init_machine	= da850_init_machine,
	.dt_compat	= da850_boards_compat,
	.init_late	= davinci_init_late,
	.restart	= da8xx_restart,
MACHINE_END

#endif
