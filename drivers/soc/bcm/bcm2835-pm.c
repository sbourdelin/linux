// SPDX-License-Identifier: GPL-2.0+
/*
 * PM driver for Broadcom BCM2835
 *
 * "bcm2708_wdog" driver written by Luke Diamand that was obtained from
 * branch "rpi-3.6.y" of git://github.com/raspberrypi/linux.git was used
 * as a hardware reference for the Broadcom BCM2835 watchdog timer.
 *
 * Copyright (C) 2018 Broadcom
 * Copyright (C) 2013 Lubomir Rintel <lkundrak@v3.sk>
 *
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/types.h>
#include <linux/module.h>
#include <linux/io.h>
#include <linux/watchdog.h>
#include <linux/platform_device.h>
#include <linux/reset-controller.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/pm_domain.h>
#include <dt-bindings/soc/bcm2835-pm.h>

#define PM_GNRIC                        0x00
#define PM_AUDIO                        0x04
#define PM_STATUS                       0x18
#define PM_RSTC				0x1c
#define PM_RSTS				0x20
#define PM_WDOG				0x24
#define PM_PADS0			0x28
#define PM_PADS2			0x2c
#define PM_PADS3			0x30
#define PM_PADS4			0x34
#define PM_PADS5			0x38
#define PM_PADS6			0x3c
#define PM_CAM0				0x44
#define PM_CAM0_LDOHPEN			BIT(2)
#define PM_CAM0_LDOLPEN			BIT(1)
#define PM_CAM0_CTRLEN			BIT(0)

#define PM_CAM1				0x48
#define PM_CAM1_LDOHPEN			BIT(2)
#define PM_CAM1_LDOLPEN			BIT(1)
#define PM_CAM1_CTRLEN			BIT(0)

#define PM_CCP2TX			0x4c
#define PM_CCP2TX_LDOEN			BIT(1)
#define PM_CCP2TX_CTRLEN		BIT(0)

#define PM_DSI0				0x50
#define PM_DSI0_LDOHPEN			BIT(2)
#define PM_DSI0_LDOLPEN			BIT(1)
#define PM_DSI0_CTRLEN			BIT(0)

#define PM_DSI1				0x54
#define PM_DSI1_LDOHPEN			BIT(2)
#define PM_DSI1_LDOLPEN			BIT(1)
#define PM_DSI1_CTRLEN			BIT(0)

#define PM_HDMI				0x58
#define PM_HDMI_RSTDR			BIT(19)
#define PM_HDMI_LDOPD			BIT(1)
#define PM_HDMI_CTRLEN			BIT(0)

#define PM_USB				0x5c
/* The power gates must be enabled with this bit before enabling the LDO in the
 * USB block.
 */
#define PM_USB_CTRLEN			BIT(0)

#define PM_PXLDO			0x60
#define PM_PXBG				0x64
#define PM_DFT				0x68
#define PM_SMPS				0x6c
#define PM_XOSC				0x70
#define PM_SPAREW			0x74
#define PM_SPARER			0x78
#define PM_AVS_RSTDR			0x7c
#define PM_AVS_STAT			0x80
#define PM_AVS_EVENT			0x84
#define PM_AVS_INTEN			0x88
#define PM_DUMMY			0xfc

#define PM_IMAGE			0x108
#define PM_GRAFX			0x10c
#define PM_PROC				0x110
#define PM_ENAB				BIT(12)
#define PM_ISPRSTN			BIT(8)
#define PM_H264RSTN			BIT(7)
#define PM_PERIRSTN			BIT(6)
#define PM_V3DRSTN			BIT(6)
#define PM_ISFUNC			BIT(5)
#define PM_MRDONE			BIT(4)
#define PM_MEMREP			BIT(3)
#define PM_ISPOW			BIT(2)
#define PM_POWOK			BIT(1)
#define PM_POWUP			BIT(0)
#define PM_INRUSH_SHIFT			13
#define PM_INRUSH_3_5_MA		0
#define PM_INRUSH_5_MA			1
#define PM_INRUSH_10_MA			2
#define PM_INRUSH_20_MA			3
#define PM_INRUSH_MASK			(3 << PM_INRUSH_SHIFT)

#define PM_PASSWORD			0x5a000000

#define PM_WDOG_TIME_SET		0x000fffff
#define PM_RSTC_WRCFG_CLR		0xffffffcf
#define PM_RSTS_HADWRH_SET		0x00000040
#define PM_RSTC_WRCFG_SET		0x00000030
#define PM_RSTC_WRCFG_FULL_RESET	0x00000020
#define PM_RSTC_RESET			0x00000102

#define PM_READ(reg) readl(pm->base + (reg))
#define PM_WRITE(reg, val) writel(PM_PASSWORD | (val), pm->base + (reg))

#define ASB_BRDG_VERSION                0x00
#define ASB_CPR_CTRL                    0x04

#define ASB_V3D_S_CTRL			0x08
#define ASB_V3D_M_CTRL			0x0c
#define ASB_ISP_S_CTRL			0x10
#define ASB_ISP_M_CTRL			0x14
#define ASB_H264_S_CTRL			0x18
#define ASB_H264_M_CTRL			0x1c

#define ASB_REQ_STOP                    BIT(0)
#define ASB_ACK                         BIT(1)
#define ASB_EMPTY                       BIT(2)
#define ASB_FULL                        BIT(3)

#define ASB_AXI_BRDG_ID			0x20

#define ASB_READ(reg) readl(pm->asb + (reg))
#define ASB_WRITE(reg, val) writel(PM_PASSWORD | (val), pm->asb + (reg))

/*
 * The Raspberry Pi firmware uses the RSTS register to know which partition
 * to boot from. The partition value is spread into bits 0, 2, 4, 6, 8, 10.
 * Partition 63 is a special partition used by the firmware to indicate halt.
 */
#define PM_RSTS_RASPBERRYPI_HALT	0x555

#define SECS_TO_WDOG_TICKS(x) ((x) << 16)
#define WDOG_TICKS_TO_SECS(x) ((x) >> 16)

struct bcm2835_power_domain {
	struct generic_pm_domain base;
	struct bcm2835_pm *pm;
	u32 domain;
	struct clk *clk;
};

struct bcm2835_pm {
	struct device		*dev;
	/* PM registers. */
	void __iomem		*base;
	/* AXI Async bridge registers. */
	void __iomem		*asb;
	spinlock_t		lock;

	struct genpd_onecell_data pd_xlate;
	struct bcm2835_power_domain domains[BCM2835_POWER_DOMAIN_COUNT];
	struct reset_controller_dev reset;
};

static unsigned int heartbeat;
static bool nowayout = WATCHDOG_NOWAYOUT;

static bool bcm2835_wdt_is_running(struct bcm2835_pm *pm)
{
	uint32_t cur;

	cur = PM_READ(PM_RSTC);

	return !!(cur & PM_RSTC_WRCFG_FULL_RESET);
}

static int bcm2835_wdt_start(struct watchdog_device *wdog)
{
	struct bcm2835_pm *pm = watchdog_get_drvdata(wdog);
	uint32_t cur;
	unsigned long flags;

	spin_lock_irqsave(&pm->lock, flags);

	PM_WRITE(PM_WDOG, SECS_TO_WDOG_TICKS(wdog->timeout) & PM_WDOG_TIME_SET);
	cur = PM_READ(PM_RSTC);
	PM_WRITE(PM_RSTC, (cur & PM_RSTC_WRCFG_CLR) | PM_RSTC_WRCFG_FULL_RESET);

	spin_unlock_irqrestore(&pm->lock, flags);

	return 0;
}

static int bcm2835_wdt_stop(struct watchdog_device *wdog)
{
	struct bcm2835_pm *pm = watchdog_get_drvdata(wdog);

	PM_WRITE(PM_RSTC, PM_RSTC_RESET);
	return 0;
}

static unsigned int bcm2835_wdt_get_timeleft(struct watchdog_device *wdog)
{
	struct bcm2835_pm *pm = watchdog_get_drvdata(wdog);

	uint32_t ret = PM_READ(PM_WDOG);
	return WDOG_TICKS_TO_SECS(ret & PM_WDOG_TIME_SET);
}

static void __bcm2835_restart(struct bcm2835_pm *pm)
{
	u32 val;

	/* use a timeout of 10 ticks (~150us) */
	writel(10 | PM_PASSWORD, pm->base + PM_WDOG);
	val = PM_READ(PM_RSTC);
	val &= PM_RSTC_WRCFG_CLR;
	val |= PM_PASSWORD | PM_RSTC_WRCFG_FULL_RESET;
	writel(val, pm->base + PM_RSTC);

	/* No sleeping, possibly atomic. */
	mdelay(1);
}

static int bcm2835_restart(struct watchdog_device *wdog,
			   unsigned long action, void *data)
{
	struct bcm2835_pm *pm = watchdog_get_drvdata(wdog);

	__bcm2835_restart(pm);

	return 0;
}

static const struct watchdog_ops bcm2835_wdt_ops = {
	.owner =	THIS_MODULE,
	.start =	bcm2835_wdt_start,
	.stop =		bcm2835_wdt_stop,
	.get_timeleft =	bcm2835_wdt_get_timeleft,
	.restart =	bcm2835_restart,
};

static const struct watchdog_info bcm2835_wdt_info = {
	.options =	WDIOF_SETTIMEOUT | WDIOF_MAGICCLOSE |
			WDIOF_KEEPALIVEPING,
	.identity =	"Broadcom BCM2835 Watchdog timer",
};

static struct watchdog_device bcm2835_wdt_wdd = {
	.info =		&bcm2835_wdt_info,
	.ops =		&bcm2835_wdt_ops,
	.min_timeout =	1,
	.max_timeout =	WDOG_TICKS_TO_SECS(PM_WDOG_TIME_SET),
	.timeout =	WDOG_TICKS_TO_SECS(PM_WDOG_TIME_SET),
};

static int bcm2835_asb_enable(struct bcm2835_pm *pm, u32 reg)
{
	u64 start = ktime_get_ns();

	/* Enable the module's async AXI bridges. */
	ASB_WRITE(reg, ASB_READ(reg) & ~ASB_REQ_STOP);
	while (ASB_READ(reg) & ASB_ACK) {
		cpu_relax();
		if (ktime_get_ns() - start >= 1000)
			return -ETIMEDOUT;
	}

	return 0;
}

static int bcm2835_asb_disable(struct bcm2835_pm *pm, u32 reg)
{
	u64 start = ktime_get_ns();

	/* Enable the module's async AXI bridges. */
	ASB_WRITE(reg, ASB_READ(reg) | ASB_REQ_STOP);
	while (!(ASB_READ(reg) & ASB_ACK)) {
		cpu_relax();
		if (ktime_get_ns() - start >= 1000)
			return -ETIMEDOUT;
	}

	return 0;
}

static int bcm2835_pm_power_off(struct bcm2835_power_domain *pd, u32 pm_reg)
{
	struct bcm2835_pm *pm = pd->pm;

	/* Enable functional isolation */
	PM_WRITE(pm_reg, PM_READ(pm_reg) & ~PM_ISFUNC);

	/* Enable electrical isolation */
	PM_WRITE(pm_reg, PM_READ(pm_reg) & ~PM_ISPOW);

	/* Open the power switches. */
	PM_WRITE(pm_reg, PM_READ(pm_reg) & ~PM_POWUP);

	return 0;
}

static int bcm2835_pm_power_on(struct bcm2835_power_domain *pd, u32 pm_reg)
{
	struct bcm2835_pm *pm = pd->pm;
	struct device *dev = pm->dev;
	u64 start;
	int ret;
	int inrush;
	bool powok;

	/* If it was already powered on by the fw, leave it that way. */
	if (PM_READ(pm_reg) & PM_POWUP)
		return 0;

	/* Enable power.  Allowing too much current at once may result
	 * in POWOK never getting set, so start low and ramp it up as
	 * necessary to succeed.
	 */
	powok = false;
	for (inrush = PM_INRUSH_3_5_MA; inrush <= PM_INRUSH_20_MA; inrush++) {
		PM_WRITE(pm_reg,
			 (PM_READ(pm_reg) & ~PM_INRUSH_MASK) |
			 (inrush << PM_INRUSH_SHIFT) |
			 PM_POWUP);

		start = ktime_get_ns();
		while (!(powok = !!(PM_READ(pm_reg) & PM_POWOK))) {
			cpu_relax();
			if (ktime_get_ns() - start >= 3000)
				break;
		}
	}
	if (!powok) {
		dev_err(dev, "Timeout waiting for %s power OK\n",
			pd->base.name);
		ret = -ETIMEDOUT;
		goto err_disable_powup;
	}

	/* Disable electrical isolation */
	PM_WRITE(pm_reg, PM_READ(pm_reg) | PM_ISPOW);

	/* Repair memory */
	PM_WRITE(pm_reg, PM_READ(pm_reg) | PM_MEMREP);
	start = ktime_get_ns();
	while (!(PM_READ(pm_reg) & PM_MRDONE)) {
		cpu_relax();
		if (ktime_get_ns() - start >= 1000) {
			dev_err(dev, "Timeout waiting for %s memory repair\n",
				pd->base.name);
			ret = -ETIMEDOUT;
			goto err_disable_ispow;
		}
	}

	/* Disable functional isolation */
	PM_WRITE(pm_reg, PM_READ(pm_reg) | PM_ISFUNC);

	return 0;

err_disable_ispow:
	PM_WRITE(pm_reg, PM_READ(pm_reg) & ~PM_ISPOW);
err_disable_powup:
	PM_WRITE(pm_reg, PM_READ(pm_reg) & ~(PM_POWUP | PM_INRUSH_MASK));
	return ret;
}

static int bcm2835_asb_power_on(struct bcm2835_power_domain *pd,
				u32 pm_reg,
				u32 asb_m_reg,
				u32 asb_s_reg,
				u32 reset_flags)
{
	struct bcm2835_pm *pm = pd->pm;
	int ret;

	ret = clk_prepare_enable(pd->clk);
	if (ret) {
		dev_err(pm->dev, "Failed to enable clock for %s\n",
			pd->base.name);
		return ret;
	}

	/* Wait 32 clocks for reset to propagate, 1 us will be enough */
	udelay(1);

	clk_disable_unprepare(pd->clk);

	/* Deassert the resets. */
	PM_WRITE(pm_reg, PM_READ(pm_reg) | reset_flags);

	ret = clk_prepare_enable(pd->clk);
	if (ret) {
		dev_err(pm->dev, "Failed to enable clock for %s\n",
			pd->base.name);
		goto err_enable_resets;
	}

	ret = bcm2835_asb_enable(pm, asb_m_reg);
	if (ret) {
		dev_err(pm->dev, "Failed to enable ASB master for %s\n",
			pd->base.name);
		goto err_disable_clk;
	}
	ret = bcm2835_asb_enable(pm, asb_s_reg);
	if (ret) {
		dev_err(pm->dev, "Failed to enable ASB slave for %s\n",
			pd->base.name);
		goto err_disable_asb_master;
	}

	return 0;

err_disable_asb_master:
	bcm2835_asb_disable(pm, asb_m_reg);
err_disable_clk:
	clk_disable_unprepare(pd->clk);
err_enable_resets:
	PM_WRITE(pm_reg, PM_READ(pm_reg) & ~reset_flags);
	return ret;
}

static int bcm2835_asb_power_off(struct bcm2835_power_domain *pd,
				 u32 pm_reg,
				 u32 asb_m_reg,
				 u32 asb_s_reg,
				 u32 reset_flags)
{
	struct bcm2835_pm *pm = pd->pm;
	int ret;

	ret = bcm2835_asb_disable(pm, asb_s_reg);
	if (ret) {
		dev_warn(pm->dev, "Failed to disable ASB slave for %s\n",
			 pd->base.name);
		return ret;
	}
	ret = bcm2835_asb_disable(pm, asb_m_reg);
	if (ret) {
		dev_warn(pm->dev, "Failed to disable ASB master for %s\n",
			 pd->base.name);
		bcm2835_asb_enable(pm, asb_s_reg);
		return ret;
	}

	clk_disable_unprepare(pd->clk);

	/* Assert the resets. */
	PM_WRITE(pm_reg, PM_READ(pm_reg) & ~reset_flags);

	return 0;
}

static int bcm2835_pm_pd_power_on(struct generic_pm_domain *domain)
{
	struct bcm2835_power_domain *pd =
		container_of(domain, struct bcm2835_power_domain, base);
	struct bcm2835_pm *pm = pd->pm;

	switch (pd->domain) {
	case BCM2835_POWER_DOMAIN_GRAFX:
		return bcm2835_pm_power_on(pd, PM_GRAFX);

	case BCM2835_POWER_DOMAIN_GRAFX_V3D:
		return bcm2835_asb_power_on(pd, PM_GRAFX,
					    ASB_V3D_M_CTRL, ASB_V3D_S_CTRL,
					    PM_V3DRSTN);

	case BCM2835_POWER_DOMAIN_IMAGE:
		return bcm2835_pm_power_on(pd, PM_IMAGE);

	case BCM2835_POWER_DOMAIN_IMAGE_PERI:
		return bcm2835_asb_power_on(pd, PM_IMAGE,
					    0, 0,
					    PM_PERIRSTN);

	case BCM2835_POWER_DOMAIN_IMAGE_ISP:
		return bcm2835_asb_power_on(pd, PM_IMAGE,
					    ASB_ISP_M_CTRL, ASB_ISP_S_CTRL,
					    PM_ISPRSTN);

	case BCM2835_POWER_DOMAIN_IMAGE_H264:
		return bcm2835_asb_power_on(pd, PM_IMAGE,
					    ASB_H264_M_CTRL, ASB_H264_S_CTRL,
					    PM_H264RSTN);

	case BCM2835_POWER_DOMAIN_USB:
		PM_WRITE(PM_USB, PM_USB_CTRLEN);
		return 0;

	case BCM2835_POWER_DOMAIN_DSI0:
		PM_WRITE(PM_DSI0, PM_DSI0_CTRLEN);
		PM_WRITE(PM_DSI0, PM_DSI0_CTRLEN | PM_DSI0_LDOHPEN);
		return 0;

	case BCM2835_POWER_DOMAIN_DSI1:
		PM_WRITE(PM_DSI1, PM_DSI1_CTRLEN);
		PM_WRITE(PM_DSI1, PM_DSI1_CTRLEN | PM_DSI1_LDOHPEN);
		return 0;

	case BCM2835_POWER_DOMAIN_CCP2TX:
		PM_WRITE(PM_CCP2TX, PM_CCP2TX_CTRLEN);
		PM_WRITE(PM_CCP2TX, PM_CCP2TX_CTRLEN | PM_CCP2TX_LDOEN);
		return 0;

	case BCM2835_POWER_DOMAIN_HDMI:
		PM_WRITE(PM_HDMI, PM_READ(PM_HDMI) | PM_HDMI_RSTDR);
		PM_WRITE(PM_HDMI, PM_READ(PM_HDMI) | PM_HDMI_CTRLEN);
		PM_WRITE(PM_HDMI, PM_READ(PM_HDMI) & ~PM_HDMI_LDOPD);
		usleep_range(100, 200);
		PM_WRITE(PM_HDMI, PM_READ(PM_HDMI) & ~PM_HDMI_RSTDR);
		return 0;

	default:
		dev_err(pm->dev, "Invalid domain %d\n", pd->domain);
		return -EINVAL;
	}
}

static int bcm2835_pm_pd_power_off(struct generic_pm_domain *domain)
{
	struct bcm2835_power_domain *pd =
		container_of(domain, struct bcm2835_power_domain, base);
	struct bcm2835_pm *pm = pd->pm;

	switch (pd->domain) {
	case BCM2835_POWER_DOMAIN_GRAFX:
		return bcm2835_pm_power_off(pd, PM_GRAFX);

	case BCM2835_POWER_DOMAIN_GRAFX_V3D:
		return bcm2835_asb_power_off(pd, PM_GRAFX,
					     ASB_V3D_M_CTRL, ASB_V3D_S_CTRL,
					     PM_V3DRSTN);

	case BCM2835_POWER_DOMAIN_IMAGE:
		return bcm2835_pm_power_off(pd, PM_IMAGE);

	case BCM2835_POWER_DOMAIN_IMAGE_PERI:
		return bcm2835_asb_power_off(pd, PM_IMAGE,
					     0, 0,
					     PM_PERIRSTN);

	case BCM2835_POWER_DOMAIN_IMAGE_ISP:
		return bcm2835_asb_power_off(pd, PM_IMAGE,
					     ASB_ISP_M_CTRL, ASB_ISP_S_CTRL,
					     PM_ISPRSTN);

	case BCM2835_POWER_DOMAIN_IMAGE_H264:
		return bcm2835_asb_power_off(pd, PM_IMAGE,
					     ASB_H264_M_CTRL, ASB_H264_S_CTRL,
					     PM_H264RSTN);

	case BCM2835_POWER_DOMAIN_USB:
		PM_WRITE(PM_USB, 0);
		return 0;

	case BCM2835_POWER_DOMAIN_DSI0:
		PM_WRITE(PM_DSI0, PM_DSI0_CTRLEN);
		PM_WRITE(PM_DSI0, 0);
		return 0;

	case BCM2835_POWER_DOMAIN_DSI1:
		PM_WRITE(PM_DSI1, PM_DSI1_CTRLEN);
		PM_WRITE(PM_DSI1, 0);
		return 0;

	case BCM2835_POWER_DOMAIN_CCP2TX:
		PM_WRITE(PM_CCP2TX, PM_CCP2TX_CTRLEN);
		PM_WRITE(PM_CCP2TX, 0);
		return 0;

	case BCM2835_POWER_DOMAIN_HDMI:
		PM_WRITE(PM_HDMI, PM_READ(PM_HDMI) | PM_HDMI_LDOPD);
		PM_WRITE(PM_HDMI, PM_READ(PM_HDMI) & ~PM_HDMI_CTRLEN);
		return 0;

	default:
		dev_err(pm->dev, "Invalid domain %d\n", pd->domain);
		return -EINVAL;
	}
}

static void
bcm2835_init_power_domain(struct bcm2835_pm *pm,
			  int pd_xlate_index, const char *name)
{
	struct device *dev = pm->dev;
	struct bcm2835_power_domain *dom = &pm->domains[pd_xlate_index];

	dom->clk = devm_clk_get(dev, name);

	dom->base.name = name;
	dom->base.power_on = bcm2835_pm_pd_power_on;
	dom->base.power_off = bcm2835_pm_pd_power_off;

	dom->domain = pd_xlate_index;
	dom->pm = pm;

	/* XXX: on/off at boot? */
	pm_genpd_init(&dom->base, NULL, true);

	pm->pd_xlate.domains[pd_xlate_index] = &dom->base;
}

/** bcm2835_reset_reset - Resets a block that has a reset line in the
 * PM block.
 *
 * The consumer of the reset controller must have the power domain up
 * -- there's no reset ability with the power domain down.  To reset
 * the sub-block, we just disable its access to memory through the
 * ASB, reset, and re-enable.
 */
static int bcm2835_reset_reset(struct reset_controller_dev *rcdev,
			       unsigned long id)
{
	struct bcm2835_pm *pm = container_of(rcdev, struct bcm2835_pm, reset);
	struct bcm2835_power_domain *pd;
	int ret;

	switch (id) {
	case BCM2835_RESET_V3D:
		pd = &pm->domains[BCM2835_POWER_DOMAIN_GRAFX_V3D];
		break;
	case BCM2835_RESET_H264:
		pd = &pm->domains[BCM2835_POWER_DOMAIN_IMAGE_H264];
		break;
	case BCM2835_RESET_ISP:
		pd = &pm->domains[BCM2835_POWER_DOMAIN_IMAGE_ISP];
		break;
	default:
		dev_err(pm->dev, "Bad reset id %ld\n", id);
		return -EINVAL;
	}

	ret = bcm2835_pm_pd_power_off(&pd->base);
	if (ret)
		return ret;

	return bcm2835_pm_pd_power_on(&pd->base);
}

static int bcm2835_reset_status(struct reset_controller_dev *rcdev,
				unsigned long id)
{
	struct bcm2835_pm *pm = container_of(rcdev, struct bcm2835_pm, reset);

	switch (id) {
	case BCM2835_RESET_V3D:
		return !PM_READ(PM_GRAFX & PM_V3DRSTN);
	case BCM2835_RESET_H264:
		return !PM_READ(PM_IMAGE & PM_H264RSTN);
	case BCM2835_RESET_ISP:
		return !PM_READ(PM_IMAGE & PM_ISPRSTN);
	default:
		return -EINVAL;
	}
}

const struct reset_control_ops bcm2835_reset_ops = {
	.reset = bcm2835_reset_reset,
	.status = bcm2835_reset_status,
};

static int bcm2835_init_power_domains(struct bcm2835_pm *pm)
{
	struct device *dev = pm->dev;
	static const struct {
		int parent, child;
	} domain_deps[] = {
		{ BCM2835_POWER_DOMAIN_GRAFX, BCM2835_POWER_DOMAIN_GRAFX_V3D },
		{ BCM2835_POWER_DOMAIN_IMAGE, BCM2835_POWER_DOMAIN_IMAGE_PERI },
		{ BCM2835_POWER_DOMAIN_IMAGE, BCM2835_POWER_DOMAIN_IMAGE_H264 },
		{ BCM2835_POWER_DOMAIN_IMAGE, BCM2835_POWER_DOMAIN_IMAGE_ISP },
		{ BCM2835_POWER_DOMAIN_IMAGE_PERI, BCM2835_POWER_DOMAIN_USB },
		{ BCM2835_POWER_DOMAIN_IMAGE_PERI, BCM2835_POWER_DOMAIN_CAM0 },
		{ BCM2835_POWER_DOMAIN_IMAGE_PERI, BCM2835_POWER_DOMAIN_CAM1 },
	};
	int ret, i;

	pm->pd_xlate.domains = devm_kcalloc(dev,
					    BCM2835_POWER_DOMAIN_COUNT,
					    sizeof(*pm->pd_xlate.domains),
					    GFP_KERNEL);
	if (!pm->pd_xlate.domains)
		return -ENOMEM;

	pm->pd_xlate.num_domains = BCM2835_POWER_DOMAIN_COUNT;

	bcm2835_init_power_domain(pm, BCM2835_POWER_DOMAIN_GRAFX, "grafx");
	bcm2835_init_power_domain(pm, BCM2835_POWER_DOMAIN_GRAFX_V3D, "v3d");

	bcm2835_init_power_domain(pm, BCM2835_POWER_DOMAIN_IMAGE, "image");
	bcm2835_init_power_domain(pm, BCM2835_POWER_DOMAIN_IMAGE_PERI, "peri_image");
	bcm2835_init_power_domain(pm, BCM2835_POWER_DOMAIN_IMAGE_H264, "h264");
	bcm2835_init_power_domain(pm, BCM2835_POWER_DOMAIN_IMAGE_ISP, "isp");

	bcm2835_init_power_domain(pm, BCM2835_POWER_DOMAIN_USB, "usb");
	bcm2835_init_power_domain(pm, BCM2835_POWER_DOMAIN_DSI0, "dsi0");
	bcm2835_init_power_domain(pm, BCM2835_POWER_DOMAIN_DSI1, "dsi1");
	bcm2835_init_power_domain(pm, BCM2835_POWER_DOMAIN_CAM0, "cam0");
	bcm2835_init_power_domain(pm, BCM2835_POWER_DOMAIN_CAM1, "cam1");
	bcm2835_init_power_domain(pm, BCM2835_POWER_DOMAIN_CCP2TX, "ccp2tx");
	bcm2835_init_power_domain(pm, BCM2835_POWER_DOMAIN_HDMI, "hdmi");

	for (i = 0; i < ARRAY_SIZE(domain_deps); i++) {
		pm_genpd_add_subdomain(&pm->domains[domain_deps[i].parent].base,
				       &pm->domains[domain_deps[i].child].base);
	}

	pm->reset.owner = THIS_MODULE;
	pm->reset.nr_resets = BCM2835_RESET_COUNT;
	pm->reset.ops = &bcm2835_reset_ops;
	pm->reset.of_node = dev->of_node;

	ret = devm_reset_controller_register(dev, &pm->reset);
	if (ret)
		return ret;

	of_genpd_add_provider_onecell(dev->of_node, &pm->pd_xlate);

	return 0;
}

/*
 * We can't really power off, but if we do the normal reset scheme, and
 * indicate to bootcode.bin not to reboot, then most of the chip will be
 * powered off.
 */
static void bcm2835_power_off(void)
{
	struct device_node *np =
		of_find_compatible_node(NULL, NULL, "brcm,bcm2835-pm-wdt");
	struct platform_device *pdev = of_find_device_by_node(np);
	struct bcm2835_pm *pm = platform_get_drvdata(pdev);
	u32 val;

	/*
	 * We set the watchdog hard reset bit here to distinguish this reset
	 * from the normal (full) reset. bootcode.bin will not reboot after a
	 * hard reset.
	 */
	val = PM_READ(PM_RSTS);
	val |= PM_PASSWORD | PM_RSTS_RASPBERRYPI_HALT;
	writel(val, pm->base + PM_RSTS);

	/* Continue with normal reset mechanism */
	__bcm2835_restart(pm);
}

static int bcm2835_pm_probe(struct platform_device *pdev)
{
	struct resource *res;
	struct device *dev = &pdev->dev;
	struct bcm2835_pm *pm;
	int err;

	pm = devm_kzalloc(dev, sizeof(struct bcm2835_pm), GFP_KERNEL);
	if (!pm)
		return -ENOMEM;
	pm->dev = dev;
	platform_set_drvdata(pdev, pm);

	spin_lock_init(&pm->lock);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	pm->base = devm_ioremap_resource(dev, res);
	if (IS_ERR(pm->base))
		return PTR_ERR(pm->base);

	/* We'll use the presence of the AXI ASB regs in the
	 * bcm2835-pm binding as the key for whether we can reference
	 * the full PM register range and support power domains.
	 */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	pm->asb = devm_ioremap_resource(dev, res);
	if (IS_ERR(pm->asb))
		pm->asb = NULL;

#if defined(CONFIG_WATCHDOG_CORE)
	watchdog_set_drvdata(&bcm2835_wdt_wdd, pm);
	watchdog_init_timeout(&bcm2835_wdt_wdd, heartbeat, dev);
	watchdog_set_nowayout(&bcm2835_wdt_wdd, nowayout);
	bcm2835_wdt_wdd.parent = dev;
	if (bcm2835_wdt_is_running(pm)) {
		/*
		 * The currently active timeout value (set by the
		 * bootloader) may be different from the module
		 * heartbeat parameter or the value in device
		 * tree. But we just need to set WDOG_HW_RUNNING,
		 * because then the framework will "immediately" ping
		 * the device, updating the timeout.
		 */
		set_bit(WDOG_HW_RUNNING, &bcm2835_wdt_wdd.status);
	}

	watchdog_set_restart_priority(&bcm2835_wdt_wdd, 128);

	watchdog_stop_on_reboot(&bcm2835_wdt_wdd);
	err = devm_watchdog_register_device(dev, &bcm2835_wdt_wdd);
	if (err) {
		dev_err(dev, "Failed to register watchdog device");
		return err;
	}
#endif

	if (pm->asb) {
		u32 id = ASB_READ(ASB_AXI_BRDG_ID);

		if (id != 0x62726467 /* "BRDG" */) {
			dev_err(dev, "ASB register ID returned 0x%08x\n", id);
			return -ENODEV;
		}

		err = bcm2835_init_power_domains(pm);
		if (err)
			return err;
	}

	if (pm_power_off == NULL)
		pm_power_off = bcm2835_power_off;

	dev_info(dev, "Broadcom BCM2835 watchdog timer");
	return 0;
}

static int bcm2835_pm_remove(struct platform_device *pdev)
{
	if (pm_power_off == bcm2835_power_off)
		pm_power_off = NULL;

	return 0;
}

static const struct of_device_id bcm2835_pm_of_match[] = {
	{ .compatible = "brcm,bcm2835-pm-wdt", },
	{ .compatible = "brcm,bcm2835-pm", },
	{},
};
MODULE_DEVICE_TABLE(of, bcm2835_pm_of_match);

static struct platform_driver bcm2835_pm_driver = {
	.probe		= bcm2835_pm_probe,
	.remove		= bcm2835_pm_remove,
	.driver = {
		.name =		"bcm2835-pm",
		.of_match_table = bcm2835_pm_of_match,
	},
};
module_platform_driver(bcm2835_pm_driver);

module_param(heartbeat, uint, 0);
MODULE_PARM_DESC(heartbeat, "Initial watchdog heartbeat in seconds");

module_param(nowayout, bool, 0);
MODULE_PARM_DESC(nowayout, "Watchdog cannot be stopped once started (default="
				__MODULE_STRING(WATCHDOG_NOWAYOUT) ")");

MODULE_AUTHOR("Lubomir Rintel <lkundrak@v3.sk>");
MODULE_DESCRIPTION("Driver for Broadcom BCM2835 PM/WDT");
MODULE_LICENSE("GPL");
