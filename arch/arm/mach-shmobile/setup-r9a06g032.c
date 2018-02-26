/*
 * RZ/N1 processor support file
 *
 * Copyright (C) 2018 Renesas Electronics Europe Limited
 *
 * Michel Pollet <michel.pollet@bp.renesas.com>, <buserror@gmail.com>
 *
 */
 /* SPDX-License-Identifier: GPL-2.0 */

#include <asm/mach/arch.h>
#include <dt-bindings/soc/renesas,rzn1-map.h>
#include <linux/kernel.h>
#include <linux/io.h>
#include <linux/of_platform.h>
#include <soc/rzn1/sysctrl.h>

static void __iomem *sysctrl_base_addr;

static void rzn1_sysctrl_init(void)
{
	if (sysctrl_base_addr)
		return;
	sysctrl_base_addr = ioremap(RZN1_SYSTEM_CTRL_BASE,
					RZN1_SYSTEM_CTRL_SIZE);
	BUG_ON(!sysctrl_base_addr);
}

void __iomem *rzn1_sysctrl_base(void)
{
	if (!sysctrl_base_addr)
		rzn1_sysctrl_init();
	return sysctrl_base_addr;
}
EXPORT_SYMBOL(rzn1_sysctrl_base);

static void rzn1_restart(enum reboot_mode mode, const char *cmd)
{
	rzn1_sysctrl_writel(
			rzn1_sysctrl_readl(RZN1_SYSCTRL_REG_RSTEN) |
			BIT(RZN1_SYSCTRL_REG_RSTEN_SWRST_EN) |
				BIT(RZN1_SYSCTRL_REG_RSTEN_MRESET_EN),
			RZN1_SYSCTRL_REG_RSTEN);
	rzn1_sysctrl_writel(
			rzn1_sysctrl_readl(RZN1_SYSCTRL_REG_RSTCTRL) |
			BIT(RZN1_SYSCTRL_REG_RSTCTRL_SWRST_REQ),
			RZN1_SYSCTRL_REG_RSTCTRL);
}

#ifdef CONFIG_USE_OF
static const char *rzn1_boards_compat_dt[] __initconst = {
	"renesas,r9a06g032",
	NULL,
};

DT_MACHINE_START(RZN1_DT, "Renesas RZ/N1 (Device Tree)")
	.dt_compat      = rzn1_boards_compat_dt,
	.restart	= rzn1_restart,
MACHINE_END
#endif /* CONFIG_USE_OF */
