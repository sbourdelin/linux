/*
 * ARC HSDK Platform support code
 *
 * Copyright (C) 2017 Synopsys, Inc. (www.synopsys.com)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/init.h>
#include <linux/smp.h>
#include <asm/arcregs.h>
#include <asm/io.h>
#include <asm/mach_desc.h>

#define ARC_CCM_UNUSED_ADDR	0x60000000

static void __init hsdk_init_per_cpu(unsigned int cpu)
{
	/*
	 * By default ICCM is mapped to 0x7z while this area is used for
	 * kernel virtual mappings, so move it to currently unused area.
	 */
	if (cpuinfo_arc700[cpu].iccm.sz)
		write_aux_reg(ARC_REG_AUX_ICCM, ARC_CCM_UNUSED_ADDR);

	/*
	 * By default DCCM is mapped to 0x8z while this area is used by kernel,
	 * so move it to currently unused area.
	 */
	if (cpuinfo_arc700[cpu].dccm.sz)
		write_aux_reg(ARC_REG_AUX_DCCM, ARC_CCM_UNUSED_ADDR);
}

#define ARC_PERIPHERAL_BASE	0xf0000000
#define CREG_BASE		(ARC_PERIPHERAL_BASE + 0x1000)
#define CREG_PAE		(CREG_BASE + 0x180)
#define CREG_PAE_UPDATE		(CREG_BASE + 0x194)

#define SDIO_BASE		(ARC_PERIPHERAL_BASE + 0xA000)
#define SDIO_UHS_REG_EXT	(SDIO_BASE + 0x108)
#define SDIO_UHS_REG_EXT_DIV_2	(2 << 30)

#define HSDK_GPIO_INTC          (ARC_PERIPHERAL_BASE + 0x3000)
#define GPIO_INTEN              (HSDK_GPIO_INTC + 0x30)
#define GPIO_INTMASK            (HSDK_GPIO_INTC + 0x34)
#define GPIO_INTTYPE_LEVEL      (HSDK_GPIO_INTC + 0x38)
#define GPIO_INT_POLARITY       (HSDK_GPIO_INTC + 0x3c)

#define GPIO_BLUETOOTH_INT	0x00000001
#define GPIO_HAPS_INT		0x00000004
#define GPIO_AUDIO_INT		0x00000008
/* PMOD_A header */
#define GPIO_PIN_08_INT		0x00000100
#define GPIO_PIN_09_INT		0x00000200
#define GPIO_PIN_10_INT		0x00000400
#define GPIO_PIN_11_INT		0x00000800
/* PMOD_B header */
#define GPIO_PIN_12_INT		0x00001000
#define GPIO_PIN_13_INT		0x00002000
#define GPIO_PIN_14_INT		0x00004000
#define GPIO_PIN_15_INT		0x00008000
/* PMOD_C header */
#define GPIO_PIN_16_INT		0x00010000
#define GPIO_PIN_17_INT		0x00020000
#define GPIO_PIN_18_INT		0x00040000
#define GPIO_PIN_19_INT		0x00080000
#define GPIO_PIN_20_INT		0x00100000
#define GPIO_PIN_21_INT		0x00200000
#define GPIO_PIN_22_INT		0x00400000
#define GPIO_PIN_23_INT		0x00800000
static void __init hsdk_enable_gpio_intc_wire(void)
{
	u32 val = GPIO_HAPS_INT;

	iowrite32(0xffffffff, (void __iomem *) GPIO_INTMASK);
	iowrite32(~val, (void __iomem *) GPIO_INTMASK);
	iowrite32(0x00000000, (void __iomem *) GPIO_INTTYPE_LEVEL);
	iowrite32(0xffffffff, (void __iomem *) GPIO_INT_POLARITY);
	iowrite32(val, (void __iomem *) GPIO_INTEN);
}

static void __init hsdk_init_early(void)
{
	/*
	 * PAE remapping for DMA clients does not work due to an RTL bug, so
	 * CREG_PAE register must be programmed to all zeroes, otherwise it
	 * will cause problems with DMA to/from peripherals even if PAE40 is
	 * not used.
	 */

	/* Default is 1, which means "PAE offset = 4GByte" */
	writel_relaxed(0, (void __iomem *) CREG_PAE);

	/* Really apply settings made above */
	writel(1, (void __iomem *) CREG_PAE_UPDATE);

	/*
	 * Switch SDIO external ciu clock divider from default div-by-8 to
	 * minimum possible div-by-2.
	 */
	iowrite32(SDIO_UHS_REG_EXT_DIV_2, (void __iomem *) SDIO_UHS_REG_EXT);

	sdk_enable_gpio_intc_wire();
}

static const char *hsdk_compat[] __initconst = {
	"snps,hsdk",
	NULL,
};

MACHINE_START(SIMULATION, "hsdk")
	.dt_compat	= hsdk_compat,
	.init_early     = hsdk_init_early,
	.init_per_cpu	= hsdk_init_per_cpu,
MACHINE_END
