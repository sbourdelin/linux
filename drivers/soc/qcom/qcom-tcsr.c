/*
 * This abstracts the TCSR register area in Qualcomm SoCs, originally
 * introduced by Tim Bird as part of the phy-msm-usb.ko device driver,
 * and split out by Arnd Bergmann into a separate file.
 *
 * This file shouldn't really exist, since we have no way to detect
 * if the TCSR actually exists in the hardcoded location, or if it
 * is compatible with the version that was originally used.
 *
 * If the assumptions ever change, we have to come up with a better
 * solution.
 */
#include <linux/module.h>
#include <linux/io.h>

/* USB phy selector - in TCSR address range */
#define USB2_PHY_SEL         0xfd4ab000

/*
 * qcom_tcsr_phy_sel -- Select secondary PHY via TCSR
 *
 * Select the secondary PHY using the TCSR register, if phy-num=1
 * in the DTS (or phy_number is set in the platform data).  The
 * SOC has 2 PHYs which can be used with the OTG port, and this
 * code allows configuring the correct one.
 *
 * Note: This resolves the problem I was seeing where I couldn't
 * get the USB driver working at all on a dragonboard, from cold
 * boot.  This patch depends on patch 5/14 from Ivan's msm USB
 * patch set.  It does not use DT for the register address, as
 * there's no evidence that this address changes between SoC
 * versions.
 *		- Tim
 */
int qcom_tcsr_phy_sel(u32 val)
{
	void __iomem *phy_select;
	int ret;

	phy_select = ioremap(USB2_PHY_SEL, 4);

	if (!phy_select) {
		ret = -ENOMEM;
		goto out;
	}
	/* Enable second PHY with the OTG port */
	writel(0x1, phy_select);
	ret = 0;
out:
	iounmap(phy_select);
	return ret;
}
EXPORT_SYMBOL_GPL(qcom_tcsr_phy_sel);

MODULE_AUTHOR("Tim Bird <tbird20d@gmail.com>");
MODULE_DESCRIPTION("Qualcomm TCSR abstraction");
MODULE_LICENSE("GPL v2");
