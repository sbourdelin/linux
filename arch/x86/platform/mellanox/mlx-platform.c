/*
 * arch/x86/platform/mellanox/mlx-platform.c
 * Copyright (c) 2016 Mellanox Technologies. All rights reserved.
 * Copyright (c) 2016 Vadim Pasternak <vadimp@mellanox.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the names of the copyright holders nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 * Alternatively, this software may be distributed under the terms of the
 * GNU General Public License ("GPL") version 2 as published by the Free
 * Software Foundation.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <linux/acpi.h>
#include <linux/device.h>
#include <linux/i2c.h>
#include <linux/i2c-mux.h>
#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/platform_data/i2c-mux-reg.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/version.h>

#define MLX_PLAT_DEVICE_NAME		"mlxplat"

/* LPC IFC in PCH defines */
#define MLXPLAT_CPLD_LPC_I2C_BASE_ADRR		0x2000
#define MLXPLAT_CPLD_LPC_REG_BASE_ADRR		0x2500
#define MLXPLAT_CPLD_LPC_CTRL_IFC_BUS_ID	0
#define MLXPLAT_CPLD_LPC_CTRL_IFC_SLOT_ID	31
#define MLXPLAT_CPLD_LPC_CTRL_IFC_FUNC_ID	0
#define MLXPLAT_CPLD_LPC_QM67_DEV_ID		0x1c4f
#define MLXPLAT_CPLD_LPC_QM77_DEV_ID		0x1e55
#define MLXPLAT_CPLD_LPC_RNG_DEV_ID		0x1f38
#define MLXPLAT_CPLD_LPC_I2C_CH1_OFF		0xdb
#define MLXPLAT_CPLD_LPC_I2C_CH2_OFF		0xda
#define MLXPLAT_CPLD_LPC_PIO_OFFSET		0x10000UL
#define MLXPLAT_CPLD_LPC_REG1	((MLXPLAT_CPLD_LPC_REG_BASE_ADRR + \
				  MLXPLAT_CPLD_LPC_I2C_CH1_OFF) | \
				  MLXPLAT_CPLD_LPC_PIO_OFFSET)
#define MLXPLAT_CPLD_LPC_REG2	((MLXPLAT_CPLD_LPC_REG_BASE_ADRR + \
				  MLXPLAT_CPLD_LPC_I2C_CH2_OFF) | \
				  MLXPLAT_CPLD_LPC_PIO_OFFSET)

/* Use generic decode range 4 for CPLD LPC */
#define MLXPLAT_CPLD_LPC_PCH_GEN_DEC_RANGE4	0x90
#define MLXPLAT_CPLD_LPC_PCH_GEN_DEC_BASE	0x84
#define MLXPLAT_CPLD_LPC_RNG_CTRL		0x84
#define MLXPLAT_CPLD_LPC_PCH_GEN_DEC_RANGES	4
#define MLXPLAT_CPLD_LPC_I2C_RANGE		2
#define MLXPLAT_CPLD_LPC_RANGE			3
#define MLXPLAT_CPLD_LPC_CLKS_EN		0
#define MLXPLAT_CPLD_LPC_IO_RANGE		0x100

/* Start channel numbers */
#define MLXPLAT_CPLD_CH1			2
#define MLXPLAT_CPLD_CH2			10

/* mlxplat_priv - board private data
 * @lpc_reg - register space
 * @dev_id - platform device id
 * @lpc_i2c_res- lpc cpld i2c resource space
 * @lpc_cpld_res - lpc cpld register resource space
 * @pdev - platform device
 */
struct mlxplat_priv {
	u32 lpc_reg[MLXPLAT_CPLD_LPC_PCH_GEN_DEC_RANGES];
	u16 dev_id;
	struct resource *lpc_i2c_res;
	struct resource *lpc_cpld_res;
	struct platform_device *pdev;
	struct platform_device *pdev_i2c;
};

/* Regions for LPC I2C controller and LPC base register space */
static struct resource mlxplat_lpc_resources[] = {
	[0] = DEFINE_RES_NAMED(MLXPLAT_CPLD_LPC_I2C_BASE_ADRR,
			       MLXPLAT_CPLD_LPC_IO_RANGE,
			       "mlxplat_cpld_lpc_i2c_ctrl", IORESOURCE_IO),
	[1] = DEFINE_RES_NAMED(MLXPLAT_CPLD_LPC_REG_BASE_ADRR,
			       MLXPLAT_CPLD_LPC_IO_RANGE,
			       "mlxplat_cpld_lpc_regs",
			       IORESOURCE_IO),
};

/* Platfform channels */
static int mlxplat_channels[][8] = {
	{
		MLXPLAT_CPLD_CH1, MLXPLAT_CPLD_CH1 + 1, MLXPLAT_CPLD_CH1 + 2,
		MLXPLAT_CPLD_CH1 + 3, MLXPLAT_CPLD_CH1 + 4, MLXPLAT_CPLD_CH1 +
		5, MLXPLAT_CPLD_CH1 + 6, MLXPLAT_CPLD_CH1 + 7
	},
	{
		MLXPLAT_CPLD_CH2, MLXPLAT_CPLD_CH2 + 1, MLXPLAT_CPLD_CH2 + 2,
		MLXPLAT_CPLD_CH2 + 3, MLXPLAT_CPLD_CH2 + 4, MLXPLAT_CPLD_CH2 +
		+ 5, MLXPLAT_CPLD_CH2 + 6, MLXPLAT_CPLD_CH2 + 7
	},
};

/* Platform mux data */
struct i2c_mux_reg_platform_data mlxplat_mux_data[] = {
	{
		.parent = 1,
		.base_nr = MLXPLAT_CPLD_CH1,
		.write_only = 1,
		.values = mlxplat_channels[0],
		.n_values = ARRAY_SIZE(mlxplat_channels[0]),
		.reg = (void __iomem *)MLXPLAT_CPLD_LPC_REG1,
		.reg_size = 1,
		.idle_in_use = 1,
	},
	{
		.parent = 1,
		.base_nr = MLXPLAT_CPLD_CH2,
		.write_only = 1,
		.values = mlxplat_channels[1],
		.n_values = ARRAY_SIZE(mlxplat_channels[1]),
		.reg = (void __iomem *)MLXPLAT_CPLD_LPC_REG2,
		.reg_size = 1,
		.idle_in_use = 1,
	},

};

/* mlxplat_topology - platform entry data:
 * @pdev - platform device
 * @name - platform device name
 */
struct mlxplat_topology {
	struct platform_device *pdev;
	const char *name;
};

/* Platform topology */
struct mlxplat_topology mlxplat_topo[] = {
	{
		.name = "i2c-mux-reg",
	},
	{
		.name = "i2c-mux-reg",
	},
};

struct platform_device *mlxplat_dev;

static int mlxplat_lpc_i2c_dec_range_config(struct mlxplat_priv *priv,
					    struct pci_dev *pdev, u8 range,
					    u16 base_addr)
{
	u16 rng_reg;
	u32 val;
	int err;

	if (range >= MLXPLAT_CPLD_LPC_PCH_GEN_DEC_RANGES) {
		dev_err(&priv->pdev->dev, "Incorrect LPC decode range %d > %d\n",
			range, MLXPLAT_CPLD_LPC_PCH_GEN_DEC_RANGES);
		return -ERANGE;
	}

	rng_reg = MLXPLAT_CPLD_LPC_PCH_GEN_DEC_BASE + 4 * range;
	err = pci_read_config_dword(pdev, rng_reg, &val);
	if (err) {
		dev_err(&priv->pdev->dev, "Access to LPC_PCH config failed, err %d\n",
			err);
		return -EFAULT;
	}
	priv->lpc_reg[range] = val;

	/* Clean all bits excepted reserved (reserved: 2, 16, 17 , 24 - 31). */
	val &= 0xff030002;
	/* Set bits 18 - 23 to allow decode range address mask, set bit 1 to
	 * enable decode range, clear bit 1,2 in base address.
	 */
	val |= 0xfc0001 | (base_addr & 0xfff3);
	err = pci_write_config_dword(pdev, rng_reg, val);
	if (err)
		dev_err(&priv->pdev->dev, "Config of LPC_PCH Generic Decode Range %d failed, err %d\n",
			range, err);

	return err;
}

static void mlxplat_lpc_dec_rng_config_clean(struct pci_dev *pdev, u32 val,
					     u8 range)
{
	/* Restore old value */
	if (pci_write_config_dword(pdev, (MLXPLAT_CPLD_LPC_PCH_GEN_DEC_BASE +
				   range * 4), val))
		dev_err(&pdev->dev, "Deconfig of LPC_PCH Generic Decode Range %x failed\n",
			range);

}

static int mlxplat_lpc_request_region(struct mlxplat_priv *priv,
				      struct resource *res)
{
	resource_size_t size = resource_size(res);

	if (!devm_request_region(&priv->pdev->dev, res->start, size,
				 res->name)) {
		devm_release_region(&priv->pdev->dev, res->start, size);

		if (!devm_request_region(&priv->pdev->dev, res->start, size,
					 res->name)) {
			dev_err(&priv->pdev->dev, "Request ioregion 0x%llx len 0x%llx for %s fail\n",
				res->start, size, res->name);
			return -EIO;
		}
	}

	return 0;
}

static int mlxplat_lpc_request_regions(struct mlxplat_priv *priv)
{
	int i;
	int err;

	for (i = 0; i < ARRAY_SIZE(mlxplat_lpc_resources); i++) {
		err = mlxplat_lpc_request_region(priv,
						 &mlxplat_lpc_resources[i]);
		if (err)
			return err;
	}

	priv->lpc_i2c_res = &mlxplat_lpc_resources[0];
	priv->lpc_cpld_res = &mlxplat_lpc_resources[1];

	return 0;
}

static int mlxplat_lpc_ivb_config(struct mlxplat_priv *priv,
				  struct pci_dev *pdev)
{
	int err;

	err = mlxplat_lpc_i2c_dec_range_config(priv, pdev,
					       MLXPLAT_CPLD_LPC_I2C_RANGE,
					       MLXPLAT_CPLD_LPC_I2C_BASE_ADRR);
	if (err) {
		dev_err(&priv->pdev->dev, "LPC decode range %d config failed, err %d\n",
			MLXPLAT_CPLD_LPC_I2C_RANGE, err);
		pci_dev_put(pdev);
		return -EFAULT;
	}

	err = mlxplat_lpc_i2c_dec_range_config(priv, pdev,
					       MLXPLAT_CPLD_LPC_RANGE,
					       MLXPLAT_CPLD_LPC_REG_BASE_ADRR);
	if (err) {
		dev_err(&priv->pdev->dev, "LPC decode range %d config failed, err %d\n",
			MLXPLAT_CPLD_LPC_I2C_RANGE, err);
		return -EFAULT;
	}

	return err;
}

static void mlxplat_lpc_ivb_config_clean(struct mlxplat_priv *priv,
					 struct pci_dev *pdev)
{
	mlxplat_lpc_dec_rng_config_clean(pdev,
				priv->lpc_reg[MLXPLAT_CPLD_LPC_RANGE],
				MLXPLAT_CPLD_LPC_RANGE);
	mlxplat_lpc_dec_rng_config_clean(pdev,
				priv->lpc_reg[MLXPLAT_CPLD_LPC_I2C_RANGE],
				MLXPLAT_CPLD_LPC_I2C_RANGE);

}

static int mlxplat_lpc_range_config(struct mlxplat_priv *priv,
				    struct pci_dev *pdev)
{
	u32 val, lpc_clks;
	int err;

	err = pci_read_config_dword(pdev, MLXPLAT_CPLD_LPC_RNG_CTRL, &val);
	if (err) {
		dev_err(&priv->pdev->dev, "Access to LPC Ctrl reg failed, err %d\n",
			err);
		return -EFAULT;
	}
	lpc_clks = val & 0x3;
	if (lpc_clks != MLXPLAT_CPLD_LPC_CLKS_EN) {
		val &= 0xFFFFFFFC;
		err = pci_write_config_dword(pdev, MLXPLAT_CPLD_LPC_RNG_CTRL,
					     val);
		if (err) {
			dev_err(&priv->pdev->dev, "Config LPC CLKS CTRL failed, err %d\n",
				err);
			return -EFAULT;
		}
	}

	return err;
}

static int mlxplat_lpc_config(struct mlxplat_priv *priv)
{
	struct pci_dev *pdev = NULL;
	u16 dev_id;
	int err;

	pdev = pci_get_bus_and_slot(MLXPLAT_CPLD_LPC_CTRL_IFC_BUS_ID,
				PCI_DEVFN(MLXPLAT_CPLD_LPC_CTRL_IFC_SLOT_ID,
				MLXPLAT_CPLD_LPC_CTRL_IFC_FUNC_ID));

	if (!pdev) {
		dev_err(&priv->pdev->dev, "LPC controller bus:%d slot:%d func:%d not found\n",
			MLXPLAT_CPLD_LPC_CTRL_IFC_BUS_ID,
			MLXPLAT_CPLD_LPC_CTRL_IFC_SLOT_ID,
			MLXPLAT_CPLD_LPC_CTRL_IFC_FUNC_ID);
		return -EFAULT;
	}

	err = pci_read_config_word(pdev, 2, &dev_id);
	if (err) {
		dev_err(&priv->pdev->dev, "Access PCIe LPC interface failed, err %d\n",
			err);
		goto failure;
	}

	switch (dev_id) {
	case MLXPLAT_CPLD_LPC_QM67_DEV_ID:
	case MLXPLAT_CPLD_LPC_QM77_DEV_ID:
		err = mlxplat_lpc_ivb_config(priv, pdev);
		break;
	case MLXPLAT_CPLD_LPC_RNG_DEV_ID:
		err = mlxplat_lpc_range_config(priv, pdev);
		break;
	default:
		err = -ENXIO;
		dev_err(&priv->pdev->dev, "Unsupported DevId 0x%x bus:%d slot:%d func:%d\n",
			dev_id, MLXPLAT_CPLD_LPC_CTRL_IFC_BUS_ID,
			MLXPLAT_CPLD_LPC_CTRL_IFC_SLOT_ID,
			MLXPLAT_CPLD_LPC_CTRL_IFC_FUNC_ID);
		goto failure;
	}
	priv->dev_id = dev_id;

failure:
	pci_dev_put(pdev);
	return err;
}

static int mlxplat_lpc_config_clean(struct mlxplat_priv *priv)
{
	struct pci_dev *pdev = NULL;
	int err = 0;

	pdev = pci_get_bus_and_slot(MLXPLAT_CPLD_LPC_CTRL_IFC_BUS_ID,
				PCI_DEVFN(MLXPLAT_CPLD_LPC_CTRL_IFC_SLOT_ID,
				MLXPLAT_CPLD_LPC_CTRL_IFC_FUNC_ID));
	if (!pdev) {
		dev_err(&priv->pdev->dev, "LPC controller bus:%d slot:%d func:%d not found\n",
			MLXPLAT_CPLD_LPC_CTRL_IFC_BUS_ID,
			MLXPLAT_CPLD_LPC_CTRL_IFC_SLOT_ID,
			MLXPLAT_CPLD_LPC_CTRL_IFC_FUNC_ID);
		return -EFAULT;
	}

	switch (priv->dev_id) {
	case MLXPLAT_CPLD_LPC_QM67_DEV_ID:
	case MLXPLAT_CPLD_LPC_QM77_DEV_ID:
		mlxplat_lpc_ivb_config_clean(priv, pdev);
		break;
	case MLXPLAT_CPLD_LPC_RNG_DEV_ID:
		break;
	default:
		err = -ENXIO;
		dev_err(&priv->pdev->dev, "Unsupported DevId 0x%x bus:%d slot:%d func:%d\n",
			priv->dev_id, MLXPLAT_CPLD_LPC_CTRL_IFC_BUS_ID,
			MLXPLAT_CPLD_LPC_CTRL_IFC_SLOT_ID,
			MLXPLAT_CPLD_LPC_CTRL_IFC_FUNC_ID);
		break;
	}

	pci_dev_put(pdev);

	return err;
}

static int __init mlxplat_init(void)
{
	struct mlxplat_priv *priv;
	struct device *dev;
	int i, j;
	int err;

	mlxplat_dev = platform_device_alloc(MLX_PLAT_DEVICE_NAME, -1);
	if (!mlxplat_dev) {
		pr_err("Alloc %s platform device failed\n",
			MLX_PLAT_DEVICE_NAME);
		return -ENOMEM;
	}

	err = platform_device_add(mlxplat_dev);
	if (err) {
		pr_err("Add %s platform device failed (%d)\n",
			MLX_PLAT_DEVICE_NAME, err);
		goto fail_platform_device_add;
	}
	dev = &mlxplat_dev->dev;

	priv = devm_kzalloc(dev, sizeof(struct mlxplat_priv), GFP_KERNEL);
	if (!priv) {
		err = -ENOMEM;
		dev_err(dev, "Failed to allocate mlxplat_priv\n");
		goto fail_alloc;
	}
	platform_set_drvdata(mlxplat_dev, priv);
	priv->pdev = mlxplat_dev;

	err = mlxplat_lpc_config(priv);
	if (err) {
		dev_err(dev, "Failed to configure LPC interface\n");
		goto fail_alloc;
	}

	err = mlxplat_lpc_request_regions(priv);
	if (err) {
		dev_err(dev, "Request ioregion failed (%d)\n", err);
		goto fail_alloc;
	}

	priv->pdev_i2c = platform_device_register_simple("i2c_mlxcpld", -1,
							 NULL, 0);
	if (IS_ERR(priv->pdev_i2c)) {
		err = PTR_ERR(priv->pdev_i2c);
		goto fail_alloc;
	};

	for (i = 0; i < ARRAY_SIZE(mlxplat_mux_data); i++) {
		mlxplat_topo[i].pdev = platform_device_register_resndata(dev,
						mlxplat_topo[i].name, i, NULL,
						0, &mlxplat_mux_data[i],
						sizeof(mlxplat_mux_data[i]));
		if (IS_ERR(mlxplat_topo[i].pdev)) {
			err = PTR_ERR(mlxplat_topo[i].pdev);
			goto fail_platform_mux_register;
		}
	}

	return err;

fail_platform_mux_register:
	for (j = i; j > 0 ; j--)
		platform_device_unregister(mlxplat_topo[j].pdev);
	platform_device_unregister(priv->pdev_i2c);
fail_alloc:
	platform_device_del(mlxplat_dev);
fail_platform_device_add:
	platform_device_put(mlxplat_dev);

	return err;
}

static void __exit mlxplat_exit(void)
{
	int i;
	struct mlxplat_priv *priv = platform_get_drvdata(mlxplat_dev);

	for (i = ARRAY_SIZE(mlxplat_mux_data) - 1; i >= 0 ; i--)
		platform_device_unregister(mlxplat_topo[i].pdev);

	platform_device_unregister(priv->pdev_i2c);
	mlxplat_lpc_config_clean(priv);
	platform_device_del(mlxplat_dev);
	platform_device_put(mlxplat_dev);
}

module_init(mlxplat_init);
module_exit(mlxplat_exit);

MODULE_AUTHOR("Vadim Pasternak (vadimp@mellanox.com)");
MODULE_DESCRIPTION("Mellanox platform driver");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:mlx-platform");
