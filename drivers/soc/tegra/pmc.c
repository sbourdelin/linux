/*
 * drivers/soc/tegra/pmc.c
 *
 * Copyright (c) 2010 Google, Inc
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

#define pr_fmt(fmt) "tegra-pmc: " fmt

#include <linux/kernel.h>
#include <linux/clk.h>
#include <linux/clk/tegra.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/export.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_clk.h>
#include <linux/of_platform.h>
#include <linux/pinctrl/pinctrl.h>
#include <linux/pinctrl/pinconf-generic.h>
#include <linux/pinctrl/pinconf.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/reboot.h>
#include <linux/reset.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

#include <soc/tegra/common.h>
#include <soc/tegra/fuse.h>
#include <soc/tegra/pmc.h>

#include <dt-bindings/pinctrl/pinctrl-tegra-io-pad.h>

#define PMC_CNTRL			0x0
#define  PMC_CNTRL_INTR_POLARITY	BIT(17) /* inverts INTR polarity */
#define  PMC_CNTRL_CPU_PWRREQ_OE	BIT(16) /* CPU pwr req enable */
#define  PMC_CNTRL_CPU_PWRREQ_POLARITY	BIT(15) /* CPU pwr req polarity */
#define  PMC_CNTRL_SIDE_EFFECT_LP0	BIT(14) /* LP0 when CPU pwr gated */
#define  PMC_CNTRL_SYSCLK_OE		BIT(11) /* system clock enable */
#define  PMC_CNTRL_SYSCLK_POLARITY	BIT(10) /* sys clk polarity */
#define  PMC_CNTRL_MAIN_RST		BIT(4)

#define DPD_SAMPLE			0x020
#define  DPD_SAMPLE_ENABLE		BIT(0)
#define  DPD_SAMPLE_DISABLE		(0 << 0)

#define PWRGATE_TOGGLE			0x30
#define  PWRGATE_TOGGLE_START		BIT(8)

#define REMOVE_CLAMPING			0x34

#define PWRGATE_STATUS			0x38

#define PMC_PWR_DET			0x48

#define PMC_SCRATCH0_MODE_RECOVERY	BIT(31)
#define PMC_SCRATCH0_MODE_BOOTLOADER	BIT(30)
#define PMC_SCRATCH0_MODE_RCM		BIT(1)
#define PMC_SCRATCH0_MODE_MASK		(PMC_SCRATCH0_MODE_RECOVERY | \
					 PMC_SCRATCH0_MODE_BOOTLOADER | \
					 PMC_SCRATCH0_MODE_RCM)

#define PMC_CPUPWRGOOD_TIMER		0xc8
#define PMC_CPUPWROFF_TIMER		0xcc

#define PMC_PWR_DET_VALUE		0xe4

#define PMC_SCRATCH41			0x140

#define PMC_SENSOR_CTRL			0x1b0
#define  PMC_SENSOR_CTRL_SCRATCH_WRITE	BIT(2)
#define  PMC_SENSOR_CTRL_ENABLE_RST	BIT(1)

#define PMC_RST_STATUS			0x1b4
#define  PMC_RST_STATUS_POR		0
#define  PMC_RST_STATUS_WATCHDOG	1
#define  PMC_RST_STATUS_SENSOR		2
#define  PMC_RST_STATUS_SW_MAIN		3
#define  PMC_RST_STATUS_LP0		4
#define  PMC_RST_STATUS_AOTAG		5

#define IO_DPD_REQ			0x1b8
#define  IO_DPD_REQ_CODE_IDLE		(0U << 30)
#define  IO_DPD_REQ_CODE_OFF		(1U << 30)
#define  IO_DPD_REQ_CODE_ON		(2U << 30)
#define  IO_DPD_REQ_CODE_MASK		(3U << 30)

#define IO_DPD_STATUS			0x1bc
#define IO_DPD2_REQ			0x1c0
#define IO_DPD2_STATUS			0x1c4
#define SEL_DPD_TIM			0x1c8

#define PMC_SCRATCH54			0x258
#define  PMC_SCRATCH54_DATA_SHIFT	8
#define  PMC_SCRATCH54_ADDR_SHIFT	0

#define PMC_SCRATCH55			0x25c
#define  PMC_SCRATCH55_RESET_TEGRA	BIT(31)
#define  PMC_SCRATCH55_CNTRL_ID_SHIFT	27
#define  PMC_SCRATCH55_PINMUX_SHIFT	24
#define  PMC_SCRATCH55_16BITOP		BIT(15)
#define  PMC_SCRATCH55_CHECKSUM_SHIFT	16
#define  PMC_SCRATCH55_I2CSLV1_SHIFT	0

#define GPU_RG_CNTRL			0x2d4

#define TEGRA_PMC_SEL_DPD_TIM		0x1c8
#define TEGRA_PMC_IO_DPD_SAMPLE		0x20
#define TEGRA_PMC_PWR_DET_ENABLE	0x48
#define TEGRA_PMC_PWR_DET_VAL		0xe4
#define TEGRA_PMC_IO_DPD_REQ		0x74
#define TEGRA_PMC_IO_DPD_STATUS		0x78
#define TEGRA_PMC_IO_DPD2_REQ		0x7C
#define TEGRA_PMC_IO_DPD2_STATUS	0x80
#define TEGRA_PMC_E_18V_PWR		0x3C
#define TEGRA_PMC_E_33V_PWR		0x40

/* Tegra186 and later */
#define WAKE_AOWAKE_CTRL 0x4f4
#define  WAKE_AOWAKE_CTRL_INTR_POLARITY BIT(0)

struct tegra_powergate {
	struct generic_pm_domain genpd;
	struct tegra_pmc *pmc;
	unsigned int id;
	struct clk **clks;
	unsigned int num_clks;
	struct reset_control *reset;
};

struct tegra_io_pad_soc {
	const char *name;
	const unsigned int pins[1];
	unsigned int npins;
	unsigned int dpd;
	unsigned int voltage;
	unsigned int io_power;
	unsigned int dpd_req_reg;
	unsigned int dpd_status_reg;
	unsigned int dpd_timer_reg;
	unsigned int dpd_sample_reg;
	unsigned int pwr_det_enable_reg;
	unsigned int pwr_det_val_reg;
	unsigned int pad_uv_0;
	unsigned int pad_uv_1;
	bool bdsdmem_cfc;
};

struct tegra_pmc_regs {
	unsigned int scratch0;
	unsigned int dpd_req;
	unsigned int dpd_status;
	unsigned int dpd2_req;
	unsigned int dpd2_status;
};

struct tegra_pmc_soc {
	unsigned int num_powergates;
	const char *const *powergates;
	unsigned int num_cpu_powergates;
	const u8 *cpu_powergates;

	bool has_tsense_reset;
	bool has_gpu_clamps;
	bool needs_mbist_war;

	const struct tegra_io_pad_soc *io_pads;
	unsigned int num_io_pads;
	const struct pinctrl_pin_desc *descs;
	unsigned int num_descs;

	const struct tegra_pmc_regs *regs;
	void (*init)(struct tegra_pmc *pmc);
	void (*setup_irq_polarity)(struct tegra_pmc *pmc,
				   struct device_node *np,
				   bool invert);
};

/**
 * struct tegra_pmc - NVIDIA Tegra PMC
 * @dev: pointer to PMC device structure
 * @base: pointer to I/O remapped register region
 * @clk: pointer to pclk clock
 * @soc: pointer to SoC data structure
 * @debugfs: pointer to debugfs entry
 * @rate: currently configured rate of pclk
 * @suspend_mode: lowest suspend mode available
 * @cpu_good_time: CPU power good time (in microseconds)
 * @cpu_off_time: CPU power off time (in microsecends)
 * @core_osc_time: core power good OSC time (in microseconds)
 * @core_pmu_time: core power good PMU time (in microseconds)
 * @core_off_time: core power off time (in microseconds)
 * @corereq_high: core power request is active-high
 * @sysclkreq_high: system clock request is active-high
 * @combined_req: combined power request for CPU & core
 * @cpu_pwr_good_en: CPU power good signal is enabled
 * @lp0_vec_phys: physical base address of the LP0 warm boot code
 * @lp0_vec_size: size of the LP0 warm boot code
 * @powergates_available: Bitmap of available power gates
 * @powergates_lock: mutex for power gate register access
 * @pctl: pinctrl handle which is returned after registering pinctrl
 * @pinctrl_desc: Pincontrol descriptor for IO pads
 * @allow_dynamic_switch: restrict the voltage change for each io pad
 * @voltage_switch_restriction_enabled: restrict voltage switch for all io pads
 */
struct tegra_pmc {
	struct device *dev;
	void __iomem *base;
	void __iomem *wake;
	void __iomem *aotag;
	void __iomem *scratch;
	struct clk *clk;
	struct dentry *debugfs;

	const struct tegra_pmc_soc *soc;

	unsigned long rate;

	enum tegra_suspend_mode suspend_mode;
	u32 cpu_good_time;
	u32 cpu_off_time;
	u32 core_osc_time;
	u32 core_pmu_time;
	u32 core_off_time;
	bool corereq_high;
	bool sysclkreq_high;
	bool combined_req;
	bool cpu_pwr_good_en;
	u32 lp0_vec_phys;
	u32 lp0_vec_size;
	DECLARE_BITMAP(powergates_available, TEGRA_POWERGATE_MAX);

	struct mutex powergates_lock;
	struct pinctrl_dev *pctl;
	struct pinctrl_desc pinctrl_desc;
	bool *allow_dynamic_switch;
	bool voltage_switch_restriction_enabled;
};

static const char * const tegra_sor_pad_names[] = {
	[TEGRA_IO_RAIL_HDMI] = "hdmi",
	[TEGRA_IO_RAIL_LVDS] = "lvds",
};

static struct tegra_pmc *pmc = &(struct tegra_pmc) {
	.base = NULL,
	.suspend_mode = TEGRA_SUSPEND_NONE,
};

static inline struct tegra_powergate *
to_powergate(struct generic_pm_domain *domain)
{
	return container_of(domain, struct tegra_powergate, genpd);
}

static u32 tegra_pmc_readl(unsigned long offset)
{
	return readl(pmc->base + offset);
}

static void tegra_pmc_writel(u32 value, unsigned long offset)
{
	writel(value, pmc->base + offset);
}

static inline bool tegra_powergate_state(int id)
{
	if (id == TEGRA_POWERGATE_3D && pmc->soc->has_gpu_clamps)
		return (tegra_pmc_readl(GPU_RG_CNTRL) & 0x1) == 0;
	else
		return (tegra_pmc_readl(PWRGATE_STATUS) & BIT(id)) != 0;
}

static inline bool tegra_powergate_is_valid(int id)
{
	return (pmc->soc && pmc->soc->powergates[id]);
}

static inline bool tegra_powergate_is_available(int id)
{
	return test_bit(id, pmc->powergates_available);
}

static int tegra_powergate_lookup(struct tegra_pmc *pmc, const char *name)
{
	unsigned int i;

	if (!pmc || !pmc->soc || !name)
		return -EINVAL;

	for (i = 0; i < pmc->soc->num_powergates; i++) {
		if (!tegra_powergate_is_valid(i))
			continue;

		if (!strcmp(name, pmc->soc->powergates[i]))
			return i;
	}

	return -ENODEV;
}

/**
 * tegra_powergate_set() - set the state of a partition
 * @id: partition ID
 * @new_state: new state of the partition
 */
static int tegra_powergate_set(unsigned int id, bool new_state)
{
	bool status;
	int err;

	if (id == TEGRA_POWERGATE_3D && pmc->soc->has_gpu_clamps)
		return -EINVAL;

	mutex_lock(&pmc->powergates_lock);

	if (tegra_powergate_state(id) == new_state) {
		mutex_unlock(&pmc->powergates_lock);
		return 0;
	}

	tegra_pmc_writel(PWRGATE_TOGGLE_START | id, PWRGATE_TOGGLE);

	err = readx_poll_timeout(tegra_powergate_state, id, status,
				 status == new_state, 10, 100000);

	mutex_unlock(&pmc->powergates_lock);

	return err;
}

static int __tegra_powergate_remove_clamping(unsigned int id)
{
	u32 mask;

	mutex_lock(&pmc->powergates_lock);

	/*
	 * On Tegra124 and later, the clamps for the GPU are controlled by a
	 * separate register (with different semantics).
	 */
	if (id == TEGRA_POWERGATE_3D) {
		if (pmc->soc->has_gpu_clamps) {
			tegra_pmc_writel(0, GPU_RG_CNTRL);
			goto out;
		}
	}

	/*
	 * Tegra 2 has a bug where PCIE and VDE clamping masks are
	 * swapped relatively to the partition ids
	 */
	if (id == TEGRA_POWERGATE_VDEC)
		mask = (1 << TEGRA_POWERGATE_PCIE);
	else if (id == TEGRA_POWERGATE_PCIE)
		mask = (1 << TEGRA_POWERGATE_VDEC);
	else
		mask = (1 << id);

	tegra_pmc_writel(mask, REMOVE_CLAMPING);

out:
	mutex_unlock(&pmc->powergates_lock);

	return 0;
}

static void tegra_powergate_disable_clocks(struct tegra_powergate *pg)
{
	unsigned int i;

	for (i = 0; i < pg->num_clks; i++)
		clk_disable_unprepare(pg->clks[i]);
}

static int tegra_powergate_enable_clocks(struct tegra_powergate *pg)
{
	unsigned int i;
	int err;

	for (i = 0; i < pg->num_clks; i++) {
		err = clk_prepare_enable(pg->clks[i]);
		if (err)
			goto out;
	}

	return 0;

out:
	while (i--)
		clk_disable_unprepare(pg->clks[i]);

	return err;
}

int __weak tegra210_clk_handle_mbist_war(unsigned int id)
{
	return 0;
}

static int tegra_powergate_power_up(struct tegra_powergate *pg,
				    bool disable_clocks)
{
	int err;

	err = reset_control_assert(pg->reset);
	if (err)
		return err;

	usleep_range(10, 20);

	err = tegra_powergate_set(pg->id, true);
	if (err < 0)
		return err;

	usleep_range(10, 20);

	err = tegra_powergate_enable_clocks(pg);
	if (err)
		goto disable_clks;

	usleep_range(10, 20);

	err = __tegra_powergate_remove_clamping(pg->id);
	if (err)
		goto disable_clks;

	usleep_range(10, 20);

	err = reset_control_deassert(pg->reset);
	if (err)
		goto powergate_off;

	usleep_range(10, 20);

	if (pg->pmc->soc->needs_mbist_war)
		err = tegra210_clk_handle_mbist_war(pg->id);
	if (err)
		goto disable_clks;

	if (disable_clocks)
		tegra_powergate_disable_clocks(pg);

	return 0;

disable_clks:
	tegra_powergate_disable_clocks(pg);
	usleep_range(10, 20);

powergate_off:
	tegra_powergate_set(pg->id, false);

	return err;
}

static int tegra_powergate_power_down(struct tegra_powergate *pg)
{
	int err;

	err = tegra_powergate_enable_clocks(pg);
	if (err)
		return err;

	usleep_range(10, 20);

	err = reset_control_assert(pg->reset);
	if (err)
		goto disable_clks;

	usleep_range(10, 20);

	tegra_powergate_disable_clocks(pg);

	usleep_range(10, 20);

	err = tegra_powergate_set(pg->id, false);
	if (err)
		goto assert_resets;

	return 0;

assert_resets:
	tegra_powergate_enable_clocks(pg);
	usleep_range(10, 20);
	reset_control_deassert(pg->reset);
	usleep_range(10, 20);

disable_clks:
	tegra_powergate_disable_clocks(pg);

	return err;
}

static int tegra_genpd_power_on(struct generic_pm_domain *domain)
{
	struct tegra_powergate *pg = to_powergate(domain);
	int err;

	err = tegra_powergate_power_up(pg, true);
	if (err)
		pr_err("failed to turn on PM domain %s: %d\n", pg->genpd.name,
		       err);

	return err;
}

static int tegra_genpd_power_off(struct generic_pm_domain *domain)
{
	struct tegra_powergate *pg = to_powergate(domain);
	int err;

	err = tegra_powergate_power_down(pg);
	if (err)
		pr_err("failed to turn off PM domain %s: %d\n",
		       pg->genpd.name, err);

	return err;
}

/**
 * tegra_powergate_power_on() - power on partition
 * @id: partition ID
 */
int tegra_powergate_power_on(unsigned int id)
{
	if (!tegra_powergate_is_available(id))
		return -EINVAL;

	return tegra_powergate_set(id, true);
}

/**
 * tegra_powergate_power_off() - power off partition
 * @id: partition ID
 */
int tegra_powergate_power_off(unsigned int id)
{
	if (!tegra_powergate_is_available(id))
		return -EINVAL;

	return tegra_powergate_set(id, false);
}
EXPORT_SYMBOL(tegra_powergate_power_off);

/**
 * tegra_powergate_is_powered() - check if partition is powered
 * @id: partition ID
 */
int tegra_powergate_is_powered(unsigned int id)
{
	int status;

	if (!tegra_powergate_is_valid(id))
		return -EINVAL;

	mutex_lock(&pmc->powergates_lock);
	status = tegra_powergate_state(id);
	mutex_unlock(&pmc->powergates_lock);

	return status;
}

/**
 * tegra_powergate_remove_clamping() - remove power clamps for partition
 * @id: partition ID
 */
int tegra_powergate_remove_clamping(unsigned int id)
{
	if (!tegra_powergate_is_available(id))
		return -EINVAL;

	return __tegra_powergate_remove_clamping(id);
}
EXPORT_SYMBOL(tegra_powergate_remove_clamping);

/**
 * tegra_powergate_sequence_power_up() - power up partition
 * @id: partition ID
 * @clk: clock for partition
 * @rst: reset for partition
 *
 * Must be called with clk disabled, and returns with clk enabled.
 */
int tegra_powergate_sequence_power_up(unsigned int id, struct clk *clk,
				      struct reset_control *rst)
{
	struct tegra_powergate *pg;
	int err;

	if (!tegra_powergate_is_available(id))
		return -EINVAL;

	pg = kzalloc(sizeof(*pg), GFP_KERNEL);
	if (!pg)
		return -ENOMEM;

	pg->id = id;
	pg->clks = &clk;
	pg->num_clks = 1;
	pg->reset = rst;
	pg->pmc = pmc;

	err = tegra_powergate_power_up(pg, false);
	if (err)
		pr_err("failed to turn on partition %d: %d\n", id, err);

	kfree(pg);

	return err;
}
EXPORT_SYMBOL(tegra_powergate_sequence_power_up);

#ifdef CONFIG_SMP
/**
 * tegra_get_cpu_powergate_id() - convert from CPU ID to partition ID
 * @cpuid: CPU partition ID
 *
 * Returns the partition ID corresponding to the CPU partition ID or a
 * negative error code on failure.
 */
static int tegra_get_cpu_powergate_id(unsigned int cpuid)
{
	if (pmc->soc && cpuid < pmc->soc->num_cpu_powergates)
		return pmc->soc->cpu_powergates[cpuid];

	return -EINVAL;
}

/**
 * tegra_pmc_cpu_is_powered() - check if CPU partition is powered
 * @cpuid: CPU partition ID
 */
bool tegra_pmc_cpu_is_powered(unsigned int cpuid)
{
	int id;

	id = tegra_get_cpu_powergate_id(cpuid);
	if (id < 0)
		return false;

	return tegra_powergate_is_powered(id);
}

/**
 * tegra_pmc_cpu_power_on() - power on CPU partition
 * @cpuid: CPU partition ID
 */
int tegra_pmc_cpu_power_on(unsigned int cpuid)
{
	int id;

	id = tegra_get_cpu_powergate_id(cpuid);
	if (id < 0)
		return id;

	return tegra_powergate_set(id, true);
}

/**
 * tegra_pmc_cpu_remove_clamping() - remove power clamps for CPU partition
 * @cpuid: CPU partition ID
 */
int tegra_pmc_cpu_remove_clamping(unsigned int cpuid)
{
	int id;

	id = tegra_get_cpu_powergate_id(cpuid);
	if (id < 0)
		return id;

	return tegra_powergate_remove_clamping(id);
}
#endif /* CONFIG_SMP */

static int tegra_pmc_restart_notify(struct notifier_block *this,
				    unsigned long action, void *data)
{
	const char *cmd = data;
	u32 value;

	value = readl(pmc->scratch + pmc->soc->regs->scratch0);
	value &= ~PMC_SCRATCH0_MODE_MASK;

	if (cmd) {
		if (strcmp(cmd, "recovery") == 0)
			value |= PMC_SCRATCH0_MODE_RECOVERY;

		if (strcmp(cmd, "bootloader") == 0)
			value |= PMC_SCRATCH0_MODE_BOOTLOADER;

		if (strcmp(cmd, "forced-recovery") == 0)
			value |= PMC_SCRATCH0_MODE_RCM;
	}

	writel(value, pmc->scratch + pmc->soc->regs->scratch0);

	/* reset everything but PMC_SCRATCH0 and PMC_RST_STATUS */
	value = tegra_pmc_readl(PMC_CNTRL);
	value |= PMC_CNTRL_MAIN_RST;
	tegra_pmc_writel(value, PMC_CNTRL);

	return NOTIFY_DONE;
}

static struct notifier_block tegra_pmc_restart_handler = {
	.notifier_call = tegra_pmc_restart_notify,
	.priority = 128,
};

static int powergate_show(struct seq_file *s, void *data)
{
	unsigned int i;
	int status;

	seq_printf(s, " powergate powered\n");
	seq_printf(s, "------------------\n");

	for (i = 0; i < pmc->soc->num_powergates; i++) {
		status = tegra_powergate_is_powered(i);
		if (status < 0)
			continue;

		seq_printf(s, " %9s %7s\n", pmc->soc->powergates[i],
			   status ? "yes" : "no");
	}

	return 0;
}

static int powergate_open(struct inode *inode, struct file *file)
{
	return single_open(file, powergate_show, inode->i_private);
}

static const struct file_operations powergate_fops = {
	.open = powergate_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static int tegra_powergate_debugfs_init(void)
{
	pmc->debugfs = debugfs_create_file("powergate", S_IRUGO, NULL, NULL,
					   &powergate_fops);
	if (!pmc->debugfs)
		return -ENOMEM;

	return 0;
}

static int tegra_powergate_of_get_clks(struct tegra_powergate *pg,
				       struct device_node *np)
{
	struct clk *clk;
	unsigned int i, count;
	int err;

	count = of_clk_get_parent_count(np);
	if (count == 0)
		return -ENODEV;

	pg->clks = kcalloc(count, sizeof(clk), GFP_KERNEL);
	if (!pg->clks)
		return -ENOMEM;

	for (i = 0; i < count; i++) {
		pg->clks[i] = of_clk_get(np, i);
		if (IS_ERR(pg->clks[i])) {
			err = PTR_ERR(pg->clks[i]);
			goto err;
		}
	}

	pg->num_clks = count;

	return 0;

err:
	while (i--)
		clk_put(pg->clks[i]);

	kfree(pg->clks);

	return err;
}

static int tegra_powergate_of_get_resets(struct tegra_powergate *pg,
					 struct device_node *np, bool off)
{
	int err;

	pg->reset = of_reset_control_array_get_exclusive(np);
	if (IS_ERR(pg->reset)) {
		err = PTR_ERR(pg->reset);
		pr_err("failed to get device resets: %d\n", err);
		return err;
	}

	if (off)
		err = reset_control_assert(pg->reset);
	else
		err = reset_control_deassert(pg->reset);

	if (err)
		reset_control_put(pg->reset);

	return err;
}

static void tegra_powergate_add(struct tegra_pmc *pmc, struct device_node *np)
{
	struct tegra_powergate *pg;
	int id, err;
	bool off;

	pg = kzalloc(sizeof(*pg), GFP_KERNEL);
	if (!pg)
		return;

	id = tegra_powergate_lookup(pmc, np->name);
	if (id < 0) {
		pr_err("powergate lookup failed for %s: %d\n", np->name, id);
		goto free_mem;
	}

	/*
	 * Clear the bit for this powergate so it cannot be managed
	 * directly via the legacy APIs for controlling powergates.
	 */
	clear_bit(id, pmc->powergates_available);

	pg->id = id;
	pg->genpd.name = np->name;
	pg->genpd.power_off = tegra_genpd_power_off;
	pg->genpd.power_on = tegra_genpd_power_on;
	pg->pmc = pmc;

	off = !tegra_powergate_is_powered(pg->id);

	err = tegra_powergate_of_get_clks(pg, np);
	if (err < 0) {
		pr_err("failed to get clocks for %s: %d\n", np->name, err);
		goto set_available;
	}

	err = tegra_powergate_of_get_resets(pg, np, off);
	if (err < 0) {
		pr_err("failed to get resets for %s: %d\n", np->name, err);
		goto remove_clks;
	}

	if (!IS_ENABLED(CONFIG_PM_GENERIC_DOMAINS)) {
		if (off)
			WARN_ON(tegra_powergate_power_up(pg, true));

		goto remove_resets;
	}

	/*
	 * FIXME: If XHCI is enabled for Tegra, then power-up the XUSB
	 * host and super-speed partitions. Once the XHCI driver
	 * manages the partitions itself this code can be removed. Note
	 * that we don't register these partitions with the genpd core
	 * to avoid it from powering down the partitions as they appear
	 * to be unused.
	 */
	if (IS_ENABLED(CONFIG_USB_XHCI_TEGRA) &&
	    (id == TEGRA_POWERGATE_XUSBA || id == TEGRA_POWERGATE_XUSBC)) {
		if (off)
			WARN_ON(tegra_powergate_power_up(pg, true));

		goto remove_resets;
	}

	err = pm_genpd_init(&pg->genpd, NULL, off);
	if (err < 0) {
		pr_err("failed to initialise PM domain %s: %d\n", np->name,
		       err);
		goto remove_resets;
	}

	err = of_genpd_add_provider_simple(np, &pg->genpd);
	if (err < 0) {
		pr_err("failed to add PM domain provider for %s: %d\n",
		       np->name, err);
		goto remove_genpd;
	}

	pr_debug("added PM domain %s\n", pg->genpd.name);

	return;

remove_genpd:
	pm_genpd_remove(&pg->genpd);

remove_resets:
	reset_control_put(pg->reset);

remove_clks:
	while (pg->num_clks--)
		clk_put(pg->clks[pg->num_clks]);

	kfree(pg->clks);

set_available:
	set_bit(id, pmc->powergates_available);

free_mem:
	kfree(pg);
}

static void tegra_powergate_init(struct tegra_pmc *pmc,
				 struct device_node *parent)
{
	struct device_node *np, *child;
	unsigned int i;

	/* Create a bitmap of the available and valid partitions */
	for (i = 0; i < pmc->soc->num_powergates; i++)
		if (pmc->soc->powergates[i])
			set_bit(i, pmc->powergates_available);

	np = of_get_child_by_name(parent, "powergates");
	if (!np)
		return;

	for_each_child_of_node(np, child)
		tegra_powergate_add(pmc, child);

	of_node_put(np);
}

static int tegra_io_pad_prepare(const struct tegra_io_pad_soc *pad)
{
	unsigned long rate, value;

	if (pad->dpd == UINT_MAX)
		return -ENOTSUPP;

	if (!pmc->clk)
		return 0;

	rate = clk_get_rate(pmc->clk);
	if (!rate) {
		dev_err(pmc->dev, "Failed to get clock rate\n");
		return -ENODEV;
	}

	tegra_pmc_writel(DPD_SAMPLE_ENABLE, pad->dpd_sample_reg);

	/* must be at least 200 ns, in APB (PCLK) clock cycles */
	value = DIV_ROUND_UP(1000000000, rate);
	value = DIV_ROUND_UP(200, value);
	tegra_pmc_writel(value, pad->dpd_timer_reg);

	return 0;
}

static int tegra_io_pad_poll(const struct tegra_io_pad_soc *pad,
			     u32 val, unsigned long timeout)
{
	u32 value;
	u32 mask = BIT(pad->dpd);

	timeout = jiffies + msecs_to_jiffies(timeout);

	while (time_after(timeout, jiffies)) {
		value = tegra_pmc_readl(pad->dpd_status_reg);
		if ((value & mask) == val)
			return 0;

		usleep_range(250, 1000);
	}

	return -ETIMEDOUT;
}

static void tegra_io_pad_unprepare(const struct tegra_io_pad_soc *pad)
{
	if (pmc->clk)
		tegra_pmc_writel(DPD_SAMPLE_DISABLE,  pad->dpd_sample_reg);
}

static const struct tegra_io_pad_soc *tegra_get_pad_by_name(const char *pname)
{
	unsigned int i;

	for (i = 0; i < pmc->soc->num_io_pads; ++i) {
		if (!strcmp(pname, pmc->soc->io_pads[i].name))
			return &pmc->soc->io_pads[i];
	}

	return NULL;
}

/**
 * tegra_io_pad_power_enable() - enable power to I/O pad
 * @id: Tegra I/O pad ID for which to enable power
 *
 * Returns: 0 on success or a negative error code on failure.
 */
static int tegra_io_pad_power_enable(const struct tegra_io_pad_soc *pad)
{
	int err;

	mutex_lock(&pmc->powergates_lock);

	err = tegra_io_pad_prepare(pad);
	if (err < 0) {
		pr_err("failed to prepare I/O pad %s: %d\n", pad->name, err);
		goto unlock;
	}

	tegra_pmc_writel(IO_DPD_REQ_CODE_OFF | BIT(pad->dpd),
			 pad->dpd_req_reg);

	err = tegra_io_pad_poll(pad, 0, 250);
	if (err < 0) {
		pr_err("failed to enable I/O pad %s: %d\n", pad->name, err);
		goto unlock;
	}

	tegra_io_pad_unprepare(pad);

unlock:
	mutex_unlock(&pmc->powergates_lock);
	return err;
}

/**
 * tegra_io_pad_power_disable() - disable power to I/O pad
 * @id: Tegra I/O pad ID for which to disable power
 *
 * Returns: 0 on success or a negative error code on failure.
 */
static int tegra_io_pad_power_disable(const struct tegra_io_pad_soc *pad)
{
	int err;

	mutex_lock(&pmc->powergates_lock);

	err = tegra_io_pad_prepare(pad);
	if (err < 0) {
		pr_err("failed to prepare I/O pad %s: %d\n", pad->name, err);
		goto unlock;
	}

	tegra_pmc_writel(IO_DPD_REQ_CODE_ON | BIT(pad->dpd),
			 pad->dpd_req_reg);

	err = tegra_io_pad_poll(pad, BIT(pad->dpd), 250);
	if (err < 0) {
		pr_err("failed to disable I/O pad %s: %d\n", pad->name, err);
		goto unlock;
	}

	tegra_io_pad_unprepare(pad);

unlock:
	mutex_unlock(&pmc->powergates_lock);
	return err;
}

static int tegra_io_pad_set_voltage(const struct tegra_io_pad_soc *pad,
				    int io_pad_uv)
{
	u32 value;

	if (pad->voltage == UINT_MAX)
		return -ENOTSUPP;

	if (io_pad_uv != pad->pad_uv_0 && io_pad_uv != pad->pad_uv_1)
		return -EINVAL;

	mutex_lock(&pmc->powergates_lock);

	/* write-enable PMC_PWR_DET_VALUE[pad->voltage] */
	if (pad->pwr_det_enable_reg != UINT_MAX) {
		value = tegra_pmc_readl(pad->pwr_det_enable_reg);
		value |= BIT(pad->voltage);
		tegra_pmc_writel(value, pad->pwr_det_enable_reg);
	}

	/* update I/O voltage */
	value = tegra_pmc_readl(pad->pwr_det_val_reg);

	if (io_pad_uv == pad->pad_uv_0)
		value &= ~BIT(pad->voltage);
	else
		value |= BIT(pad->voltage);

	tegra_pmc_writel(value, pad->pwr_det_val_reg);

	mutex_unlock(&pmc->powergates_lock);

	usleep_range(100, 250);

	return 0;
}

static int tegra_io_pad_get_voltage(const struct tegra_io_pad_soc *pad)
{
	u32 value;

	if (pad->voltage == UINT_MAX)
		return -ENOTSUPP;

	value = tegra_pmc_readl(pad->pwr_det_val_reg);

	if ((value & BIT(pad->voltage)) == 0)
		return pad->pad_uv_0;

	return pad->pad_uv_1;
}

/**
 * tegra_io_pad_is_powered() - check if IO pad is powered
 * @pad: Tegra I/O pad SOC data for which power status need to check
 *
 * Return 1 if power-ON, 0 if power OFF and error number in
 * negative if pad ID is not valid or power down not supported
 * on given IO pad.
 */
static int tegra_io_pad_is_powered(const struct tegra_io_pad_soc *pad)
{
	u32 value;

	if (pad->dpd == UINT_MAX)
		return -ENOTSUPP;

	value = tegra_pmc_readl(pad->dpd_status_reg);

	return !(value & BIT(pad->dpd));
}

static int tegra_io_pads_pinctrl_get_groups_count(struct pinctrl_dev *pctldev)
{
	struct tegra_pmc *tpmc = pinctrl_dev_get_drvdata(pctldev);

	return tpmc->soc->num_io_pads;
}

static const char *tegra_io_pads_pinctrl_get_group_name(
			struct pinctrl_dev *pctldev, unsigned int group)
{
	struct tegra_pmc *tpmc = pinctrl_dev_get_drvdata(pctldev);

	return tpmc->soc->io_pads[group].name;
}

static int tegra_io_pads_pinctrl_get_group_pins(struct pinctrl_dev *pctldev,
						unsigned int group,
						const unsigned int **pins,
						unsigned int *num_pins)
{
	struct tegra_pmc *tpmc = pinctrl_dev_get_drvdata(pctldev);

	*pins = tpmc->soc->io_pads[group].pins;
	*num_pins = tpmc->soc->io_pads[group].npins;

	return 0;
}

enum tegra_io_rail_pads_params {
	TEGRA_IO_PAD_POWER_SOURCE_VOLTAGE = PIN_CONFIG_END + 1,
	TEGRA_IO_PAD_DYNAMIC_VOLTAGE_SWITCH = PIN_CONFIG_END + 2,
};

static const struct pinconf_generic_params tegra_io_pads_cfg_params[] = {
	{
		.property = "nvidia,power-source-voltage",
		.param = TEGRA_IO_PAD_POWER_SOURCE_VOLTAGE,
	}, {
		.property = "nvidia,enable-voltage-switching",
		.param = TEGRA_IO_PAD_DYNAMIC_VOLTAGE_SWITCH,
	},
};

static const struct pinctrl_ops tegra_io_pads_pinctrl_ops = {
	.get_groups_count = tegra_io_pads_pinctrl_get_groups_count,
	.get_group_name	= tegra_io_pads_pinctrl_get_group_name,
	.get_group_pins	= tegra_io_pads_pinctrl_get_group_pins,
	.dt_node_to_map	= pinconf_generic_dt_node_to_map_pin,
	.dt_free_map	= pinconf_generic_dt_free_map,
};

static int tegra_io_pads_pinconf_get(struct pinctrl_dev *pctldev,
				     unsigned int pin,
				     unsigned long *config)
{
	struct tegra_pmc *tpmc = pinctrl_dev_get_drvdata(pctldev);
	u16 param = pinconf_to_config_param(*config);
	const struct tegra_io_pad_soc *pad = &tpmc->soc->io_pads[pin];
	u16 arg = 0;
	int ret;

	switch (param) {
	case PIN_CONFIG_LOW_POWER_MODE:
		ret = tegra_io_pad_is_powered(pad);
		if (ret < 0)
			return ret;
		arg = !ret;
		break;
	case TEGRA_IO_PAD_POWER_SOURCE_VOLTAGE:
		if (pmc->soc->io_pads[pin].voltage == UINT_MAX)
			return -EINVAL;

		ret = tegra_io_pad_get_voltage(pad);
		if (ret < 0)
			return ret;
		arg = ret;
		break;
	case TEGRA_IO_PAD_DYNAMIC_VOLTAGE_SWITCH:
		if (pmc->soc->io_pads[pin].voltage == UINT_MAX)
			return -EINVAL;

		if (pmc->voltage_switch_restriction_enabled &&
		    pmc->allow_dynamic_switch[pin])
			arg = 1;
		else
			arg = 0;
		break;
	default:
		dev_dbg(tpmc->dev, "I/O pad %s does not support param %d\n",
			pad->name, param);
		return -EINVAL;
	}

	*config = pinconf_to_config_packed(param, arg);

	return 0;
}

static int tegra_io_pads_pinconf_set(struct pinctrl_dev *pctldev,
				     unsigned int pin, unsigned long *configs,
				     unsigned int num_configs)
{
	struct tegra_pmc *tpmc = pinctrl_dev_get_drvdata(pctldev);
	const struct tegra_io_pad_soc *pad = &tpmc->soc->io_pads[pin];
	unsigned int i;
	int ret;

	for (i = 0; i < num_configs; i++) {
		u16 param_val = pinconf_to_config_argument(configs[i]);
		u16 param = pinconf_to_config_param(configs[i]);

		switch (param) {
		case PIN_CONFIG_LOW_POWER_MODE:
			if (param_val)
				ret = tegra_io_pad_power_disable(pad);
			else
				ret = tegra_io_pad_power_enable(pad);
			if (ret < 0) {
				dev_err(tpmc->dev,
					"Failed to set low power %s of I/O pad %s: %d\n",
					(param_val) ? "disable" : "enable",
					pad->name, ret);
				return ret;
			}
			break;
		case TEGRA_IO_PAD_POWER_SOURCE_VOLTAGE:
			if (pmc->soc->io_pads[pin].voltage == UINT_MAX)
				return -EINVAL;

			if (pmc->voltage_switch_restriction_enabled &&
			    !pmc->allow_dynamic_switch[pin]) {
				dev_err(tpmc->dev,
					"IO Pad %s: Dynamic voltage switching not allowed\n",
					pad->name);
				return -EINVAL;
			}

			ret = tegra_io_pad_set_voltage(pad, param_val);
			if (ret < 0) {
				dev_err(tpmc->dev,
					"Failed to set voltage %d of pin %u: %d\n",
					param_val, pin, ret);
				return ret;
			}
			break;
		case TEGRA_IO_PAD_DYNAMIC_VOLTAGE_SWITCH:
			if (pmc->soc->io_pads[pin].voltage == UINT_MAX)
				return -EINVAL;

			pmc->allow_dynamic_switch[pin] = true;
			break;
		default:
			dev_err(tpmc->dev, "I/O pad %s does not support param %d\n",
				pad->name, param);
			return -EINVAL;
		}
	}

	return 0;
}

#ifdef CONFIG_DEBUG_FS
static void tegra_io_pads_pinconf_dbg_show(struct pinctrl_dev *pctldev,
					   struct seq_file *s, unsigned int pin)
{
	struct tegra_pmc *tpmc = pinctrl_dev_get_drvdata(pctldev);
	unsigned long config = 0;
	u16 param, param_val;
	int ret;
	int i;

	for (i = 0; i < tpmc->pinctrl_desc.num_custom_params; ++i) {
		param = tpmc->pinctrl_desc.custom_params[i].param;
		config = pinconf_to_config_packed(param, 0);
		ret = tegra_io_pads_pinconf_get(pctldev, pin, &config);
		if (ret < 0)
			continue;
		param_val = pinconf_to_config_argument(config);
		switch (param) {
		case TEGRA_IO_PAD_POWER_SOURCE_VOLTAGE:
			if (param_val == TEGRA_IO_PAD_VOLTAGE_1200000UV)
				seq_puts(s, "\n\t\tPad voltage 1200000uV");
			else if (param_val == TEGRA_IO_PAD_VOLTAGE_1800000UV)
				seq_puts(s, "\n\t\tPad voltage 1800000uV");
			else
				seq_puts(s, "\n\t\tPad voltage 3300000uV");
			break;
		case TEGRA_IO_PAD_DYNAMIC_VOLTAGE_SWITCH:
			seq_printf(s, "\n\t\tSwitching voltage: %s",
				   (param_val) ? "Enable" : "Disable");
			break;
		default:
			break;
		}
	}
}
#else
static void tegra_io_pads_pinconf_dbg_show(struct pinctrl_dev *pctldev,
					   struct seq_file *s, unsigned int pin)
{
}
#endif

static const struct pinconf_ops tegra_io_pads_pinconf_ops = {
	.pin_config_get = tegra_io_pads_pinconf_get,
	.pin_config_set = tegra_io_pads_pinconf_set,
	.pin_config_dbg_show = tegra_io_pads_pinconf_dbg_show,
	.is_generic = true,
};

static int tegra_io_pads_pinctrl_init(struct tegra_pmc *pmc)
{
	if (!pmc->soc->num_descs)
		return 0;

	pmc->allow_dynamic_switch = devm_kzalloc(pmc->dev, pmc->soc->num_descs *
					 sizeof(*pmc->allow_dynamic_switch),
					 GFP_KERNEL);
	if (!pmc->allow_dynamic_switch) {
		pr_err("failed to allocate allow_dynamic_switch\n");
		return 0;
	}

	pmc->voltage_switch_restriction_enabled = false;
	pmc->pinctrl_desc.name = "pinctrl-pmc-io-pads";
	pmc->pinctrl_desc.pctlops = &tegra_io_pads_pinctrl_ops;
	pmc->pinctrl_desc.confops = &tegra_io_pads_pinconf_ops;
	pmc->pinctrl_desc.pins = pmc->soc->descs;
	pmc->pinctrl_desc.npins = pmc->soc->num_descs;
	pmc->pinctrl_desc.custom_params = tegra_io_pads_cfg_params;
	pmc->pinctrl_desc.num_custom_params =
				ARRAY_SIZE(tegra_io_pads_cfg_params);

	pmc->pctl = devm_pinctrl_register(pmc->dev, &pmc->pinctrl_desc, pmc);
	if (IS_ERR(pmc->pctl)) {
		int ret = PTR_ERR(pmc->pctl);

		pr_err("failed to register pinctrl-io-pad: %d\n", ret);
		return ret;
	}

	pmc->voltage_switch_restriction_enabled =
		of_property_read_bool(pmc->dev->of_node,
				      "nvidia,restrict-voltage-switch");

	return 0;
}

/**
 * tegra_io_rail_power_on() - enable power to I/O rail
 * @id: Tegra I/O pad ID for which to enable power
 *
 * See also: tegra_io_pad_power_enable()
 */
int tegra_io_rail_power_on(unsigned int id)
{
	const struct tegra_io_pad_soc *pad;

	if (id != TEGRA_IO_RAIL_LVDS && id != TEGRA_IO_RAIL_HDMI) {
		dev_err(pmc->dev, "invalid pad id\n");
		return -EINVAL;
	}

	pad = tegra_get_pad_by_name(tegra_sor_pad_names[id]);
	if (!pad) {
		dev_err(pmc->dev, "IO Pad not found\n");
		return -EINVAL;
	}

	return tegra_io_pad_power_enable(pad);
}
EXPORT_SYMBOL(tegra_io_rail_power_on);

/**
 * tegra_io_rail_power_off() - disable power to I/O rail
 * @id: Tegra I/O pad ID for which to disable power
 *
 * See also: tegra_io_pad_power_disable()
 */
int tegra_io_rail_power_off(unsigned int id)
{
	const struct tegra_io_pad_soc *pad;

	if (id != TEGRA_IO_RAIL_LVDS && id != TEGRA_IO_RAIL_HDMI) {
		dev_err(pmc->dev, "invalid pad id\n");
		return -EINVAL;
	}

	pad = tegra_get_pad_by_name(tegra_sor_pad_names[id]);
	if (!pad) {
		dev_err(pmc->dev, "IO Pad not found\n");
		return -EINVAL;
	}

	return tegra_io_pad_power_disable(pad);
}
EXPORT_SYMBOL(tegra_io_rail_power_off);

#ifdef CONFIG_PM_SLEEP
enum tegra_suspend_mode tegra_pmc_get_suspend_mode(void)
{
	return pmc->suspend_mode;
}

void tegra_pmc_set_suspend_mode(enum tegra_suspend_mode mode)
{
	if (mode < TEGRA_SUSPEND_NONE || mode >= TEGRA_MAX_SUSPEND_MODE)
		return;

	pmc->suspend_mode = mode;
}

void tegra_pmc_enter_suspend_mode(enum tegra_suspend_mode mode)
{
	unsigned long long rate = 0;
	u32 value;

	switch (mode) {
	case TEGRA_SUSPEND_LP1:
		rate = 32768;
		break;

	case TEGRA_SUSPEND_LP2:
		rate = clk_get_rate(pmc->clk);
		break;

	default:
		break;
	}

	if (WARN_ON_ONCE(rate == 0))
		rate = 100000000;

	if (rate != pmc->rate) {
		u64 ticks;

		ticks = pmc->cpu_good_time * rate + USEC_PER_SEC - 1;
		do_div(ticks, USEC_PER_SEC);
		tegra_pmc_writel(ticks, PMC_CPUPWRGOOD_TIMER);

		ticks = pmc->cpu_off_time * rate + USEC_PER_SEC - 1;
		do_div(ticks, USEC_PER_SEC);
		tegra_pmc_writel(ticks, PMC_CPUPWROFF_TIMER);

		wmb();

		pmc->rate = rate;
	}

	value = tegra_pmc_readl(PMC_CNTRL);
	value &= ~PMC_CNTRL_SIDE_EFFECT_LP0;
	value |= PMC_CNTRL_CPU_PWRREQ_OE;
	tegra_pmc_writel(value, PMC_CNTRL);
}
#endif

static int tegra_pmc_parse_dt(struct tegra_pmc *pmc, struct device_node *np)
{
	u32 value, values[2];

	if (of_property_read_u32(np, "nvidia,suspend-mode", &value)) {
	} else {
		switch (value) {
		case 0:
			pmc->suspend_mode = TEGRA_SUSPEND_LP0;
			break;

		case 1:
			pmc->suspend_mode = TEGRA_SUSPEND_LP1;
			break;

		case 2:
			pmc->suspend_mode = TEGRA_SUSPEND_LP2;
			break;

		default:
			pmc->suspend_mode = TEGRA_SUSPEND_NONE;
			break;
		}
	}

	pmc->suspend_mode = tegra_pm_validate_suspend_mode(pmc->suspend_mode);

	if (of_property_read_u32(np, "nvidia,cpu-pwr-good-time", &value))
		pmc->suspend_mode = TEGRA_SUSPEND_NONE;

	pmc->cpu_good_time = value;

	if (of_property_read_u32(np, "nvidia,cpu-pwr-off-time", &value))
		pmc->suspend_mode = TEGRA_SUSPEND_NONE;

	pmc->cpu_off_time = value;

	if (of_property_read_u32_array(np, "nvidia,core-pwr-good-time",
				       values, ARRAY_SIZE(values)))
		pmc->suspend_mode = TEGRA_SUSPEND_NONE;

	pmc->core_osc_time = values[0];
	pmc->core_pmu_time = values[1];

	if (of_property_read_u32(np, "nvidia,core-pwr-off-time", &value))
		pmc->suspend_mode = TEGRA_SUSPEND_NONE;

	pmc->core_off_time = value;

	pmc->corereq_high = of_property_read_bool(np,
				"nvidia,core-power-req-active-high");

	pmc->sysclkreq_high = of_property_read_bool(np,
				"nvidia,sys-clock-req-active-high");

	pmc->combined_req = of_property_read_bool(np,
				"nvidia,combined-power-req");

	pmc->cpu_pwr_good_en = of_property_read_bool(np,
				"nvidia,cpu-pwr-good-en");

	if (of_property_read_u32_array(np, "nvidia,lp0-vec", values,
				       ARRAY_SIZE(values)))
		if (pmc->suspend_mode == TEGRA_SUSPEND_LP0)
			pmc->suspend_mode = TEGRA_SUSPEND_LP1;

	pmc->lp0_vec_phys = values[0];
	pmc->lp0_vec_size = values[1];

	return 0;
}

static void tegra_pmc_init(struct tegra_pmc *pmc)
{
	if (pmc->soc->init)
		pmc->soc->init(pmc);
}

static void tegra_pmc_init_tsense_reset(struct tegra_pmc *pmc)
{
	static const char disabled[] = "emergency thermal reset disabled";
	u32 pmu_addr, ctrl_id, reg_addr, reg_data, pinmux;
	struct device *dev = pmc->dev;
	struct device_node *np;
	u32 value, checksum;

	if (!pmc->soc->has_tsense_reset)
		return;

	np = of_find_node_by_name(pmc->dev->of_node, "i2c-thermtrip");
	if (!np) {
		dev_warn(dev, "i2c-thermtrip node not found, %s.\n", disabled);
		return;
	}

	if (of_property_read_u32(np, "nvidia,i2c-controller-id", &ctrl_id)) {
		dev_err(dev, "I2C controller ID missing, %s.\n", disabled);
		goto out;
	}

	if (of_property_read_u32(np, "nvidia,bus-addr", &pmu_addr)) {
		dev_err(dev, "nvidia,bus-addr missing, %s.\n", disabled);
		goto out;
	}

	if (of_property_read_u32(np, "nvidia,reg-addr", &reg_addr)) {
		dev_err(dev, "nvidia,reg-addr missing, %s.\n", disabled);
		goto out;
	}

	if (of_property_read_u32(np, "nvidia,reg-data", &reg_data)) {
		dev_err(dev, "nvidia,reg-data missing, %s.\n", disabled);
		goto out;
	}

	if (of_property_read_u32(np, "nvidia,pinmux-id", &pinmux))
		pinmux = 0;

	value = tegra_pmc_readl(PMC_SENSOR_CTRL);
	value |= PMC_SENSOR_CTRL_SCRATCH_WRITE;
	tegra_pmc_writel(value, PMC_SENSOR_CTRL);

	value = (reg_data << PMC_SCRATCH54_DATA_SHIFT) |
		(reg_addr << PMC_SCRATCH54_ADDR_SHIFT);
	tegra_pmc_writel(value, PMC_SCRATCH54);

	value = PMC_SCRATCH55_RESET_TEGRA;
	value |= ctrl_id << PMC_SCRATCH55_CNTRL_ID_SHIFT;
	value |= pinmux << PMC_SCRATCH55_PINMUX_SHIFT;
	value |= pmu_addr << PMC_SCRATCH55_I2CSLV1_SHIFT;

	/*
	 * Calculate checksum of SCRATCH54, SCRATCH55 fields. Bits 23:16 will
	 * contain the checksum and are currently zero, so they are not added.
	 */
	checksum = reg_addr + reg_data + (value & 0xff) + ((value >> 8) & 0xff)
		+ ((value >> 24) & 0xff);
	checksum &= 0xff;
	checksum = 0x100 - checksum;

	value |= checksum << PMC_SCRATCH55_CHECKSUM_SHIFT;

	tegra_pmc_writel(value, PMC_SCRATCH55);

	value = tegra_pmc_readl(PMC_SENSOR_CTRL);
	value |= PMC_SENSOR_CTRL_ENABLE_RST;
	tegra_pmc_writel(value, PMC_SENSOR_CTRL);

	dev_info(pmc->dev, "emergency thermal reset enabled\n");

out:
	of_node_put(np);
}

static int tegra_pmc_probe(struct platform_device *pdev)
{
	void __iomem *base;
	struct resource *res;
	int err;

	/*
	 * Early initialisation should have configured an initial
	 * register mapping and setup the soc data pointer. If these
	 * are not valid then something went badly wrong!
	 */
	if (WARN_ON(!pmc->base || !pmc->soc))
		return -ENODEV;

	err = tegra_pmc_parse_dt(pmc, pdev->dev.of_node);
	if (err < 0)
		return err;

	/* take over the memory region from the early initialization */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(base))
		return PTR_ERR(base);

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "wake");
	if (res) {
		pmc->wake = devm_ioremap_resource(&pdev->dev, res);
		if (IS_ERR(pmc->wake))
			return PTR_ERR(pmc->wake);
	} else {
		pmc->wake = base;
	}

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "aotag");
	if (res) {
		pmc->aotag = devm_ioremap_resource(&pdev->dev, res);
		if (IS_ERR(pmc->aotag))
			return PTR_ERR(pmc->aotag);
	} else {
		pmc->aotag = base;
	}

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "scratch");
	if (res) {
		pmc->scratch = devm_ioremap_resource(&pdev->dev, res);
		if (IS_ERR(pmc->scratch))
			return PTR_ERR(pmc->scratch);
	} else {
		pmc->scratch = base;
	}

	pmc->clk = devm_clk_get(&pdev->dev, "pclk");
	if (IS_ERR(pmc->clk)) {
		err = PTR_ERR(pmc->clk);

		if (err != -ENOENT) {
			dev_err(&pdev->dev, "failed to get pclk: %d\n", err);
			return err;
		}

		pmc->clk = NULL;
	}

	pmc->dev = &pdev->dev;

	tegra_pmc_init(pmc);

	tegra_pmc_init_tsense_reset(pmc);

	if (IS_ENABLED(CONFIG_DEBUG_FS)) {
		err = tegra_powergate_debugfs_init();
		if (err < 0)
			return err;
	}

	err = register_restart_handler(&tegra_pmc_restart_handler);
	if (err) {
		debugfs_remove(pmc->debugfs);
		dev_err(&pdev->dev, "unable to register restart handler, %d\n",
			err);
		return err;
	}

	err = tegra_io_pads_pinctrl_init(pmc);
	if (err < 0)
		return err;

	mutex_lock(&pmc->powergates_lock);
	iounmap(pmc->base);
	pmc->base = base;
	mutex_unlock(&pmc->powergates_lock);

	return 0;
}

#if defined(CONFIG_PM_SLEEP) && defined(CONFIG_ARM)
static int tegra_pmc_suspend(struct device *dev)
{
	tegra_pmc_writel(virt_to_phys(tegra_resume), PMC_SCRATCH41);

	return 0;
}

static int tegra_pmc_resume(struct device *dev)
{
	tegra_pmc_writel(0x0, PMC_SCRATCH41);

	return 0;
}

static SIMPLE_DEV_PM_OPS(tegra_pmc_pm_ops, tegra_pmc_suspend, tegra_pmc_resume);

#endif

static const char * const tegra20_powergates[] = {
	[TEGRA_POWERGATE_CPU] = "cpu",
	[TEGRA_POWERGATE_3D] = "3d",
	[TEGRA_POWERGATE_VENC] = "venc",
	[TEGRA_POWERGATE_VDEC] = "vdec",
	[TEGRA_POWERGATE_PCIE] = "pcie",
	[TEGRA_POWERGATE_L2] = "l2",
	[TEGRA_POWERGATE_MPE] = "mpe",
};

static const struct tegra_pmc_regs tegra20_pmc_regs = {
	.scratch0 = 0x50,
	.dpd_req = 0x1b8,
	.dpd_status = 0x1bc,
	.dpd2_req = 0x1c0,
	.dpd2_status = 0x1c4,
};

static void tegra20_pmc_init(struct tegra_pmc *pmc)
{
	u32 value;

	/* Always enable CPU power request */
	value = tegra_pmc_readl(PMC_CNTRL);
	value |= PMC_CNTRL_CPU_PWRREQ_OE;
	tegra_pmc_writel(value, PMC_CNTRL);

	value = tegra_pmc_readl(PMC_CNTRL);

	if (pmc->sysclkreq_high)
		value &= ~PMC_CNTRL_SYSCLK_POLARITY;
	else
		value |= PMC_CNTRL_SYSCLK_POLARITY;

	/* configure the output polarity while the request is tristated */
	tegra_pmc_writel(value, PMC_CNTRL);

	/* now enable the request */
	value = tegra_pmc_readl(PMC_CNTRL);
	value |= PMC_CNTRL_SYSCLK_OE;
	tegra_pmc_writel(value, PMC_CNTRL);
}

static void tegra20_pmc_setup_irq_polarity(struct tegra_pmc *pmc,
					   struct device_node *np,
					   bool invert)
{
	u32 value;

	value = tegra_pmc_readl(PMC_CNTRL);

	if (invert)
		value |= PMC_CNTRL_INTR_POLARITY;
	else
		value &= ~PMC_CNTRL_INTR_POLARITY;

	tegra_pmc_writel(value, PMC_CNTRL);
}

static const struct tegra_pmc_soc tegra20_pmc_soc = {
	.num_powergates = ARRAY_SIZE(tegra20_powergates),
	.powergates = tegra20_powergates,
	.num_cpu_powergates = 0,
	.cpu_powergates = NULL,
	.has_tsense_reset = false,
	.has_gpu_clamps = false,
	.num_io_pads = 0,
	.io_pads = NULL,
	.regs = &tegra20_pmc_regs,
	.init = tegra20_pmc_init,
	.setup_irq_polarity = tegra20_pmc_setup_irq_polarity,
};

static const char * const tegra30_powergates[] = {
	[TEGRA_POWERGATE_CPU] = "cpu0",
	[TEGRA_POWERGATE_3D] = "3d0",
	[TEGRA_POWERGATE_VENC] = "venc",
	[TEGRA_POWERGATE_VDEC] = "vdec",
	[TEGRA_POWERGATE_PCIE] = "pcie",
	[TEGRA_POWERGATE_L2] = "l2",
	[TEGRA_POWERGATE_MPE] = "mpe",
	[TEGRA_POWERGATE_HEG] = "heg",
	[TEGRA_POWERGATE_SATA] = "sata",
	[TEGRA_POWERGATE_CPU1] = "cpu1",
	[TEGRA_POWERGATE_CPU2] = "cpu2",
	[TEGRA_POWERGATE_CPU3] = "cpu3",
	[TEGRA_POWERGATE_CELP] = "celp",
	[TEGRA_POWERGATE_3D1] = "3d1",
};

static const u8 tegra30_cpu_powergates[] = {
	TEGRA_POWERGATE_CPU,
	TEGRA_POWERGATE_CPU1,
	TEGRA_POWERGATE_CPU2,
	TEGRA_POWERGATE_CPU3,
};

static const struct tegra_pmc_soc tegra30_pmc_soc = {
	.num_powergates = ARRAY_SIZE(tegra30_powergates),
	.powergates = tegra30_powergates,
	.num_cpu_powergates = ARRAY_SIZE(tegra30_cpu_powergates),
	.cpu_powergates = tegra30_cpu_powergates,
	.has_tsense_reset = true,
	.has_gpu_clamps = false,
	.num_io_pads = 0,
	.io_pads = NULL,
	.regs = &tegra20_pmc_regs,
	.init = tegra20_pmc_init,
	.setup_irq_polarity = tegra20_pmc_setup_irq_polarity,
};

static const char * const tegra114_powergates[] = {
	[TEGRA_POWERGATE_CPU] = "crail",
	[TEGRA_POWERGATE_3D] = "3d",
	[TEGRA_POWERGATE_VENC] = "venc",
	[TEGRA_POWERGATE_VDEC] = "vdec",
	[TEGRA_POWERGATE_MPE] = "mpe",
	[TEGRA_POWERGATE_HEG] = "heg",
	[TEGRA_POWERGATE_CPU1] = "cpu1",
	[TEGRA_POWERGATE_CPU2] = "cpu2",
	[TEGRA_POWERGATE_CPU3] = "cpu3",
	[TEGRA_POWERGATE_CELP] = "celp",
	[TEGRA_POWERGATE_CPU0] = "cpu0",
	[TEGRA_POWERGATE_C0NC] = "c0nc",
	[TEGRA_POWERGATE_C1NC] = "c1nc",
	[TEGRA_POWERGATE_DIS] = "dis",
	[TEGRA_POWERGATE_DISB] = "disb",
	[TEGRA_POWERGATE_XUSBA] = "xusba",
	[TEGRA_POWERGATE_XUSBB] = "xusbb",
	[TEGRA_POWERGATE_XUSBC] = "xusbc",
};

static const u8 tegra114_cpu_powergates[] = {
	TEGRA_POWERGATE_CPU0,
	TEGRA_POWERGATE_CPU1,
	TEGRA_POWERGATE_CPU2,
	TEGRA_POWERGATE_CPU3,
};

static const struct tegra_pmc_soc tegra114_pmc_soc = {
	.num_powergates = ARRAY_SIZE(tegra114_powergates),
	.powergates = tegra114_powergates,
	.num_cpu_powergates = ARRAY_SIZE(tegra114_cpu_powergates),
	.cpu_powergates = tegra114_cpu_powergates,
	.has_tsense_reset = true,
	.has_gpu_clamps = false,
	.num_io_pads = 0,
	.io_pads = NULL,
	.regs = &tegra20_pmc_regs,
	.init = tegra20_pmc_init,
	.setup_irq_polarity = tegra20_pmc_setup_irq_polarity,
};

static const char * const tegra124_powergates[] = {
	[TEGRA_POWERGATE_CPU] = "crail",
	[TEGRA_POWERGATE_3D] = "3d",
	[TEGRA_POWERGATE_VENC] = "venc",
	[TEGRA_POWERGATE_PCIE] = "pcie",
	[TEGRA_POWERGATE_VDEC] = "vdec",
	[TEGRA_POWERGATE_MPE] = "mpe",
	[TEGRA_POWERGATE_HEG] = "heg",
	[TEGRA_POWERGATE_SATA] = "sata",
	[TEGRA_POWERGATE_CPU1] = "cpu1",
	[TEGRA_POWERGATE_CPU2] = "cpu2",
	[TEGRA_POWERGATE_CPU3] = "cpu3",
	[TEGRA_POWERGATE_CELP] = "celp",
	[TEGRA_POWERGATE_CPU0] = "cpu0",
	[TEGRA_POWERGATE_C0NC] = "c0nc",
	[TEGRA_POWERGATE_C1NC] = "c1nc",
	[TEGRA_POWERGATE_SOR] = "sor",
	[TEGRA_POWERGATE_DIS] = "dis",
	[TEGRA_POWERGATE_DISB] = "disb",
	[TEGRA_POWERGATE_XUSBA] = "xusba",
	[TEGRA_POWERGATE_XUSBB] = "xusbb",
	[TEGRA_POWERGATE_XUSBC] = "xusbc",
	[TEGRA_POWERGATE_VIC] = "vic",
	[TEGRA_POWERGATE_IRAM] = "iram",
};

static const u8 tegra124_cpu_powergates[] = {
	TEGRA_POWERGATE_CPU0,
	TEGRA_POWERGATE_CPU1,
	TEGRA_POWERGATE_CPU2,
	TEGRA_POWERGATE_CPU3,
};

#define TEGRA124_IO_PAD_CONFIG(_pin, _npins, _name, _dpd,	\
			       _vbit, _iopower, _reg)		\
	{							\
		.name =  #_name,				\
		.pins = {(_pin)},				\
		.npins = _npins,				\
		.dpd = _dpd,					\
		.voltage = _vbit,				\
		.io_power = _iopower,				\
		.dpd_req_reg = TEGRA_PMC_IO_##_reg##_REQ,	\
		.dpd_status_reg = TEGRA_PMC_IO_##_reg##_STATUS,	\
		.dpd_timer_reg = TEGRA_PMC_SEL_DPD_TIM,		\
		.dpd_sample_reg = TEGRA_PMC_IO_DPD_SAMPLE,	\
		.pwr_det_enable_reg = TEGRA_PMC_PWR_DET_ENABLE, \
		.pwr_det_val_reg = TEGRA_PMC_PWR_DET_VAL, \
	},

/**
 * All IO pads of Tegra SoCs do not support the low power and multi level
 * voltage configurations for its pads.
 * Defining macros for different cases as follows:
 * TEGRA_IO_PAD_LPONLY : IO pad which support low power state but
 *			 operate in single level of IO voltage.
 */
#define TEGRA124_IO_PAD_LPONLY(_pin, _name, _dpd, _reg)	\
	TEGRA124_IO_PAD_CONFIG(_pin, 1, _name, _dpd, UINT_MAX, UINT_MAX, _reg)

#define TEGRA124_IO_PAD_DESC_LP(_pin, _name, _dpd, _reg)	\
	{					\
		.number = _pin,			\
		.name = #_name,			\
	},

#define TEGRA124_IO_PAD_TABLE(_lponly_, _pvonly_, _lp_n_pv_)	\
	_lponly_(0, audio, 17, DPD)			\
	_lponly_(1, bb, 15, DPD)			\
	_lponly_(2, cam, 4, DPD2)			\
	_lponly_(3, comp, 22, DPD)			\
	_lponly_(4, csia, 0, DPD2)			\
	_lponly_(5, csib, 1, DPD2)			\
	_lponly_(6, csie, 12, DPD2)			\
	_lponly_(7, dp, 19, DPD2)			\
	_lponly_(8, dsi, 2, DPD)			\
	_lponly_(9, dsib, 7, DPD2)			\
	_lponly_(10, dsic, 8, DPD2)			\
	_lponly_(11, dsid, 9, DPD2)			\
	_lponly_(12, hdmi, 28, DPD)			\
	_lponly_(13, hsic, 19, DPD)			\
	_lponly_(14, lvds, 25, DPD2)			\
	_lponly_(15, mipi-bias, 3, DPD)			\
	_lponly_(16, nand, 13, DPD)			\
	_lponly_(17, pex-bias, 4, DPD)			\
	_lponly_(18, pex-clk1, 5, DPD)			\
	_lponly_(19, pex-clk2, 6, DPD)			\
	_lponly_(20, pex-ctrl, 0, DPD2)			\
	_lponly_(21, sdmmc1, 1, DPD2)		\
	_lponly_(22, sdmmc3, 2, DPD2)		\
	_lponly_(23, sdmmc4, 3, DPD2)		\
	_lponly_(24, sys-ddc, 26, DPD)		\
	_lponly_(25, uart, 14, DPD)		\
	_lponly_(26, usb0, 9, DPD)			\
	_lponly_(27, usb1, 10, DPD)			\
	_lponly_(28, usb2, 11, DPD)			\
	_lponly_(29, usb-bias, 12, DPD)			\

static const struct tegra_io_pad_soc tegra124_io_pads[] = {
	TEGRA124_IO_PAD_TABLE(TEGRA124_IO_PAD_LPONLY, NULL, NULL)
};

static const struct pinctrl_pin_desc tegra124_io_pads_pinctrl_desc[] = {
	TEGRA124_IO_PAD_TABLE(TEGRA124_IO_PAD_DESC_LP, NULL, NULL)
};

static const struct tegra_pmc_soc tegra124_pmc_soc = {
	.num_powergates = ARRAY_SIZE(tegra124_powergates),
	.powergates = tegra124_powergates,
	.num_cpu_powergates = ARRAY_SIZE(tegra124_cpu_powergates),
	.cpu_powergates = tegra124_cpu_powergates,
	.has_tsense_reset = true,
	.has_gpu_clamps = true,
	.num_io_pads = ARRAY_SIZE(tegra124_io_pads),
	.io_pads = tegra124_io_pads,
	.regs = &tegra20_pmc_regs,
	.init = tegra20_pmc_init,
	.setup_irq_polarity = tegra20_pmc_setup_irq_polarity,
	.num_descs = ARRAY_SIZE(tegra124_io_pads_pinctrl_desc),
	.descs = tegra124_io_pads_pinctrl_desc,
};

static const char * const tegra210_powergates[] = {
	[TEGRA_POWERGATE_CPU] = "crail",
	[TEGRA_POWERGATE_3D] = "3d",
	[TEGRA_POWERGATE_VENC] = "venc",
	[TEGRA_POWERGATE_PCIE] = "pcie",
	[TEGRA_POWERGATE_MPE] = "mpe",
	[TEGRA_POWERGATE_SATA] = "sata",
	[TEGRA_POWERGATE_CPU1] = "cpu1",
	[TEGRA_POWERGATE_CPU2] = "cpu2",
	[TEGRA_POWERGATE_CPU3] = "cpu3",
	[TEGRA_POWERGATE_CPU0] = "cpu0",
	[TEGRA_POWERGATE_C0NC] = "c0nc",
	[TEGRA_POWERGATE_SOR] = "sor",
	[TEGRA_POWERGATE_DIS] = "dis",
	[TEGRA_POWERGATE_DISB] = "disb",
	[TEGRA_POWERGATE_XUSBA] = "xusba",
	[TEGRA_POWERGATE_XUSBB] = "xusbb",
	[TEGRA_POWERGATE_XUSBC] = "xusbc",
	[TEGRA_POWERGATE_VIC] = "vic",
	[TEGRA_POWERGATE_IRAM] = "iram",
	[TEGRA_POWERGATE_NVDEC] = "nvdec",
	[TEGRA_POWERGATE_NVJPG] = "nvjpg",
	[TEGRA_POWERGATE_AUD] = "aud",
	[TEGRA_POWERGATE_DFD] = "dfd",
	[TEGRA_POWERGATE_VE2] = "ve2",
};

static const u8 tegra210_cpu_powergates[] = {
	TEGRA_POWERGATE_CPU0,
	TEGRA_POWERGATE_CPU1,
	TEGRA_POWERGATE_CPU2,
	TEGRA_POWERGATE_CPU3,
};

#define TEGRA210X_IO_PAD_CONFIG(_pin, _npins, _name, _dpd,	\
			       _vbit, _iopower, _reg, _bds)	\
	{							\
		.name =  #_name,				\
		.pins = {(_pin)},				\
		.npins = _npins,				\
		.dpd = _dpd,					\
		.voltage = _vbit,				\
		.io_power = _iopower,				\
		.dpd_req_reg = TEGRA_PMC_IO_##_reg##_REQ,	\
		.dpd_status_reg = TEGRA_PMC_IO_##_reg##_STATUS,	\
		.dpd_timer_reg = TEGRA_PMC_SEL_DPD_TIM,		\
		.dpd_sample_reg = TEGRA_PMC_IO_DPD_SAMPLE,	\
		.bdsdmem_cfc = _bds,				\
		.pwr_det_enable_reg = TEGRA_PMC_PWR_DET_ENABLE, \
		.pwr_det_val_reg = TEGRA_PMC_PWR_DET_VAL, \
		.pad_uv_0 = TEGRA_IO_PAD_VOLTAGE_1800000UV,	\
		.pad_uv_1 = TEGRA_IO_PAD_VOLTAGE_3300000UV,	\
	},

#define TEGRA210_IO_PAD_CONFIG(_pin, _npins, _name, _dpd,	\
			       _vbit, _iopower, _reg)		\
	TEGRA210X_IO_PAD_CONFIG(_pin, _npins, _name, _dpd,	\
				_vbit, _iopower, _reg, false)

/**
 * All IO pads of Tegra SoCs do not support the low power and multi level
 * voltage configurations for its pads.
 * Defining macros for different cases as follows:
 * TEGRA_IO_PAD_LPONLY : IO pad which support low power state but
 *			 operate in single level of IO voltage.
 * TEGRA_IO_PAD_LP_N_PV: IO pad which support low power state as well as
 *			 it can operate in multi-level voltages.
 * TEGRA_IO_PAD_PVONLY:  IO pad which does not support low power state but
 *			 it can operate in multi-level voltages.
 */
#define TEGRA210_IO_PAD_LPONLY(_pin, _name, _dpd, _reg)	\
	TEGRA210_IO_PAD_CONFIG(_pin, 1, _name, _dpd, UINT_MAX, UINT_MAX, _reg)

#define TEGRA210_IO_PAD_LP_N_PV(_pin, _name, _dpd, _vbit, _io, _reg)  \
	TEGRA210_IO_PAD_CONFIG(_pin, 1, _name, _dpd, _vbit, _io, _reg)

#define TEGRA210_IO_PAD_PVONLY(_pin, _name, _vbit, _io, _reg)	\
	TEGRA210_IO_PAD_CONFIG(_pin, 0, _name, UINT_MAX, _vbit, _io, _reg)

#define TEGRA210_IO_PAD_DESC_LP(_pin, _name, _dpd, _reg)	\
	{					\
		.number = _pin,			\
		.name = #_name,			\
	},
#define TEGRA210_IO_PAD_DESC_LP_N_PV(_pin, _name, _dpd, _vbit, _io, _reg) \
	TEGRA210_IO_PAD_DESC_LP(_pin, _name, _dpd, _reg)

#define TEGRA210_IO_PAD_DESC_PV(_pin, _name, _vbit, _io, _reg) \
	TEGRA210_IO_PAD_DESC_LP(_pin, _name, UINT_MAX, _reg)

#define TEGRA210_IO_PAD_TABLE(_lponly_, _pvonly_, _lp_n_pv_)	\
	_lp_n_pv_(0, audio, 17, 5, 5, DPD)		\
	_lp_n_pv_(1, audio-hv, 29, 18, 18, DPD2)	\
	_lp_n_pv_(2, cam, 4, 10, 10, DPD2)		\
	_lponly_(3, csia, 0, DPD)			\
	_lponly_(4, csib, 1, DPD)			\
	_lponly_(5, csic, 10, DPD2)			\
	_lponly_(6, csid, 11, DPD2)			\
	_lponly_(7, csie, 12, DPD2)			\
	_lponly_(8, csif, 13, DPD2)			\
	_lp_n_pv_(9, dbg, 25, 19, 19, DPD)		\
	_lponly_(10, debug-nonao, 26, DPD)		\
	_lp_n_pv_(11, dmic, 18, 20, 20, DPD2)		\
	_lponly_(12, dp, 19, DPD2)			\
	_lponly_(13, dsi, 2, DPD)			\
	_lponly_(14, dsib, 7, DPD2)			\
	_lponly_(15, dsic, 8, DPD2)			\
	_lponly_(16, dsid, 9, DPD2)			\
	_lponly_(17, emmc, 3, DPD2)			\
	_lponly_(18, emmc2, 5, DPD2)			\
	_lp_n_pv_(19, gpio, 27, 21, 21, DPD)		\
	_lponly_(20, hdmi, 28, DPD)			\
	_lponly_(21, hsic, 19, DPD)			\
	_lponly_(22, lvds, 25, DPD2)			\
	_lponly_(23, mipi-bias, 3, DPD)			\
	_lponly_(24, pex-bias, 4, DPD)			\
	_lponly_(25, pex-clk1, 5, DPD)			\
	_lponly_(26, pex-clk2, 6, DPD)			\
	_pvonly_(27, pex-ctrl, 11, 11, DPD2)		\
	_lp_n_pv_(28, sdmmc1, 1, 12, 12, DPD2)		\
	_lp_n_pv_(29, sdmmc3, 2, 13, 13, DPD2)		\
	_lp_n_pv_(30, spi, 14, 22, 22, DPD2)		\
	_lp_n_pv_(31, spi-hv, 15, 23, 23, DPD2)		\
	_lp_n_pv_(32, uart, 14, 2, 2, DPD)		\
	_lponly_(33, usb0, 9, DPD)			\
	_lponly_(34, usb1, 10, DPD)			\
	_lponly_(35, usb2, 11, DPD)			\
	_lponly_(36, usb3, 18, DPD)			\
	_lponly_(37, usb-bias, 12, DPD)			\
	_pvonly_(38, sys, 12, UINT_MAX, DPD)

static const struct tegra_io_pad_soc tegra210_io_pads[] = {
	TEGRA210_IO_PAD_TABLE(TEGRA210_IO_PAD_LPONLY, TEGRA210_IO_PAD_PVONLY,
			      TEGRA210_IO_PAD_LP_N_PV)
};

static const struct pinctrl_pin_desc tegra210_io_pads_pinctrl_desc[] = {
	TEGRA210_IO_PAD_TABLE(TEGRA210_IO_PAD_DESC_LP, TEGRA210_IO_PAD_DESC_PV,
			      TEGRA210_IO_PAD_DESC_LP_N_PV)
};

static const struct tegra_pmc_soc tegra210_pmc_soc = {
	.num_powergates = ARRAY_SIZE(tegra210_powergates),
	.powergates = tegra210_powergates,
	.num_cpu_powergates = ARRAY_SIZE(tegra210_cpu_powergates),
	.cpu_powergates = tegra210_cpu_powergates,
	.has_tsense_reset = true,
	.has_gpu_clamps = true,
	.needs_mbist_war = true,
	.num_io_pads = ARRAY_SIZE(tegra210_io_pads),
	.io_pads = tegra210_io_pads,
	.regs = &tegra20_pmc_regs,
	.num_descs = ARRAY_SIZE(tegra210_io_pads_pinctrl_desc),
	.descs = tegra210_io_pads_pinctrl_desc,
	.init = tegra20_pmc_init,
	.setup_irq_polarity = tegra20_pmc_setup_irq_polarity,
};

static const struct tegra_pmc_regs tegra186_pmc_regs = {
	.scratch0 = 0x2000,
	.dpd_req = 0x74,
	.dpd_status = 0x78,
	.dpd2_req = 0x7c,
	.dpd2_status = 0x80,
};

static void tegra186_pmc_setup_irq_polarity(struct tegra_pmc *pmc,
					    struct device_node *np,
					    bool invert)
{
	struct resource regs;
	void __iomem *wake;
	u32 value;
	int index;

	index = of_property_match_string(np, "reg-names", "wake");
	if (index < 0) {
		pr_err("failed to find PMC wake registers\n");
		return;
	}

	of_address_to_resource(np, index, &regs);

	wake = ioremap_nocache(regs.start, resource_size(&regs));
	if (!wake) {
		pr_err("failed to map PMC wake registers\n");
		return;
	}

	value = readl(wake + WAKE_AOWAKE_CTRL);

	if (invert)
		value |= WAKE_AOWAKE_CTRL_INTR_POLARITY;
	else
		value &= ~WAKE_AOWAKE_CTRL_INTR_POLARITY;

	writel(value, wake + WAKE_AOWAKE_CTRL);

	iounmap(wake);
}

#define TEGRA186_IO_PAD_CONFIG(_pin, _npins, _name, _dpd_reg, _dpd_bit,     \
			       _padv_reg, _padv_bit, _v0, _v1, _iopwr_bit,  \
			       _bds)  \
	{							\
		.name =  #_name,				\
		.pins = {(_pin)},				\
		.npins = _npins,				\
		.dpd_req_reg = TEGRA_PMC_IO_##_dpd_reg##_REQ,	\
		.dpd_status_reg = TEGRA_PMC_IO_##_dpd_reg##_STATUS,	\
		.dpd_timer_reg = TEGRA_PMC_SEL_DPD_TIM,		\
		.dpd_sample_reg = TEGRA_PMC_IO_DPD_SAMPLE,	\
		.dpd = _dpd_bit,				\
		.pwr_det_val_reg = TEGRA_PMC_##_padv_reg##_PWR, \
		.pwr_det_enable_reg = UINT_MAX,		\
		.pad_uv_0 = TEGRA_IO_PAD_VOLTAGE_##_v0##000UV,	\
		.pad_uv_1 = TEGRA_IO_PAD_VOLTAGE_##_v1##000UV,	\
		.voltage = _padv_bit,				\
		.io_power = _iopwr_bit,				\
		.bdsdmem_cfc = _bds,				\
	},

#define TEGRA186_IO_PAD_LPONLY(_pin, _name, _dpd_reg, _dpd_bit, _iopwr_bit, \
			       _bds)					    \
	TEGRA186_IO_PAD_CONFIG(_pin, 1, _name, _dpd_reg, _dpd_bit,	    \
			       E_33V, UINT_MAX, 1200, 1200, _iopwr_bit, _bds)

#define TEGRA186_IO_PAD_LP_N_PV(_pin, _name, _dpd_reg, _dpd_bit, _padv_reg, \
				_padv_bit, _v0, _v1, _iopwr_bit, _bds)	    \
	TEGRA186_IO_PAD_CONFIG(_pin, 1, _name, _dpd_reg, _dpd_bit,	    \
			       _padv_reg, _padv_bit, _v0, _v1, _iopwr_bit,  \
			       _bds)

#define TEGRA186_IO_PAD_PVONLY(_pin, _name, _padv_reg, _padv_bit, _v0, _v1, \
			       _iopwr_bit, _bds)			    \
	TEGRA186_IO_PAD_CONFIG(_pin, 1, _name, DPD, UINT_MAX, _padv_reg,    \
			       _padv_bit, _v0, _v1, _iopwr_bit, _bds)

#define TEGRA186_IO_PAD_DESC_LP(_pin, _name, _dpd_reg, _dpd_bit, _iopwr_bit, \
				_bds)					     \
	{								\
		.number = _pin,						\
		.name = #_name,						\
	},

#define TEGRA186_IO_PAD_DESC_LP_N_PV(_pin, _name, _dpd_reg, _dpd_bit,	\
				     _padv_reg, _padv_bit, _v0, _v1,	\
				     _iopwr_bit, _bds)			\
	TEGRA186_IO_PAD_DESC_LP(_pin, _name, _dpd_reg, _dpd_bit,	\
				_iopwr_bit, _bds)

#define TEGRA186_IO_PAD_DESC_PV(_pin, _name, _padv_reg, _padv_bit, _v0, _v1, \
			       _iopwr_bit, _bds)			    \
	TEGRA186_IO_PAD_DESC_LP(_pin, _name, UINT_MAX, UINT_MAX,	    \
				UINT_MAX, UINT_MAX)

#define TEGRA186_IO_PAD_TABLE(_lponly_, _pvonly_, _lp_n_pv_)	\
	_lponly_(0, csia, DPD, 0, UINT_MAX, false)			\
	_lponly_(1, csib, DPD, 1, UINT_MAX, false)			\
	_lponly_(2, dsi, DPD, 2, UINT_MAX, false)			\
	_lponly_(3, mipi-bias, DPD, 3, 9, false)			\
	_lponly_(4, pex-clk_bias, DPD, 4, UINT_MAX, false)		\
	_lponly_(5, pex-clk3, DPD, 5, UINT_MAX, false)			\
	_lponly_(6, pex-clk2, DPD, 6, UINT_MAX, false)			\
	_lponly_(7, pex-clk1, DPD, 7, UINT_MAX, false)			\
	_lponly_(8, usb0, DPD, 9, UINT_MAX, false)			\
	_lponly_(9, usb1, DPD, 10, UINT_MAX, false)			\
	_lponly_(10, usb2, DPD, 11, UINT_MAX, false)			\
	_lponly_(11, usb-bias, DPD, 12, UINT_MAX, false)		\
	_lponly_(12, uart, DPD, 14, 2, false)				\
	_lponly_(13, audio, DPD, 17, 5, false)				\
	_lponly_(14, hsic, DPD, 19, UINT_MAX, false)			\
	_lp_n_pv_(15, dbg, DPD, 25, E_18V, 4, 1200, 1800, 19, false)	\
	_lponly_(16, hdmi-dp0, DPD, 28, UINT_MAX, false)		\
	_lponly_(17, hdmi-dp1, DPD, 29, UINT_MAX, false)		\
	_lponly_(18, pex-ctrl, DPD2, 0, 11, false)			\
	_lp_n_pv_(19, sdmmc2-hv, DPD2, 2, E_33V, 5, 1800, 3300, 30, true) \
	_lponly_(20, sdmmc4, DPD2, 4, 14, false)			\
	_lponly_(21, cam, DPD2, 6, 10, false)				\
	_lponly_(22, dsib, DPD2, 8, UINT_MAX, false)			\
	_lponly_(23, dsic, DPD2, 9, UINT_MAX, false)			\
	_lponly_(24, dsid, DPD2, 10, UINT_MAX, false)			\
	_lponly_(25, csic, DPD2, 11, UINT_MAX, false)			\
	_lponly_(26, csid, DPD2, 12, UINT_MAX, false)			\
	_lponly_(27, csie, DPD2, 13, UINT_MAX, false)			\
	_lponly_(28, csif, DPD2, 14, UINT_MAX, false)			\
	_lp_n_pv_(29, spi, DPD2, 15, E_18V, 5, 1200, 1800, 22, false)	\
	_lp_n_pv_(30, ufs, DPD2, 17, E_18V, 0, 1200, 1800, 6, false)	\
	_lp_n_pv_(31, dmic-hv, DPD2, 20, E_33V, 2, 1800, 3300, 28, true) \
	_lponly_(32, edp, DPD2, 21, 4, false)				\
	_lp_n_pv_(33, sdmmc1-hv, DPD2, 23, E_33V, 4, 1800, 3300, 15, true) \
	_lp_n_pv_(34, sdmmc3-hv, DPD2, 24, E_33V, 6, 1800, 3300, 31, true) \
	_lponly_(35, conn, DPD2, 28, 3, false)				\
	_lp_n_pv_(36, audio-hv, DPD2, 29, E_33V, 1, 1800, 3300, 18, true) \
	_pvonly_(37, ao-hv, E_33V, 0, 1800, 3300, 27, true)

static const struct tegra_io_pad_soc tegra186_io_pads[] = {
	TEGRA186_IO_PAD_TABLE(TEGRA186_IO_PAD_LPONLY, TEGRA186_IO_PAD_PVONLY,
			      TEGRA186_IO_PAD_LP_N_PV)
};

static const struct pinctrl_pin_desc tegra186_io_pads_pinctrl_desc[] = {
	TEGRA186_IO_PAD_TABLE(TEGRA186_IO_PAD_DESC_LP, TEGRA186_IO_PAD_DESC_PV,
			      TEGRA186_IO_PAD_DESC_LP_N_PV)
};

static const struct tegra_pmc_soc tegra186_pmc_soc = {
	.num_powergates = 0,
	.powergates = NULL,
	.num_cpu_powergates = 0,
	.cpu_powergates = NULL,
	.has_tsense_reset = false,
	.has_gpu_clamps = false,
	.num_io_pads = ARRAY_SIZE(tegra186_io_pads),
	.io_pads = tegra186_io_pads,
	.num_descs = ARRAY_SIZE(tegra186_io_pads_pinctrl_desc),
	.descs = tegra186_io_pads_pinctrl_desc,
	.regs = &tegra186_pmc_regs,
	.init = NULL,
	.setup_irq_polarity = tegra186_pmc_setup_irq_polarity,
};

static const struct of_device_id tegra_pmc_match[] = {
	{ .compatible = "nvidia,tegra194-pmc", .data = &tegra186_pmc_soc },
	{ .compatible = "nvidia,tegra186-pmc", .data = &tegra186_pmc_soc },
	{ .compatible = "nvidia,tegra210-pmc", .data = &tegra210_pmc_soc },
	{ .compatible = "nvidia,tegra132-pmc", .data = &tegra124_pmc_soc },
	{ .compatible = "nvidia,tegra124-pmc", .data = &tegra124_pmc_soc },
	{ .compatible = "nvidia,tegra114-pmc", .data = &tegra114_pmc_soc },
	{ .compatible = "nvidia,tegra30-pmc", .data = &tegra30_pmc_soc },
	{ .compatible = "nvidia,tegra20-pmc", .data = &tegra20_pmc_soc },
	{ }
};

static struct platform_driver tegra_pmc_driver = {
	.driver = {
		.name = "tegra-pmc",
		.suppress_bind_attrs = true,
		.of_match_table = tegra_pmc_match,
#if defined(CONFIG_PM_SLEEP) && defined(CONFIG_ARM)
		.pm = &tegra_pmc_pm_ops,
#endif
	},
	.probe = tegra_pmc_probe,
};
builtin_platform_driver(tegra_pmc_driver);

/*
 * Early initialization to allow access to registers in the very early boot
 * process.
 */
static int __init tegra_pmc_early_init(void)
{
	const struct of_device_id *match;
	struct device_node *np;
	struct resource regs;
	bool invert;

	mutex_init(&pmc->powergates_lock);

	np = of_find_matching_node_and_match(NULL, tegra_pmc_match, &match);
	if (!np) {
		/*
		 * Fall back to legacy initialization for 32-bit ARM only. All
		 * 64-bit ARM device tree files for Tegra are required to have
		 * a PMC node.
		 *
		 * This is for backwards-compatibility with old device trees
		 * that didn't contain a PMC node. Note that in this case the
		 * SoC data can't be matched and therefore powergating is
		 * disabled.
		 */
		if (IS_ENABLED(CONFIG_ARM) && soc_is_tegra()) {
			pr_warn("DT node not found, powergating disabled\n");

			regs.start = 0x7000e400;
			regs.end = 0x7000e7ff;
			regs.flags = IORESOURCE_MEM;

			pr_warn("Using memory region %pR\n", &regs);
		} else {
			/*
			 * At this point we're not running on Tegra, so play
			 * nice with multi-platform kernels.
			 */
			return 0;
		}
	} else {
		/*
		 * Extract information from the device tree if we've found a
		 * matching node.
		 */
		if (of_address_to_resource(np, 0, &regs) < 0) {
			pr_err("failed to get PMC registers\n");
			of_node_put(np);
			return -ENXIO;
		}
	}

	pmc->base = ioremap_nocache(regs.start, resource_size(&regs));
	if (!pmc->base) {
		pr_err("failed to map PMC registers\n");
		of_node_put(np);
		return -ENXIO;
	}

	if (np) {
		pmc->soc = match->data;

		tegra_powergate_init(pmc, np);

		/*
		 * Invert the interrupt polarity if a PMC device tree node
		 * exists and contains the nvidia,invert-interrupt property.
		 */
		invert = of_property_read_bool(np, "nvidia,invert-interrupt");

		pmc->soc->setup_irq_polarity(pmc, np, invert);

		of_node_put(np);
	}

	return 0;
}
early_initcall(tegra_pmc_early_init);
