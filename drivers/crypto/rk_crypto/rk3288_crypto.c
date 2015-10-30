/*
 *Crypto acceleration support for Rockchip RK3288
 *
 * Copyright (c) 2015, Fuzhou Rockchip Electronics Co., Ltd
 *
 * Author: Zain Wang <zain.wang@rock-chips.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * Some ideas are from marvell/cesa.c and s5p-sss.c driver.
 */

#include "rk3288_crypto.h"
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/clk.h>
#include <linux/crypto.h>

struct crypto_info_t *crypto_p;

static int rk_crypto_enable_clk(struct crypto_info_t *dev)
{
	if (clk_prepare_enable(dev->clk)) {
		dev_err(dev->dev, "[%s:%d], Couldn't enable clock 'clk'\n",
						__func__, __LINE__);
		return -ENOENT;
	}
	if (clk_prepare_enable(dev->aclk)) {
		dev_err(dev->dev, "[%s:%d], Couldn't enable clock 'aclk'\n",
						__func__, __LINE__);
		goto err_aclk;
	}
	if (clk_prepare_enable(dev->hclk)) {
		dev_err(dev->dev, "[%s:%d], Couldn't enable clock 'hclk'\n",
						__func__, __LINE__);
		goto err_hclk;
	}
	if (clk_prepare_enable(dev->pclk)) {
		dev_err(dev->dev, "[%s:%d], Couldn't enable clock 'pclk'\n",
						__func__, __LINE__);
		goto err_pclk;
	}
	return 0;

err_pclk:
	clk_disable_unprepare(dev->hclk);
err_hclk:
	clk_disable_unprepare(dev->aclk);
err_aclk:
	clk_disable_unprepare(dev->clk);

	return -ENOENT;
}

static void rk_crypto_disable_clk(struct crypto_info_t *dev)
{
	clk_disable_unprepare(dev->hclk);
	clk_disable_unprepare(dev->aclk);
	clk_disable_unprepare(dev->pclk);
	clk_disable_unprepare(dev->clk);
}

static int check_alignment(struct scatterlist *sg_src,
			   struct scatterlist *sg_dst,
			   int align_mask)
{
	int in, out, align;

	in = IS_ALIGNED((u32)sg_src->offset, 4) &&
	     IS_ALIGNED(sg_src->length, align_mask);
	if (sg_dst == NULL)
		return in;
	out = IS_ALIGNED((u32)sg_dst->offset, 4) &&
	      IS_ALIGNED(sg_dst->length, align_mask);
	align = in && out;

	return (align && (sg_src->length == sg_dst->length));
}

static int rk_load_data(struct crypto_info_t *dev,
			      struct scatterlist *sg_src,
			      struct scatterlist *sg_dst)
{
	uint32_t count;
	int ret;

	dev->aligned = dev->aligned ?
		check_alignment(sg_src, sg_dst, dev->align_size) :
		dev->aligned;
	if (dev->aligned) {
		count = min(dev->left_bytes, sg_src->length);
		dev->left_bytes -= count;

		ret = dma_map_sg(dev->dev, sg_src, 1, DMA_TO_DEVICE);
		if (!ret) {
			dev_err(dev->dev, "[%s:%d] dma_map_sg(src)  error\n",
							__func__, __LINE__);
			return -EINVAL;
		}
		dev->addr_in = sg_dma_address(sg_src);

		if (sg_dst != NULL) {
			ret = dma_map_sg(dev->dev, sg_dst, 1, DMA_FROM_DEVICE);
			if (!ret) {
				dev_err(dev->dev,
					"[%s:%d] dma_map_sg(dst)  error\n",
					__func__, __LINE__);
				dma_unmap_sg(dev->dev, sg_src, 1,
						DMA_TO_DEVICE);
				return -EINVAL;
			}
			dev->addr_out = sg_dma_address(sg_dst);
		}
	} else {
		count = (dev->left_bytes > PAGE_SIZE) ?
			PAGE_SIZE : dev->left_bytes;

		ret = sg_pcopy_to_buffer(dev->first, dev->nents,
					 dev->addr_vir, count,
					 dev->total - dev->left_bytes);
		if (!ret) {
			dev_err(dev->dev, "[%s:%d] pcopy err\n",
						__func__, __LINE__);
			return -EINVAL;
		}
		dev->left_bytes -= count;
		sg_init_one(&dev->sg_tmp, dev->addr_vir, count);
		ret = dma_map_sg(dev->dev, &dev->sg_tmp, 1, DMA_TO_DEVICE);
		if (!ret) {
			dev_err(dev->dev, "[%s:%d] dma_map_sg(sg_tmp)  error\n",
							__func__, __LINE__);
			return -ENOMEM;
		}
		dev->addr_in = sg_dma_address(&dev->sg_tmp);

		if (sg_dst != NULL) {
			ret = dma_map_sg(dev->dev, &dev->sg_tmp, 1,
					 DMA_FROM_DEVICE);
			if (!ret) {
				dev_err(dev->dev,
					"[%s:%d] dma_map_sg(sg_tmp)  error\n",
					__func__, __LINE__);
				dma_unmap_sg(dev->dev, &dev->sg_tmp, 1,
					     DMA_TO_DEVICE);
				return -ENOMEM;
			}
			dev->addr_out = sg_dma_address(&dev->sg_tmp);
		}
	}
	dev->count = count;
	return 0;
}

static void rk_unload_data(struct crypto_info_t *dev)
{
	struct scatterlist *sg_in, *sg_out;

	sg_in = dev->aligned ? dev->sg_src : &dev->sg_tmp;
	dma_unmap_sg(dev->dev, sg_in, 1, DMA_TO_DEVICE);

	if (dev->sg_dst != NULL) {
		sg_out = dev->aligned ? dev->sg_dst : &dev->sg_tmp;
		dma_unmap_sg(dev->dev, sg_out, 1, DMA_FROM_DEVICE);
	}
}

static irqreturn_t crypto_irq_handle(int irq, void *dev_id)
{
	struct crypto_info_t *dev  = platform_get_drvdata(dev_id);
	uint32_t interrupt_status;
	int err = 0;

	spin_lock(&dev->lock);

	if (irq == dev->irq) {
		interrupt_status = CRYPTO_READ(dev, RK_CRYPTO_INTSTS);
		CRYPTO_WRITE(dev, RK_CRYPTO_INTSTS, interrupt_status);
		if (interrupt_status & 0x0a) {
			dev_warn(dev->dev, "DMA Error\n");
			err = -EFAULT;
		} else if (interrupt_status & 0x05)
			err = dev->update(dev);

		if (err)
			dev->complete(dev, err);
	}
	spin_unlock(&dev->lock);
	return IRQ_HANDLED;
}

static void rk_crypto_tasklet_cb(unsigned long data)
{
	struct crypto_info_t *dev = (struct crypto_info_t *)data;
	struct crypto_async_request *async_req, *backlog;
	struct rk_ahash_reqctx *hash_reqctx;
	struct rk_cipher_reqctx *ablk_reqctx;
	int err = 0;

	spin_lock(&dev->lock);
	backlog   = crypto_get_backlog(&dev->queue);
	async_req = crypto_dequeue_request(&dev->queue);
	spin_unlock(&dev->lock);
	if (!async_req) {
		dev_err(dev->dev, "async_req is NULL !!\n");
		return;
	}
	if (backlog) {
		backlog->complete(backlog, -EINPROGRESS);
		backlog = NULL;
	}

	if (crypto_tfm_alg_type(async_req->tfm) == CRYPTO_ALG_TYPE_AHASH) {
		dev->ahash_req = ahash_request_cast(async_req);
		hash_reqctx = ahash_request_ctx(dev->ahash_req);
	} else {
		dev->ablk_req = ablkcipher_request_cast(async_req);
		ablk_reqctx   = ablkcipher_request_ctx(dev->ablk_req);
	}
	err = dev->start(dev);
	if (err)
		dev->complete(dev, err);
}

static struct crypto_alg *rk_cipher_algs[] = {
	&rk_ecb_aes_alg,
	&rk_cbc_aes_alg,
	&rk_ecb_des_alg,
	&rk_cbc_des_alg,
	&rk_ecb_des3_ede_alg,
	&rk_cbc_des3_ede_alg,
};

static int rk_crypto_register(void)
{
	int i, k;
	int err = 0;

	for (i = 0; i < ARRAY_SIZE(rk_cipher_algs); i++) {
		if (crypto_register_alg(rk_cipher_algs[i]))
			goto err_cipher_algs;
	}
	return err;

err_cipher_algs:
	for (k = 0; k < i; k++)
		crypto_unregister_alg(rk_cipher_algs[k]);
	return err;
}

static void rk_crypto_unregister(void)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(rk_cipher_algs); i++)
		crypto_unregister_alg(rk_cipher_algs[i]);
}

static int rk_crypto_probe(struct platform_device *pdev)
{
	int err = 0;
	struct resource *res;
	struct device *dev = &pdev->dev;
	struct crypto_info_t *crypto_info;

	crypto_info = devm_kzalloc(&pdev->dev,
			sizeof(*crypto_info), GFP_KERNEL);
	if (!crypto_info)
		return -ENOMEM;

	spin_lock_init(&crypto_info->lock);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	crypto_info->reg = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(crypto_info->reg)) {
		dev_warn(crypto_info->dev, "Error on remap reg\n");
		err = PTR_ERR(crypto_info->reg);
		goto err_ioremap;
	}

	crypto_info->aclk = clk_get(&pdev->dev, "aclk_crypto");
	if (IS_ERR(crypto_info->aclk)) {
		dev_err(dev, "failed to find crypto clock source\n");
		err = -ENOENT;
		goto err_ioremap;
	}

	crypto_info->hclk = clk_get(&pdev->dev, "hclk_crypto");
	if (IS_ERR(crypto_info->hclk)) {
		dev_err(dev, "failed to find crypto clock source\n");
		err = -ENOENT;
		goto err_ioremap;
	}

	crypto_info->clk = clk_get(&pdev->dev, "srst_crypto");
	if (IS_ERR(crypto_info->clk)) {
		dev_err(dev, "failed to find crypto clock source\n");
		err = -ENOENT;
		goto err_ioremap;
	}

	crypto_info->pclk = clk_get(&pdev->dev, "apb_pclk");
	if (IS_ERR(crypto_info->pclk)) {
		dev_err(dev, "failed to find crypto clock source\n");
		err = -ENOENT;
		goto err_ioremap;
	}

	crypto_info->irq = platform_get_irq(pdev, 0);
	if (crypto_info->irq < 0) {
		dev_warn(crypto_info->dev,
				"control Interrupt is not available.\n");
		err = crypto_info->irq;
		goto err_ioremap;
	}

	err = request_irq(crypto_info->irq, crypto_irq_handle, IRQF_SHARED,
			  "rk-crypto", pdev);
	if (err) {
		dev_warn(crypto_info->dev, "irq request failed.\n");
		goto err_ioremap;
	}

	crypto_info->dev = &pdev->dev;
	platform_set_drvdata(pdev, crypto_info);
	crypto_p = crypto_info;

	tasklet_init(&crypto_info->crypto_tasklet,
			rk_crypto_tasklet_cb, (unsigned long)crypto_info);
	crypto_init_queue(&crypto_info->queue, 50);

	crypto_info->enable_clk = rk_crypto_enable_clk;
	crypto_info->disable_clk = rk_crypto_disable_clk;
	crypto_info->load_data = rk_load_data;
	crypto_info->unload_data = rk_unload_data;

	err = rk_crypto_register();
	if (err) {
		pr_info("err in register alg");
		goto err_reg_alg;
	}

	return 0;

err_reg_alg:
	free_irq(crypto_info->irq, crypto_info);
err_ioremap:
	kfree(crypto_info);
	crypto_p = NULL;

	return err;
}

static int rk_crypto_remove(struct platform_device *pdev)
{
	struct crypto_info_t *crypto_tmp = platform_get_drvdata(pdev);

	rk_crypto_unregister();
	tasklet_kill(&crypto_tmp->crypto_tasklet);
	free_irq(crypto_tmp->irq, crypto_tmp);
	kfree(crypto_tmp);
	crypto_p = NULL;

	return 0;
}
#ifdef CONFIG_OF
static const struct of_device_id crypto_of_id_table[] = {
	{ .compatible = "rockchip,crypto" },
	{}
};
#endif /* CONFIG_OF */

static const struct platform_device_id crypto_id_table[] = {
	{ "rockchip,crypto" },
	{}
};

static struct platform_driver crypto_driver = {
	.probe		= rk_crypto_probe,
	.remove		= rk_crypto_remove,
	.driver		= {
		.name	= "rockchip,crypto",
		.of_match_table	= of_match_ptr(crypto_of_id_table),
	},
	.id_table	= crypto_id_table,
};

module_platform_driver(crypto_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Zain Wang");
