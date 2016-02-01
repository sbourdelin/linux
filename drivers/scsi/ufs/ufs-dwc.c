/* ==========================================================================
 * The Synopsys DWC UFS Software Driver and documentation (hereinafter
 * "Software") is an unsupported proprietary work of Synopsys, Inc. unless
 * otherwise expressly agreed to in writing between Synopsys and you.
 *
 * The Software IS NOT an item of Licensed Software or Licensed Product under
 * any End User Software License Agreement or Agreement for Licensed Product
 * with Synopsys or any supplement thereto.  Permission is hereby granted,
 * free of charge, to any person obtaining a copy of this software annotated
 * with this license and the Software, to deal in the Software without
 * restriction, including without limitation the rights to use, copy, modify,
 * merge, publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so, subject
 * to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THIS SOFTWARE IS BEING DISTRIBUTED BY SYNOPSYS SOLELY ON AN "AS IS" BASIS
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE HEREBY DISCLAIMED. IN NO EVENT SHALL SYNOPSYS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 * ==========================================================================
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/delay.h>

#include "ufshcd.h"
#include "ufshcd-pltfrm.h"

/**
 * struct ufs_hba_dwc_vops - UFS DWC specific variant operations
 *
 */
static struct ufs_hba_variant_ops ufs_hba_dwc_vops = {
	.name                   = "dwc",
};

/**
 * ufs_dwc_probe()
 * @pdev: pointer to platform device structure
 *
 */
static int ufs_dwc_probe(struct platform_device *pdev)
{
	int err;
	struct device *dev = &pdev->dev;

	/* Perform generic probe */
	err = ufshcd_pltfrm_init(pdev, &ufs_hba_dwc_vops);
	if (err)
		dev_err(dev, "ufshcd_pltfrm_init() failed %d\n", err);

	return err;
}

/**
 * ufs_dwc_remove()
 * @pdev: pointer to platform device structure
 *
 */
static int ufs_dwc_remove(struct platform_device *pdev)
{
	struct ufs_hba *hba =  platform_get_drvdata(pdev);

	pm_runtime_get_sync(&(pdev)->dev);
	ufshcd_remove(hba);

	return 0;
}

static const struct of_device_id ufs_dwc_match[] = {
	{
		.compatible = "snps,ufshcd"
	},
	{ },
};
MODULE_DEVICE_TABLE(of, ufs_dwc_match);

static const struct dev_pm_ops ufs_dwc_pm_ops = {
	.suspend	= ufshcd_pltfrm_suspend,
	.resume		= ufshcd_pltfrm_resume,
	.runtime_suspend = ufshcd_pltfrm_runtime_suspend,
	.runtime_resume  = ufshcd_pltfrm_runtime_resume,
	.runtime_idle    = ufshcd_pltfrm_runtime_idle,
};

static struct platform_driver ufs_dwc_driver = {
	.probe		= ufs_dwc_probe,
	.remove		= ufs_dwc_remove,
	.shutdown = ufshcd_pltfrm_shutdown,
	.driver		= {
		.name	= "ufshcd-dwc",
		.pm	= &ufs_dwc_pm_ops,
		.of_match_table	= of_match_ptr(ufs_dwc_match),
	},
};

module_platform_driver(ufs_dwc_driver);

MODULE_ALIAS("platform:ufshcd-dwc");
MODULE_DESCRIPTION("DesignWare UFS Host platform glue driver");
MODULE_AUTHOR("Joao Pinto <Joao.Pinto@synopsys.com>");
MODULE_LICENSE("Dual BSD/GPL");
