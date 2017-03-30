/*
 * Intel FPGA Feature Device Driver
 *
 * Copyright (C) 2017 Intel Corporation, Inc.
 *
 * Authors:
 *   Kang Luwei <luwei.kang@intel.com>
 *   Zhang Yi <yi.z.zhang@intel.com>
 *   Wu Hao <hao.wu@intel.com>
 *   Xiao Guangrong <guangrong.xiao@linux.intel.com>
 *
 * This work is licensed under a dual BSD/GPLv2 license. When using or
 * redistributing this file, you may do so under either license. See the
 * LICENSE.BSD file under this directory for the BSD license and see
 * the COPYING file in the top-level directory for the GPLv2 license.
 */

#include "feature-dev.h"

void feature_platform_data_add(struct feature_platform_data *pdata,
			       int index, const char *name,
			       int resource_index, void __iomem *ioaddr)
{
	WARN_ON(index >= pdata->num);

	pdata->features[index].name = name;
	pdata->features[index].resource_index = resource_index;
	pdata->features[index].ioaddr = ioaddr;
}

int feature_platform_data_size(int num)
{
	return sizeof(struct feature_platform_data) +
		num * sizeof(struct feature);
}

struct feature_platform_data *
feature_platform_data_alloc_and_init(struct platform_device *dev, int num)
{
	struct feature_platform_data *pdata;

	pdata = kzalloc(feature_platform_data_size(num), GFP_KERNEL);
	if (pdata) {
		pdata->dev = dev;
		pdata->num = num;
		mutex_init(&pdata->lock);
	}

	return pdata;
}

int fme_feature_num(void)
{
	return FME_FEATURE_ID_MAX;
}

int port_feature_num(void)
{
	return PORT_FEATURE_ID_MAX;
}

int fpga_port_id(struct platform_device *pdev)
{
	struct feature_port_header *port_hdr;
	struct feature_port_capability capability;

	port_hdr = get_feature_ioaddr_by_index(&pdev->dev,
					       PORT_FEATURE_ID_HEADER);
	WARN_ON(!port_hdr);

	capability.csr = readq(&port_hdr->capability);
	return capability.port_number;
}
EXPORT_SYMBOL_GPL(fpga_port_id);

/*
 * Enable Port by clear the port soft reset bit, which is set by default.
 * The User AFU is unable to respond to any MMIO access while in reset.
 * __fpga_port_enable function should only be used after __fpga_port_disable
 * function.
 */
void __fpga_port_enable(struct platform_device *pdev)
{
	struct feature_platform_data *pdata = dev_get_platdata(&pdev->dev);
	struct feature_port_header *port_hdr;
	struct feature_port_control control;

	WARN_ON(!pdata->disable_count);

	if (--pdata->disable_count != 0)
		return;

	port_hdr = get_feature_ioaddr_by_index(&pdev->dev,
					       PORT_FEATURE_ID_HEADER);
	WARN_ON(!port_hdr);

	control.csr = readq(&port_hdr->control);
	control.port_sftrst = 0x0;
	writeq(control.csr, &port_hdr->control);
}
EXPORT_SYMBOL_GPL(__fpga_port_enable);

#define RST_POLL_INVL 10 /* us */
#define RST_POLL_TIMEOUT 1000 /* us */

int __fpga_port_disable(struct platform_device *pdev)
{
	struct feature_platform_data *pdata = dev_get_platdata(&pdev->dev);
	struct feature_port_header *port_hdr;
	struct feature_port_control control;

	if (pdata->disable_count++ != 0)
		return 0;

	port_hdr = get_feature_ioaddr_by_index(&pdev->dev,
					       PORT_FEATURE_ID_HEADER);
	WARN_ON(!port_hdr);

	/* Set port soft reset */
	control.csr = readq(&port_hdr->control);
	control.port_sftrst = 0x1;
	writeq(control.csr, &port_hdr->control);

	/*
	 * HW sets ack bit to 1 when all outstanding requests have been drained
	 * on this port and minimum soft reset pulse width has elapsed.
	 * Driver polls port_soft_reset_ack to determine if reset done by HW.
	 */
	control.port_sftrst_ack = 1;

	if (fpga_wait_register_field(port_sftrst_ack, control,
		&port_hdr->control, RST_POLL_TIMEOUT, RST_POLL_INVL)) {
		dev_err(&pdev->dev, "timeout, fail to reset device\n");
		return -ETIMEDOUT;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(__fpga_port_disable);
