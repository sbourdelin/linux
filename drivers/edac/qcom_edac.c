// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2018, The Linux Foundation. All rights reserved.
 */

#include <linux/edac.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/smp.h>
#include <linux/soc/qcom/llcc-qcom.h>

#include "edac_mc.h"
#include "edac_device.h"

#ifdef CONFIG_EDAC_QCOM_LLCC_PANIC_ON_UE
#define LLCC_ERP_PANIC_ON_UE            1
#else
#define LLCC_ERP_PANIC_ON_UE            0
#endif

#define EDAC_LLCC                       "qcom_llcc"

#define TRP_SYN_REG_CNT                 6

#define DRP_SYN_REG_CNT                 8

#define LLCC_COMMON_STATUS0             0x0003000C
#define LLCC_LB_CNT_MASK                GENMASK(31, 28)
#define LLCC_LB_CNT_SHIFT               28

/* single & Double Bit syndrome register offsets */
#define TRP_ECC_SB_ERR_SYN0             0x0002304C
#define TRP_ECC_DB_ERR_SYN0             0x00020370
#define DRP_ECC_SB_ERR_SYN0             0x0004204C
#define DRP_ECC_DB_ERR_SYN0             0x00042070

/* Error register offsets */
#define TRP_ECC_ERROR_STATUS1           0x00020348
#define TRP_ECC_ERROR_STATUS0           0x00020344
#define DRP_ECC_ERROR_STATUS1           0x00042048
#define DRP_ECC_ERROR_STATUS0           0x00042044

/* TRP, DRP interrupt register offsets */
#define DRP_INTERRUPT_STATUS            0x00041000
#define TRP_INTERRUPT_0_STATUS          0x00020480
#define DRP_INTERRUPT_CLEAR             0x00041008
#define DRP_ECC_ERROR_CNTR_CLEAR        0x00040004
#define TRP_INTERRUPT_0_CLEAR           0x00020484
#define TRP_ECC_ERROR_CNTR_CLEAR        0x00020440

/* Mask and shift macros */
#define ECC_DB_ERR_COUNT_MASK           GENMASK(4, 0)
#define ECC_DB_ERR_WAYS_MASK            GENMASK(31, 16)
#define ECC_DB_ERR_WAYS_SHIFT           BIT(4)

#define ECC_SB_ERR_COUNT_MASK           GENMASK(23, 16)
#define ECC_SB_ERR_COUNT_SHIFT          BIT(4)
#define ECC_SB_ERR_WAYS_MASK            GENMASK(15, 0)

#define SB_ECC_ERROR                    BIT(0)
#define DB_ECC_ERROR                    BIT(1)

#define DRP_TRP_INT_CLEAR               GENMASK(1, 0)
#define DRP_TRP_CNT_CLEAR               GENMASK(1, 0)

/* Config registers offsets*/
#define DRP_ECC_ERROR_CFG               0x00040000

/* TRP, DRP interrupt register offsets */
#define CMN_INTERRUPT_0_ENABLE          0x0003001C
#define CMN_INTERRUPT_2_ENABLE          0x0003003C
#define TRP_INTERRUPT_0_ENABLE          0x00020488
#define DRP_INTERRUPT_ENABLE            0x0004100C

#define SB_ERROR_THRESHOLD              0x1
#define SB_ERROR_THRESHOLD_SHIFT        24
#define SB_DB_TRP_INTERRUPT_ENABLE      0x3
#define TRP0_INTERRUPT_ENABLE           0x1
#define DRP0_INTERRUPT_ENABLE           BIT(6)
#define SB_DB_DRP_INTERRUPT_ENABLE      0x3

enum {
	LLCC_DRAM_CE = 0,
	LLCC_DRAM_UE,
	LLCC_TRAM_CE,
	LLCC_TRAM_UE,
	LLCC_ERR_TYPE_MAX = LLCC_TRAM_UE + 1,
};

static int qcom_llcc_core_setup(struct regmap *llcc_bcast_regmap)
{
	u32 sb_err_threshold;
	int ret;

	/* Enable TRP in instance 2 of common interrupt enable register */
	ret = regmap_update_bits(llcc_bcast_regmap, CMN_INTERRUPT_2_ENABLE,
				 TRP0_INTERRUPT_ENABLE,
				 TRP0_INTERRUPT_ENABLE);
	if (ret)
		return ret;

	/* Enable ECC interrupts on Tag Ram */
	ret = regmap_update_bits(llcc_bcast_regmap, TRP_INTERRUPT_0_ENABLE,
				 SB_DB_TRP_INTERRUPT_ENABLE,
				 SB_DB_TRP_INTERRUPT_ENABLE);
	if (ret)
		return ret;

	/* Enable SB error for Data RAM */
	sb_err_threshold = (SB_ERROR_THRESHOLD << SB_ERROR_THRESHOLD_SHIFT);
	ret = regmap_write(llcc_bcast_regmap, DRP_ECC_ERROR_CFG,
			   sb_err_threshold);
	if (ret)
		return ret;

	/* Enable DRP in instance 2 of common interrupt enable register */
	ret = regmap_update_bits(llcc_bcast_regmap, CMN_INTERRUPT_2_ENABLE,
				 DRP0_INTERRUPT_ENABLE,
				 DRP0_INTERRUPT_ENABLE);
	if (ret)
		return ret;

	/* Enable ECC interrupts on Data Ram */
	ret = regmap_write(llcc_bcast_regmap, DRP_INTERRUPT_ENABLE,
			   SB_DB_DRP_INTERRUPT_ENABLE);
	return ret;
}

/* Clear the error interrupt and counter registers */
static int
qcom_llcc_clear_errors_status(int err_type, struct llcc_drv_data *drv)
{
	int ret = 0;

	switch (err_type) {
	case LLCC_DRAM_CE:
	case LLCC_DRAM_UE:
		/* Clear the interrupt */
		ret = regmap_write(drv->bcast_regmap, DRP_INTERRUPT_CLEAR,
				   DRP_TRP_INT_CLEAR);
		if (ret)
			return ret;

		/* Clear the counters */
		ret = regmap_write(drv->bcast_regmap, DRP_ECC_ERROR_CNTR_CLEAR,
				   DRP_TRP_CNT_CLEAR);
		if (ret)
			return ret;
		break;
	case LLCC_TRAM_CE:
	case LLCC_TRAM_UE:
		ret = regmap_write(drv->bcast_regmap, TRP_INTERRUPT_0_CLEAR,
				   DRP_TRP_INT_CLEAR);
		if (ret)
			return ret;

		ret = regmap_write(drv->bcast_regmap, TRP_ECC_ERROR_CNTR_CLEAR,
				   DRP_TRP_CNT_CLEAR);
		if (ret)
			return ret;
		break;
	}
	return ret;
}

/* Dump Syndrome registers data for Tag RAM, Data RAM bit errors*/
static int
dump_syn_reg_values(struct llcc_drv_data *drv, u32 bank, int err_type)
{
	struct llcc_edac_reg_data *reg_data = &(drv->edac_reg[err_type]);
	int err_cnt, err_ways, ret, i;
	u32 synd_reg, synd_val;

	for (i = 0; i < reg_data->reg_cnt; i++) {
		synd_reg = reg_data->synd_reg + (i * 4);
		ret = regmap_read(drv->regmap, drv->offsets[bank] + synd_reg,
				  &synd_val);
		if (ret)
			goto clear;
		edac_printk(KERN_CRIT, EDAC_LLCC, "%s: ECC_SYN%d: 0x%8x\n",
			    reg_data->err_name, i, synd_val);
	}

	ret = regmap_read(drv->regmap,
			  drv->offsets[bank] + reg_data->err_status_reg,
			  &err_cnt);
	if (ret)
		goto clear;

	err_cnt &= reg_data->err_count_mask;
	err_cnt >>= reg_data->err_count_shift;
	edac_printk(KERN_CRIT, EDAC_LLCC, "%s: error count: 0x%4x\n",
		    reg_data->err_name, err_cnt);

	ret = regmap_read(drv->regmap,
			  drv->offsets[bank] + reg_data->err_ways_status,
			  &err_ways);
	if (ret)
		goto clear;

	err_ways &= reg_data->err_ways_mask;
	err_ways >>= reg_data->err_ways_shift;

	edac_printk(KERN_CRIT, EDAC_LLCC, "%s: error ways: 0x%4x\n",
		    reg_data->err_name, err_ways);

clear:
	ret = qcom_llcc_clear_errors_status(err_type, drv);
	return ret;
}

static int
dump_syn_reg(struct edac_device_ctl_info *edev_ctl, int err_type, u32 bank)
{
	struct llcc_drv_data *drv = edev_ctl->pvt_info;
	int ret = 0;

	ret = dump_syn_reg_values(drv, bank, err_type);
	if (ret)
		return ret;

	switch (err_type) {
	case LLCC_DRAM_CE:
		edac_device_handle_ce(edev_ctl, 0, bank,
				      "LLCC Data RAM correctable Error");
		break;
	case LLCC_DRAM_UE:
		edac_device_handle_ue(edev_ctl, 0, bank,
				      "LLCC Data RAM uncorrectable Error");
		break;
	case LLCC_TRAM_CE:
		edac_device_handle_ce(edev_ctl, 0, bank,
				      "LLCC Tag RAM correctable Error");
		break;
	case LLCC_TRAM_UE:
		edac_device_handle_ue(edev_ctl, 0, bank,
				      "LLCC Tag RAM uncorrectable Error");
		break;
	}

	return ret;
}

static irqreturn_t
llcc_ecc_irq_handler(int irq, void *edev_ctl)
{
	struct edac_device_ctl_info *edac_dev_ctl;
	irqreturn_t irq_rc = IRQ_NONE;
	u32 drp_error, trp_error, i;
	struct llcc_drv_data *drv;
	int ret;

	edac_dev_ctl = (struct edac_device_ctl_info *)edev_ctl;
	drv = edac_dev_ctl->pvt_info;

	for (i = 0; i < drv->num_banks; i++) {
		/* Look for Data RAM errors */
		ret = regmap_read(drv->regmap,
				  drv->offsets[i] + DRP_INTERRUPT_STATUS,
				  &drp_error);
		if (ret)
			return irq_rc;

		if (drp_error & SB_ECC_ERROR) {
			edac_printk(KERN_CRIT, EDAC_LLCC,
				    "Single Bit Error detected in Data Ram\n");
			ret = dump_syn_reg(edev_ctl, LLCC_DRAM_CE, i);
			if (!ret)
				irq_rc = IRQ_HANDLED;
		} else if (drp_error & DB_ECC_ERROR) {
			edac_printk(KERN_CRIT, EDAC_LLCC,
				    "Double Bit Error detected in Data Ram\n");
			ret = dump_syn_reg(edev_ctl, LLCC_DRAM_UE, i);
			if (!ret)
				irq_rc = IRQ_HANDLED;
		}

		/* Look for Tag RAM errors */
		ret = regmap_read(drv->regmap,
				  drv->offsets[i] + TRP_INTERRUPT_0_STATUS,
				  &trp_error);
		if (ret)
			return irq_rc;

		if (trp_error & SB_ECC_ERROR) {
			edac_printk(KERN_CRIT, EDAC_LLCC,
				    "Single Bit Error detected in Tag Ram\n");
			ret = dump_syn_reg(edev_ctl, LLCC_TRAM_CE, i);
			if (!ret)
				irq_rc = IRQ_HANDLED;
		} else if (trp_error & DB_ECC_ERROR) {
			edac_printk(KERN_CRIT, EDAC_LLCC,
				    "Double Bit Error detected in Tag Ram\n");
			ret = dump_syn_reg(edev_ctl, LLCC_TRAM_UE, i);
			if (!ret)
				irq_rc = IRQ_HANDLED;
		}
	}

	return irq_rc;
}

static void llcc_edac_reg_data_init(struct llcc_edac_reg_data *edac_reg)
{

	struct llcc_edac_reg_data *reg_data;

	/* Initialize register info for LLCC_DRAM_CE */
	reg_data = &edac_reg[LLCC_DRAM_CE];
	reg_data->err_name = "DRAM Single-bit";
	reg_data->reg_cnt = DRP_SYN_REG_CNT;
	reg_data->synd_reg = DRP_ECC_SB_ERR_SYN0;
	reg_data->err_status_reg = DRP_ECC_ERROR_STATUS1;
	reg_data->err_count_mask = ECC_SB_ERR_COUNT_MASK;
	reg_data->err_count_shift = ECC_SB_ERR_COUNT_SHIFT;
	reg_data->err_ways_status = DRP_ECC_ERROR_STATUS0;
	reg_data->err_ways_mask = ECC_SB_ERR_WAYS_MASK;

	/* Initialize register info for LLCC_DRAM_UE */
	reg_data = &edac_reg[LLCC_DRAM_UE];
	reg_data->err_name = "DRAM Double-bit";
	reg_data->reg_cnt = DRP_SYN_REG_CNT;
	reg_data->synd_reg = DRP_ECC_DB_ERR_SYN0;
	reg_data->err_status_reg = DRP_ECC_ERROR_STATUS1;
	reg_data->err_count_mask = ECC_DB_ERR_COUNT_MASK;
	reg_data->err_ways_status = DRP_ECC_ERROR_STATUS0;
	reg_data->err_ways_mask = ECC_DB_ERR_WAYS_MASK;
	reg_data->err_ways_shift = ECC_DB_ERR_WAYS_SHIFT;

	/* Initialize register info for LLCC_TRAM_CE */
	reg_data = &edac_reg[LLCC_TRAM_CE];
	reg_data->err_name = "TRAM Single-bit";
	reg_data->reg_cnt = TRP_SYN_REG_CNT;
	reg_data->synd_reg = TRP_ECC_SB_ERR_SYN0;
	reg_data->err_status_reg = TRP_ECC_ERROR_STATUS1;
	reg_data->err_count_mask = ECC_SB_ERR_COUNT_MASK;
	reg_data->err_count_shift = ECC_SB_ERR_COUNT_SHIFT;
	reg_data->err_ways_status = TRP_ECC_ERROR_STATUS0;
	reg_data->err_ways_mask = ECC_SB_ERR_WAYS_MASK;

	/* Initialize register info for LLCC_TRAM_UE */
	reg_data = &edac_reg[LLCC_TRAM_UE];
	reg_data->err_name = "TRAM Double-bit";
	reg_data->reg_cnt = TRP_SYN_REG_CNT;
	reg_data->synd_reg = TRP_ECC_DB_ERR_SYN0;
	reg_data->err_status_reg = TRP_ECC_ERROR_STATUS1;
	reg_data->err_count_mask = ECC_DB_ERR_COUNT_MASK;
	reg_data->err_ways_status = TRP_ECC_ERROR_STATUS0;
	reg_data->err_ways_mask = ECC_DB_ERR_WAYS_MASK;
	reg_data->err_ways_shift = ECC_DB_ERR_WAYS_SHIFT;
}

static int qcom_llcc_edac_probe(struct platform_device *pdev)
{
	struct llcc_drv_data *llcc_driv_data = pdev->dev.platform_data;
	struct edac_device_ctl_info *edev_ctl;
	struct device *dev = &pdev->dev;
	int ecc_irq;
	int rc;

	/* Initialize register set for the error types*/
	llcc_driv_data->edac_reg = devm_kcalloc(dev, LLCC_ERR_TYPE_MAX,
					sizeof(struct llcc_edac_reg_data),
					GFP_KERNEL);
	llcc_edac_reg_data_init(llcc_driv_data->edac_reg);

	rc = qcom_llcc_core_setup(llcc_driv_data->bcast_regmap);
	if (rc)
		return rc;

	/* Allocate edac control info */
	edev_ctl = edac_device_alloc_ctl_info(0, "qcom-llcc", 1, "bank",
					      llcc_driv_data->num_banks, 1,
					      NULL, 0,
					      edac_device_alloc_index());

	if (!edev_ctl)
		return -ENOMEM;

	edev_ctl->dev = dev;
	edev_ctl->mod_name = dev_name(dev);
	edev_ctl->dev_name = dev_name(dev);
	edev_ctl->ctl_name = "llcc";
	edev_ctl->panic_on_ue = LLCC_ERP_PANIC_ON_UE;
	edev_ctl->pvt_info = llcc_driv_data;

	rc = edac_device_add_device(edev_ctl);
	if (rc)
		goto out_mem;

	platform_set_drvdata(pdev, edev_ctl);

	/* Request for ecc irq */
	ecc_irq = llcc_driv_data->ecc_irq;
	if (ecc_irq < 0) {
		rc = -ENODEV;
		goto out_dev;
	}
	rc = devm_request_irq(dev, ecc_irq, llcc_ecc_irq_handler,
			      IRQF_TRIGGER_HIGH, "llcc_ecc", edev_ctl);
	if (rc)
		goto out_dev;

	return rc;

out_dev:
	edac_device_del_device(edev_ctl->dev);
out_mem:
	edac_device_free_ctl_info(edev_ctl);

	return rc;
}

static int qcom_llcc_edac_remove(struct platform_device *pdev)
{
	struct edac_device_ctl_info *edev_ctl = dev_get_drvdata(&pdev->dev);

	edac_device_del_device(edev_ctl->dev);
	edac_device_free_ctl_info(edev_ctl);
	platform_set_drvdata(pdev, NULL);

	return 0;
}

static const struct of_device_id qcom_llcc_edac_match_table[] = {
#ifdef EDAC_QCOM_LLCC
	{ .compatible = "qcom,llcc-edac" },
#endif
	{ },
};

static struct platform_driver qcom_llcc_edac_driver = {
	.probe = qcom_llcc_edac_probe,
	.remove = qcom_llcc_edac_remove,
	.driver = {
		.name = "qcom_llcc_edac",
		.of_match_table = qcom_llcc_edac_match_table,
	},
};
module_platform_driver(qcom_llcc_edac_driver);

MODULE_DESCRIPTION("QCOM EDAC driver");
MODULE_LICENSE("GPL v2");
