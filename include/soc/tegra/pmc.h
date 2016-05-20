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

/*
 * TEGRA_IO_PAD: The IO pins of Tegra SoCs are grouped for common control
 * of IO interface like setting voltage signal levels, power state of the
 * interface. The group is generally referred as io-pads. The power and
 * voltage control of IO pins are available at io-pads level.
 * The following macros make the super list all IO pads found on Tegra SoC
 * generations.
 */
enum tegra_io_pads {
	TEGRA_IO_PADS_AUDIO,
	TEGRA_IO_PADS_AUDIO_HV,
	TEGRA_IO_PADS_BB,
	TEGRA_IO_PADS_CAM,
	TEGRA_IO_PADS_COMP,
	TEGRA_IO_PADS_CSIA,
	TEGRA_IO_PADS_CSIB,
	TEGRA_IO_PADS_CSIC,
	TEGRA_IO_PADS_CSID,
	TEGRA_IO_PADS_CSIE,
	TEGRA_IO_PADS_CSIF,
	TEGRA_IO_PADS_DBG,
	TEGRA_IO_PADS_DEBUG_NONAO,
	TEGRA_IO_PADS_DMIC,
	TEGRA_IO_PADS_DP,
	TEGRA_IO_PADS_DSI,
	TEGRA_IO_PADS_DSIB,
	TEGRA_IO_PADS_DSIC,
	TEGRA_IO_PADS_DSID,
	TEGRA_IO_PADS_EMMC,
	TEGRA_IO_PADS_EMMC2,
	TEGRA_IO_PADS_GPIO,
	TEGRA_IO_PADS_HDMI,
	TEGRA_IO_PADS_HSIC,
	TEGRA_IO_PADS_HV,
	TEGRA_IO_PADS_LVDS,
	TEGRA_IO_PADS_MIPI_BIAS,
	TEGRA_IO_PADS_NAND,
	TEGRA_IO_PADS_PEX_BIAS,
	TEGRA_IO_PADS_PEX_CLK1,
	TEGRA_IO_PADS_PEX_CLK2,
	TEGRA_IO_PADS_PEX_CNTRL,
	TEGRA_IO_PADS_SDMMC1,
	TEGRA_IO_PADS_SDMMC3,
	TEGRA_IO_PADS_SDMMC4,
	TEGRA_IO_PADS_SPI,
	TEGRA_IO_PADS_SPI_HV,
	TEGRA_IO_PADS_SYS_DDC,
	TEGRA_IO_PADS_UART,
	TEGRA_IO_PADS_USB0,
	TEGRA_IO_PADS_USB1,
	TEGRA_IO_PADS_USB2,
	TEGRA_IO_PADS_USB3,
	TEGRA_IO_PADS_USB_BIAS,

	/* Last entry */
	TEGRA_IO_PADS_MAX,
};

/* tegra_io_pads_vconf_voltage: The voltage level of IO rails which source
 *				the IO pads.
 */
enum tegra_io_pads_vconf_voltage {
	TEGRA_IO_PADS_VCONF_1800000UV,
	TEGRA_IO_PADS_VCONF_3300000UV,
};

#ifdef CONFIG_ARCH_TEGRA
int tegra_powergate_is_powered(unsigned int id);
int tegra_powergate_power_on(unsigned int id);
int tegra_powergate_power_off(unsigned int id);
int tegra_powergate_remove_clamping(unsigned int id);

/* Must be called with clk disabled, and returns with clk enabled */
int tegra_powergate_sequence_power_up(unsigned int id, struct clk *clk,
				      struct reset_control *rst);

/* Power enable/disable of the IO pads */
int tegra_io_pads_power_enable(enum tegra_io_pads id);
int tegra_io_pads_power_disable(enum tegra_io_pads id);
int tegra_io_pads_power_is_enabled(enum tegra_io_pads id);

/* Set/get Tegra IO pads voltage config registers */
int tegra_io_pads_set_voltage_config(enum tegra_io_pads id,
				     enum tegra_io_pads_vconf_voltage rail_uv);
int tegra_io_pads_get_voltage_config(enum tegra_io_pads id);

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

static inline int tegra_io_pads_power_enable(enum tegra_io_pads id)
{
	return -ENOTSUPP;
}

static inline int tegra_io_pads_power_disable(enum tegra_io_pads id)
{
	return -ENOTSUPP;
}

static inline int tegra_io_pads_power_is_enabled(enum tegra_io_pads id)
{
	return -ENOTSUPP;
}

static inline int tegra_io_pads_set_voltage_config(
			enum tegra_io_pads id,
			enum tegra_io_pads_vconf_voltage rail_uv)
{
	return -ENOTSUPP;
}

static inline int tegra_io_pads_get_voltage_config(enum tegra_io_pads id)
{
	return -ENOTSUPP;
}
#endif /* CONFIG_ARCH_TEGRA */

#endif /* __SOC_TEGRA_PMC_H__ */
