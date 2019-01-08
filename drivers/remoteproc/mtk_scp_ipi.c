// SPDX-License-Identifier: GPL-2.0
//
// Copyright (c) 2018 MediaTek Inc.

#include <linux/clk.h>
#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_data/mtk_scp.h>
#include <linux/platform_device.h>

#include "mtk_common.h"

int scp_ipi_register(struct platform_device *pdev,
		     enum scp_ipi_id id,
		     scp_ipi_handler_t handler,
		     const char *name,
		     void *priv)
{
	struct mtk_scp *scp = platform_get_drvdata(pdev);
	struct scp_ipi_desc *ipi_desc;

	if (!scp) {
		dev_err(&pdev->dev, "scp device is not ready\n");
		return -EPROBE_DEFER;
	}

	if (WARN(id < 0 ||  id >= SCP_IPI_MAX || handler == NULL,
	    "register scp ipi id %d with invalid arguments\n", id))
		return -EINVAL;

	ipi_desc = scp->ipi_desc;
	ipi_desc[id].name = name;
	ipi_desc[id].handler = handler;
	ipi_desc[id].priv = priv;

	return 0;
}
EXPORT_SYMBOL_GPL(scp_ipi_register);

int scp_ipi_send(struct platform_device *pdev,
		 enum scp_ipi_id id,
		 void *buf,
		 unsigned int len,
		 unsigned int wait)
{
	struct mtk_scp *scp = platform_get_drvdata(pdev);
	struct share_obj *send_obj = scp->send_buf;
	unsigned long timeout;
	int ret;

	if (WARN(id <= SCP_IPI_INIT || id >= SCP_IPI_MAX ||
	    len > sizeof(send_obj->share_buf) || !buf,
	    "failed to send ipi message\n"))
		return -EINVAL;

	ret = clk_prepare_enable(scp->clk);
	if (ret) {
		dev_err(scp->dev, "failed to enable clock\n");
		return ret;
	}

	mutex_lock(&scp->scp_mutex);

	 /* Wait until SCP receives the last command */
	timeout = jiffies + msecs_to_jiffies(2000);
	do {
		if (time_after(jiffies, timeout)) {
			dev_err(scp->dev, "scp_ipi_send: IPI timeout!\n");
			ret = -EIO;
			mutex_unlock(&scp->scp_mutex);
			goto clock_disable;
		}
	} while (readl(scp->reg_base + MT8183_HOST_TO_SCP));

	memcpy(send_obj->share_buf, buf, len);
	send_obj->len = len;
	send_obj->id = id;

	scp->ipi_id_ack[id] = false;
	/* send the command to SCP */
	writel(MT8183_HOST_IPC_INT_BIT, scp->reg_base + MT8183_HOST_TO_SCP);

	mutex_unlock(&scp->scp_mutex);

	if (wait) {
		/* wait for SCP's ACK */
		timeout = msecs_to_jiffies(wait);
		ret = wait_event_timeout(scp->ack_wq,
					 scp->ipi_id_ack[id],
					 timeout);
		scp->ipi_id_ack[id] = false;
		if (WARN(!ret,
			 "scp ipi %d ack time out !", id))
			ret = -EIO;
		else
			ret = 0;
	}

clock_disable:
	clk_disable_unprepare(scp->clk);

	return ret;
}
EXPORT_SYMBOL_GPL(scp_ipi_send);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("MediaTek scp IPI interface");
