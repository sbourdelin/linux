/*
 * Copyright (c) 2010 Google, Inc
 * Copyright (c) 2014 NVIDIA Corporation
 *
 * Author:
 *	Colin Cross <ccross@google.com>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#ifndef __SOC_TEGRA_PMC_H__
#define __SOC_TEGRA_PMC_H__

#include <linux/reboot.h>

#include <soc/tegra/pm.h>

struct clk;
struct reset_control;

#ifdef CONFIG_PM_SLEEP
enum tegra_suspend_mode tegra_pmc_get_suspend_mode(void);
void tegra_pmc_set_suspend_mode(enum tegra_suspend_mode mode);
void tegra_pmc_enter_suspend_mode(enum tegra_suspend_mode mode);
#endif /* CONFIG_PM_SLEEP */

#ifdef CONFIG_SMP
bool tegra_pmc_cpu_is_powered(unsigned int cpuid);
int tegra_pmc_cpu_power_on(unsigned int cpuid);
int tegra_pmc_cpu_remove_clamping(unsigned int cpuid);
#endif /* CONFIG_SMP */

/*
 * powergate and I/O rail APIs
 */

#define TEGRA_POWERGATE_CPU	0
#define TEGRA_POWERGATE_3D	1
#define TEGRA_POWERGATE_VENC	2
#define TEGRA_POWERGATE_PCIE	3
#define TEGRA_POWERGATE_VDEC	4
#define TEGRA_POWERGATE_L2	5
#define TEGRA_POWERGATE_MPE	6
#define TEGRA_POWERGATE_HEG	7
#define TEGRA_POWERGATE_SATA	8
#define TEGRA_POWERGATE_CPU1	9
#define TEGRA_POWERGATE_CPU2	10
#define TEGRA_POWERGATE_CPU3	11
#define TEGRA_POWERGATE_CELP	12
#define TEGRA_POWERGATE_3D1	13
#define TEGRA_POWERGATE_CPU0	14
#define TEGRA_POWERGATE_C0NC	15
#define TEGRA_POWERGATE_C1NC	16
#define TEGRA_POWERGATE_SOR	17
#define TEGRA_POWERGATE_DIS	18
#define TEGRA_POWERGATE_DISB	19
#define TEGRA_POWERGATE_XUSBA	20
#define TEGRA_POWERGATE_XUSBB	21
#define TEGRA_POWERGATE_XUSBC	22
#define TEGRA_POWERGATE_VIC	23
#define TEGRA_POWERGATE_IRAM	24
#define TEGRA_POWERGATE_NVDEC	25
#define TEGRA_POWERGATE_NVJPG	26
#define TEGRA_POWERGATE_AUD	27
#define TEGRA_POWERGATE_DFD	28
#define TEGRA_POWERGATE_VE2	29
#define TEGRA_POWERGATE_MAX	TEGRA_POWERGATE_VE2

#define TEGRA_POWERGATE_3D0	TEGRA_POWERGATE_3D

#define TEGRA_IO_RAIL_CSIA	0
#define TEGRA_IO_RAIL_CSIB	1
#define TEGRA_IO_RAIL_DSI	2
#define TEGRA_IO_RAIL_MIPI_BIAS	3
#define TEGRA_IO_RAIL_PEX_BIAS	4
#define TEGRA_IO_RAIL_PEX_CLK1	5
#define TEGRA_IO_RAIL_PEX_CLK2	6
#define TEGRA_IO_RAIL_USB0	9
#define TEGRA_IO_RAIL_USB1	10
#define TEGRA_IO_RAIL_USB2	11
#define TEGRA_IO_RAIL_USB_BIAS	12
#define TEGRA_IO_RAIL_NAND	13
#define TEGRA_IO_RAIL_UART	14
#define TEGRA_IO_RAIL_BB	15
#define TEGRA_IO_RAIL_AUDIO	17
#define TEGRA_IO_RAIL_HSIC	19
#define TEGRA_IO_RAIL_COMP	22
#define TEGRA_IO_RAIL_HDMI	28
#define TEGRA_IO_RAIL_PEX_CNTRL	32
#define TEGRA_IO_RAIL_SDMMC1	33
#define TEGRA_IO_RAIL_SDMMC3	34
#define TEGRA_IO_RAIL_SDMMC4	35
#define TEGRA_IO_RAIL_CAM	36
#define TEGRA_IO_RAIL_RES	37
#define TEGRA_IO_RAIL_HV	38
#define TEGRA_IO_RAIL_DSIB	39
#define TEGRA_IO_RAIL_DSIC	40
#define TEGRA_IO_RAIL_DSID	41
#define TEGRA_IO_RAIL_CSIE	44
#define TEGRA_IO_RAIL_LVDS	57
#define TEGRA_IO_RAIL_SYS_DDC	58

/* TEGRA_IO_PAD: The IO pins of Tegra SoCs are grouped for common control
 * of IO interface like setting voltage signal levels, power state of the
 * interface. The group is generally referred as io-pads. The power and
 * voltage control of IO pins are available at io-pads level.
 * The following macros make the super list all IO pads found on Tegra SoC
 * generations.
 */
#define TEGRA_IO_PAD_AUDIO		0
#define TEGRA_IO_PAD_AUDIO_HV		1
#define TEGRA_IO_PAD_BB			2
#define TEGRA_IO_PAD_CAM		3
#define TEGRA_IO_PAD_COMP		4
#define TEGRA_IO_PAD_CSIA		5
#define TEGRA_IO_PAD_CSIB		6
#define TEGRA_IO_PAD_CSIC		7
#define TEGRA_IO_PAD_CSID		8
#define TEGRA_IO_PAD_CSIE		9
#define TEGRA_IO_PAD_CSIF		10
#define TEGRA_IO_PAD_DBG		11
#define TEGRA_IO_PAD_DEBUG_NONAO	12
#define TEGRA_IO_PAD_DMIC		13
#define TEGRA_IO_PAD_DP			14
#define TEGRA_IO_PAD_DSI		15
#define TEGRA_IO_PAD_DSIB		16
#define TEGRA_IO_PAD_DSIC		17
#define TEGRA_IO_PAD_DSID		18
#define TEGRA_IO_PAD_EMMC		19
#define TEGRA_IO_PAD_EMMC2		20
#define TEGRA_IO_PAD_GPIO		21
#define TEGRA_IO_PAD_HDMI		22
#define TEGRA_IO_PAD_HSIC		23
#define TEGRA_IO_PAD_HV			24
#define TEGRA_IO_PAD_LVDS		25
#define TEGRA_IO_PAD_MIPI_BIAS		26
#define TEGRA_IO_PAD_NAND		27
#define TEGRA_IO_PAD_PEX_BIAS		28
#define TEGRA_IO_PAD_PEX_CLK1		29
#define TEGRA_IO_PAD_PEX_CLK2		30
#define TEGRA_IO_PAD_PEX_CNTRL		31
#define TEGRA_IO_PAD_SDMMC1		32
#define TEGRA_IO_PAD_SDMMC3		33
#define TEGRA_IO_PAD_SDMMC4		34
#define TEGRA_IO_PAD_SPI		35
#define TEGRA_IO_PAD_SPI_HV		36
#define TEGRA_IO_PAD_SYS_DDC		37
#define TEGRA_IO_PAD_UART		38
#define TEGRA_IO_PAD_USB0		39
#define TEGRA_IO_PAD_USB1		40
#define TEGRA_IO_PAD_USB2		41
#define TEGRA_IO_PAD_USB3		42
#define TEGRA_IO_PAD_USB_BIAS		43

#ifdef CONFIG_ARCH_TEGRA
int tegra_powergate_is_powered(unsigned int id);
int tegra_powergate_power_on(unsigned int id);
int tegra_powergate_power_off(unsigned int id);
int tegra_powergate_remove_clamping(unsigned int id);

/* Must be called with clk disabled, and returns with clk enabled */
int tegra_powergate_sequence_power_up(unsigned int id, struct clk *clk,
				      struct reset_control *rst);

int tegra_io_rail_power_on(unsigned int id);
int tegra_io_rail_power_off(unsigned int id);

/* Power enable/disable of the IO pads */
int tegra_io_pads_power_enable(int io_pad_id);
int tegra_io_pads_power_disable(int io_pad_id);
int tegra_io_pads_power_is_enabled(int io_pad_id);

/* Set/Get of IO pad voltage */
int tegra_io_pads_configure_voltage(int io_pad_id, int io_volt_uv);
int tegra_io_pads_get_configured_voltage(int io_pad_id);

#else
static inline int tegra_powergate_is_powered(unsigned int id)
{
	return -ENOSYS;
}

static inline int tegra_powergate_power_on(unsigned int id)
{
	return -ENOSYS;
}

static inline int tegra_powergate_power_off(unsigned int id)
{
	return -ENOSYS;
}

static inline int tegra_powergate_remove_clamping(unsigned int id)
{
	return -ENOSYS;
}

static inline int tegra_powergate_sequence_power_up(unsigned int id,
						    struct clk *clk,
						    struct reset_control *rst)
{
	return -ENOSYS;
}

static inline int tegra_io_rail_power_on(unsigned int id)
{
	return -ENOSYS;
}

static inline int tegra_io_rail_power_off(unsigned int id)
{
	return -ENOSYS;
}

static inline int tegra_io_pads_power_enable(int io_pad_id)
{
	return -ENOTSUPP;
}

static inline int tegra_io_pads_power_disable(int io_pad_id)
{
	return -ENOTSUPP;
}

static inline int tegra_io_pads_power_is_enabled(int io_pad_id)
{
	return -ENOTSUPP;
}

static inline int tegra_io_pads_configure_voltage(int io_pad_id, int io_volt_uv)
{
	return -ENOTSUPP;
}

static inline int tegra_io_pads_get_configured_voltage(int io_pad_id)
{
	return -ENOTSUPP;
}
#endif /* CONFIG_ARCH_TEGRA */

#endif /* __SOC_TEGRA_PMC_H__ */
