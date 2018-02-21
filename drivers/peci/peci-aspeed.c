// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2012-2020 ASPEED Technology Inc.
// Copyright (c) 2018 Intel Corporation

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/peci.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

#define DUMP_DEBUG 0

/* Aspeed PECI Registers */
#define AST_PECI_CTRL     0x00
#define AST_PECI_TIMING   0x04
#define AST_PECI_CMD      0x08
#define AST_PECI_CMD_CTRL 0x0c
#define AST_PECI_EXP_FCS  0x10
#define AST_PECI_CAP_FCS  0x14
#define AST_PECI_INT_CTRL 0x18
#define AST_PECI_INT_STS  0x1c
#define AST_PECI_W_DATA0  0x20
#define AST_PECI_W_DATA1  0x24
#define AST_PECI_W_DATA2  0x28
#define AST_PECI_W_DATA3  0x2c
#define AST_PECI_R_DATA0  0x30
#define AST_PECI_R_DATA1  0x34
#define AST_PECI_R_DATA2  0x38
#define AST_PECI_R_DATA3  0x3c
#define AST_PECI_W_DATA4  0x40
#define AST_PECI_W_DATA5  0x44
#define AST_PECI_W_DATA6  0x48
#define AST_PECI_W_DATA7  0x4c
#define AST_PECI_R_DATA4  0x50
#define AST_PECI_R_DATA5  0x54
#define AST_PECI_R_DATA6  0x58
#define AST_PECI_R_DATA7  0x5c

/* AST_PECI_CTRL - 0x00 : Control Register */
#define PECI_CTRL_SAMPLING_MASK     GENMASK(19, 16)
#define PECI_CTRL_SAMPLING(x)       (((x) << 16) & PECI_CTRL_SAMPLING_MASK)
#define PECI_CTRL_SAMPLING_GET(x)   (((x) & PECI_CTRL_SAMPLING_MASK) >> 16)
#define PECI_CTRL_READ_MODE_MASK    GENMASK(13, 12)
#define PECI_CTRL_READ_MODE(x)      (((x) << 12) & PECI_CTRL_READ_MODE_MASK)
#define PECI_CTRL_READ_MODE_GET(x)  (((x) & PECI_CTRL_READ_MODE_MASK) >> 12)
#define PECI_CTRL_READ_MODE_COUNT   BIT(12)
#define PECI_CTRL_READ_MODE_DBG     BIT(13)
#define PECI_CTRL_CLK_SOURCE_MASK   BIT(11)
#define PECI_CTRL_CLK_SOURCE(x)     (((x) << 11) & PECI_CTRL_CLK_SOURCE_MASK)
#define PECI_CTRL_CLK_SOURCE_GET(x) (((x) & PECI_CTRL_CLK_SOURCE_MASK) >> 11)
#define PECI_CTRL_CLK_DIV_MASK      GENMASK(10, 8)
#define PECI_CTRL_CLK_DIV(x)        (((x) << 8) & PECI_CTRL_CLK_DIV_MASK)
#define PECI_CTRL_CLK_DIV_GET(x)    (((x) & PECI_CTRL_CLK_DIV_MASK) >> 8)
#define PECI_CTRL_INVERT_OUT        BIT(7)
#define PECI_CTRL_INVERT_IN         BIT(6)
#define PECI_CTRL_BUS_CONTENT_EN    BIT(5)
#define PECI_CTRL_PECI_EN           BIT(4)
#define PECI_CTRL_PECI_CLK_EN       BIT(0)

/* AST_PECI_TIMING - 0x04 : Timing Negotiation Register */
#define PECI_TIMING_MESSAGE_MASK   GENMASK(15, 8)
#define PECI_TIMING_MESSAGE(x)     (((x) << 8) & PECI_TIMING_MESSAGE_MASK)
#define PECI_TIMING_MESSAGE_GET(x) (((x) & PECI_TIMING_MESSAGE_MASK) >> 8)
#define PECI_TIMING_ADDRESS_MASK   GENMASK(7, 0)
#define PECI_TIMING_ADDRESS(x)     ((x) & PECI_TIMING_ADDRESS_MASK)
#define PECI_TIMING_ADDRESS_GET(x) ((x) & PECI_TIMING_ADDRESS_MASK)

/* AST_PECI_CMD - 0x08 : Command Register */
#define PECI_CMD_PIN_MON    BIT(31)
#define PECI_CMD_STS_MASK   GENMASK(27, 24)
#define PECI_CMD_STS_GET(x) (((x) & PECI_CMD_STS_MASK) >> 24)
#define PECI_CMD_FIRE       BIT(0)

/* AST_PECI_LEN - 0x0C : Read/Write Length Register */
#define PECI_AW_FCS_EN       BIT(31)
#define PECI_READ_LEN_MASK   GENMASK(23, 16)
#define PECI_READ_LEN(x)     (((x) << 16) & PECI_READ_LEN_MASK)
#define PECI_WRITE_LEN_MASK  GENMASK(15, 8)
#define PECI_WRITE_LEN(x)    (((x) << 8) & PECI_WRITE_LEN_MASK)
#define PECI_TAGET_ADDR_MASK GENMASK(7, 0)
#define PECI_TAGET_ADDR(x)   ((x) & PECI_TAGET_ADDR_MASK)

/* AST_PECI_EXP_FCS - 0x10 : Expected FCS Data Register */
#define PECI_EXPECT_READ_FCS_MASK      GENMASK(23, 16)
#define PECI_EXPECT_READ_FCS_GET(x)    (((x) & PECI_EXPECT_READ_FCS_MASK) >> 16)
#define PECI_EXPECT_AW_FCS_AUTO_MASK   GENMASK(15, 8)
#define PECI_EXPECT_AW_FCS_AUTO_GET(x) (((x) & PECI_EXPECT_AW_FCS_AUTO_MASK) \
					>> 8)
#define PECI_EXPECT_WRITE_FCS_MASK     GENMASK(7, 0)
#define PECI_EXPECT_WRITE_FCS_GET(x)   ((x) & PECI_EXPECT_WRITE_FCS_MASK)

/* AST_PECI_CAP_FCS - 0x14 : Captured FCS Data Register */
#define PECI_CAPTURE_READ_FCS_MASK    GENMASK(23, 16)
#define PECI_CAPTURE_READ_FCS_GET(x)  (((x) & PECI_CAPTURE_READ_FCS_MASK) >> 16)
#define PECI_CAPTURE_WRITE_FCS_MASK   GENMASK(7, 0)
#define PECI_CAPTURE_WRITE_FCS_GET(x) ((x) & PECI_CAPTURE_WRITE_FCS_MASK)

/* AST_PECI_INT_CTRL/STS - 0x18/0x1c : Interrupt Register */
#define PECI_INT_TIMING_RESULT_MASK GENMASK(31, 30)
#define PECI_INT_TIMEOUT            BIT(4)
#define PECI_INT_CONNECT            BIT(3)
#define PECI_INT_W_FCS_BAD          BIT(2)
#define PECI_INT_W_FCS_ABORT        BIT(1)
#define PECI_INT_CMD_DONE           BIT(0)

struct aspeed_peci {
	struct peci_adapter	adaper;
	struct device		*dev;
	struct regmap		*regmap;
	int			irq;
	struct completion	xfer_complete;
	u32			sts;
	u32			cmd_timeout_ms;
};

#define PECI_INT_MASK  (PECI_INT_TIMEOUT | PECI_INT_CONNECT | \
			PECI_INT_W_FCS_BAD | PECI_INT_W_FCS_ABORT | \
			PECI_INT_CMD_DONE)

#define PECI_IDLE_CHECK_TIMEOUT_MS      50
#define PECI_IDLE_CHECK_INTERVAL_MS     10

#define PECI_RD_SAMPLING_POINT_DEFAULT  8
#define PECI_RD_SAMPLING_POINT_MAX      15
#define PECI_CLK_DIV_DEFAULT            0
#define PECI_CLK_DIV_MAX                7
#define PECI_MSG_TIMING_NEGO_DEFAULT    1
#define PECI_MSG_TIMING_NEGO_MAX        255
#define PECI_ADDR_TIMING_NEGO_DEFAULT   1
#define PECI_ADDR_TIMING_NEGO_MAX       255
#define PECI_CMD_TIMEOUT_MS_DEFAULT     1000
#define PECI_CMD_TIMEOUT_MS_MAX         60000

static int aspeed_peci_xfer_native(struct aspeed_peci *priv,
				   struct peci_xfer_msg *msg)
{
	u32 peci_head, peci_state, rx_data, cmd_sts;
	uint reg;
	ktime_t start, end;
	s64 elapsed_ms;
	long err, timeout = msecs_to_jiffies(priv->cmd_timeout_ms);
	int i, rc = 0;

	start = ktime_get();

	/* Check command sts and bus idle state */
	while (!regmap_read(priv->regmap, AST_PECI_CMD, &cmd_sts) &&
	       (cmd_sts & (PECI_CMD_STS_MASK | PECI_CMD_PIN_MON))) {
		end = ktime_get();
		elapsed_ms = ktime_to_ms(ktime_sub(end, start));
		if (elapsed_ms >= PECI_IDLE_CHECK_TIMEOUT_MS) {
			dev_dbg(priv->dev, "Timeout waiting for idle state!\n");
			return -ETIMEDOUT;
		}

		usleep_range(PECI_IDLE_CHECK_INTERVAL_MS * 1000,
			     (PECI_IDLE_CHECK_INTERVAL_MS * 1000) + 1000);
	};

	reinit_completion(&priv->xfer_complete);

	peci_head = PECI_TAGET_ADDR(msg->addr) |
				    PECI_WRITE_LEN(msg->tx_len) |
				    PECI_READ_LEN(msg->rx_len);

	rc = regmap_write(priv->regmap, AST_PECI_CMD_CTRL, peci_head);
	if (rc)
		return rc;

	for (i = 0; i < msg->tx_len; i += 4) {
		reg = i < 16 ? AST_PECI_W_DATA0 + i % 16 :
			       AST_PECI_W_DATA4 + i % 16;
		rc = regmap_write(priv->regmap, reg,
				  (msg->tx_buf[i + 3] << 24) |
				  (msg->tx_buf[i + 2] << 16) |
				  (msg->tx_buf[i + 1] << 8) |
				  msg->tx_buf[i + 0]);
		if (rc)
			return rc;
	}

	dev_dbg(priv->dev, "HEAD : 0x%08x\n", peci_head);
#if DUMP_DEBUG
	print_hex_dump(KERN_DEBUG, "TX : ", DUMP_PREFIX_NONE, 16, 1,
		       msg->tx_buf, msg->tx_len, true);
#endif

	rc = regmap_write(priv->regmap, AST_PECI_CMD, PECI_CMD_FIRE);
	if (rc)
		return rc;

	err = wait_for_completion_interruptible_timeout(&priv->xfer_complete,
							timeout);

	dev_dbg(priv->dev, "INT_STS : 0x%08x\n", priv->sts);
	if (!regmap_read(priv->regmap, AST_PECI_CMD, &peci_state))
		dev_dbg(priv->dev, "PECI_STATE : 0x%lx\n",
			PECI_CMD_STS_GET(peci_state));
	else
		dev_dbg(priv->dev, "PECI_STATE : read error\n");

	if (err <= 0 || !(priv->sts & PECI_INT_CMD_DONE)) {
		if (err < 0) { /* -ERESTARTSYS */
			return (int)err;
		} else if (err == 0) {
			dev_dbg(priv->dev, "Timeout waiting for a response!\n");
			return -ETIMEDOUT;
		}

		dev_dbg(priv->dev, "No valid response!\n");
		return -EFAULT;
	}

	for (i = 0; i < msg->rx_len; i++) {
		u8 byte_offset = i % 4;

		if (byte_offset == 0) {
			reg = i < 16 ? AST_PECI_R_DATA0 + i % 16 :
				       AST_PECI_R_DATA4 + i % 16;
			rc = regmap_read(priv->regmap, reg, &rx_data);
			if (rc)
				return rc;
		}

		msg->rx_buf[i] = (u8)(rx_data >> (byte_offset << 3));
	}

#if DUMP_DEBUG
	print_hex_dump(KERN_DEBUG, "RX : ", DUMP_PREFIX_NONE, 16, 1,
		       msg->rx_buf, msg->rx_len, true);
#endif
	if (!regmap_read(priv->regmap, AST_PECI_CMD, &peci_state))
		dev_dbg(priv->dev, "PECI_STATE : 0x%lx\n",
			PECI_CMD_STS_GET(peci_state));
	else
		dev_dbg(priv->dev, "PECI_STATE : read error\n");
	dev_dbg(priv->dev, "------------------------\n");

	return rc;
}

static irqreturn_t aspeed_peci_irq_handler(int irq, void *arg)
{
	struct aspeed_peci *priv = arg;
	bool valid_irq = true;

	if (regmap_read(priv->regmap, AST_PECI_INT_STS, &priv->sts))
		return IRQ_NONE;

	switch (priv->sts & PECI_INT_MASK) {
	case PECI_INT_TIMEOUT:
		dev_dbg(priv->dev, "PECI_INT_TIMEOUT\n");
		if (regmap_write(priv->regmap, AST_PECI_INT_STS,
				 PECI_INT_TIMEOUT))
			return IRQ_NONE;
		break;
	case PECI_INT_CONNECT:
		dev_dbg(priv->dev, "PECI_INT_CONNECT\n");
		if (regmap_write(priv->regmap, AST_PECI_INT_STS,
				 PECI_INT_CONNECT))
			return IRQ_NONE;
		break;
	case PECI_INT_W_FCS_BAD:
		dev_dbg(priv->dev, "PECI_INT_W_FCS_BAD\n");
		if (regmap_write(priv->regmap, AST_PECI_INT_STS,
				 PECI_INT_W_FCS_BAD))
			return IRQ_NONE;
		break;
	case PECI_INT_W_FCS_ABORT:
		dev_dbg(priv->dev, "PECI_INT_W_FCS_ABORT\n");
		if (regmap_write(priv->regmap, AST_PECI_INT_STS,
				 PECI_INT_W_FCS_ABORT))
			return IRQ_NONE;
		break;
	case PECI_INT_CMD_DONE:
		dev_dbg(priv->dev, "PECI_INT_CMD_DONE\n");
		if (regmap_write(priv->regmap, AST_PECI_INT_STS,
				 PECI_INT_CMD_DONE) ||
		    regmap_write(priv->regmap, AST_PECI_CMD, 0))
			return IRQ_NONE;
		break;
	default:
		dev_dbg(priv->dev, "Unknown PECI interrupt : 0x%08x\n",
			priv->sts);
		if (regmap_write(priv->regmap, AST_PECI_INT_STS, priv->sts))
			return IRQ_NONE;
		valid_irq = false;
		break;
	}

	if (valid_irq)
		complete(&priv->xfer_complete);

	return IRQ_HANDLED;
}

static int aspeed_peci_init_ctrl(struct aspeed_peci *priv)
{
	struct clk *clkin;
	u32 clk_freq, clk_divisor, clk_div_val = 0;
	u32 msg_timing_nego, addr_timing_nego, rd_sampling_point;
	int ret;

	clkin = devm_clk_get(priv->dev, NULL);
	if (IS_ERR(clkin)) {
		dev_err(priv->dev, "Failed to get clk source.\n");
		return PTR_ERR(clkin);
	}

	ret = of_property_read_u32(priv->dev->of_node, "clock-frequency",
				   &clk_freq);
	if (ret < 0) {
		dev_err(priv->dev,
			"Could not read clock-frequency property.\n");
		return ret;
	}

	clk_divisor = clk_get_rate(clkin) / clk_freq;
	devm_clk_put(priv->dev, clkin);

	while ((clk_divisor >> 1) && (clk_div_val < PECI_CLK_DIV_MAX))
		clk_div_val++;

	ret = of_property_read_u32(priv->dev->of_node, "msg-timing-nego",
				   &msg_timing_nego);
	if (ret || msg_timing_nego > PECI_MSG_TIMING_NEGO_MAX) {
		dev_warn(priv->dev,
			 "Invalid msg-timing-nego : %u, Use default : %u\n",
			 msg_timing_nego, PECI_MSG_TIMING_NEGO_DEFAULT);
		msg_timing_nego = PECI_MSG_TIMING_NEGO_DEFAULT;
	}

	ret = of_property_read_u32(priv->dev->of_node, "addr-timing-nego",
				   &addr_timing_nego);
	if (ret || addr_timing_nego > PECI_ADDR_TIMING_NEGO_MAX) {
		dev_warn(priv->dev,
			 "Invalid addr-timing-nego : %u, Use default : %u\n",
			 addr_timing_nego, PECI_ADDR_TIMING_NEGO_DEFAULT);
		addr_timing_nego = PECI_ADDR_TIMING_NEGO_DEFAULT;
	}

	ret = of_property_read_u32(priv->dev->of_node, "rd-sampling-point",
				   &rd_sampling_point);
	if (ret || rd_sampling_point > PECI_RD_SAMPLING_POINT_MAX) {
		dev_warn(priv->dev,
			 "Invalid rd-sampling-point : %u. Use default : %u\n",
			 rd_sampling_point,
			 PECI_RD_SAMPLING_POINT_DEFAULT);
		rd_sampling_point = PECI_RD_SAMPLING_POINT_DEFAULT;
	}

	ret = of_property_read_u32(priv->dev->of_node, "cmd-timeout-ms",
				   &priv->cmd_timeout_ms);
	if (ret || priv->cmd_timeout_ms > PECI_CMD_TIMEOUT_MS_MAX ||
	    priv->cmd_timeout_ms == 0) {
		dev_warn(priv->dev,
			 "Invalid cmd-timeout-ms : %u. Use default : %u\n",
			 priv->cmd_timeout_ms,
			 PECI_CMD_TIMEOUT_MS_DEFAULT);
		priv->cmd_timeout_ms = PECI_CMD_TIMEOUT_MS_DEFAULT;
	}

	ret = regmap_write(priv->regmap, AST_PECI_CTRL,
			   PECI_CTRL_CLK_DIV(PECI_CLK_DIV_DEFAULT) |
			   PECI_CTRL_PECI_CLK_EN);
	if (ret)
		return ret;

	usleep_range(1000, 5000);

	/**
	 * Timing negotiation period setting.
	 * The unit of the programmed value is 4 times of PECI clock period.
	 */
	ret = regmap_write(priv->regmap, AST_PECI_TIMING,
			   PECI_TIMING_MESSAGE(msg_timing_nego) |
			   PECI_TIMING_ADDRESS(addr_timing_nego));
	if (ret)
		return ret;

	/* Clear interrupts */
	ret = regmap_write(priv->regmap, AST_PECI_INT_STS, PECI_INT_MASK);
	if (ret)
		return ret;

	/* Enable interrupts */
	ret = regmap_write(priv->regmap, AST_PECI_INT_CTRL, PECI_INT_MASK);
	if (ret)
		return ret;

	/* Read sampling point and clock speed setting */
	ret = regmap_write(priv->regmap, AST_PECI_CTRL,
			   PECI_CTRL_SAMPLING(rd_sampling_point) |
			   PECI_CTRL_CLK_DIV(clk_div_val) |
			   PECI_CTRL_PECI_EN | PECI_CTRL_PECI_CLK_EN);
	if (ret)
		return ret;

	return 0;
}

static const struct regmap_config aspeed_peci_regmap_config = {
	.reg_bits = 32,
	.val_bits = 32,
	.reg_stride = 4,
	.max_register = AST_PECI_R_DATA7,
	.val_format_endian = REGMAP_ENDIAN_LITTLE,
	.fast_io = true,
};

static int aspeed_peci_xfer(struct peci_adapter *adaper,
			    struct peci_xfer_msg *msg)
{
	struct aspeed_peci *priv = peci_get_adapdata(adaper);

	return aspeed_peci_xfer_native(priv, msg);
}

static int aspeed_peci_probe(struct platform_device *pdev)
{
	struct aspeed_peci *priv;
	struct resource *res;
	void __iomem *base;
	int ret = 0;

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	dev_set_drvdata(&pdev->dev, priv);
	priv->dev = &pdev->dev;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(base))
		return PTR_ERR(base);

	priv->regmap = devm_regmap_init_mmio(&pdev->dev, base,
					     &aspeed_peci_regmap_config);
	if (IS_ERR(priv->regmap))
		return PTR_ERR(priv->regmap);

	priv->irq = platform_get_irq(pdev, 0);
	if (!priv->irq)
		return -ENODEV;

	ret = devm_request_irq(&pdev->dev, priv->irq, aspeed_peci_irq_handler,
			       IRQF_SHARED,
			       "peci-aspeed-irq",
			       priv);
	if (ret < 0)
		return ret;

	init_completion(&priv->xfer_complete);

	priv->adaper.dev.parent = priv->dev;
	priv->adaper.dev.of_node = of_node_get(dev_of_node(priv->dev));
	strlcpy(priv->adaper.name, pdev->name, sizeof(priv->adaper.name));
	priv->adaper.xfer = aspeed_peci_xfer;
	peci_set_adapdata(&priv->adaper, priv);

	ret = aspeed_peci_init_ctrl(priv);
	if (ret < 0)
		return ret;

	ret = peci_add_adapter(&priv->adaper);
	if (ret < 0)
		return ret;

	dev_info(&pdev->dev, "peci bus %d registered, irq %d\n",
		 priv->adaper.nr, priv->irq);

	return 0;
}

static int aspeed_peci_remove(struct platform_device *pdev)
{
	struct aspeed_peci *priv = dev_get_drvdata(&pdev->dev);

	peci_del_adapter(&priv->adaper);
	of_node_put(priv->adaper.dev.of_node);

	return 0;
}

static const struct of_device_id aspeed_peci_of_table[] = {
	{ .compatible = "aspeed,ast2400-peci", },
	{ .compatible = "aspeed,ast2500-peci", },
	{ }
};
MODULE_DEVICE_TABLE(of, aspeed_peci_of_table);

static struct platform_driver aspeed_peci_driver = {
	.probe  = aspeed_peci_probe,
	.remove = aspeed_peci_remove,
	.driver = {
		.name           = "peci-aspeed",
		.of_match_table = of_match_ptr(aspeed_peci_of_table),
	},
};
module_platform_driver(aspeed_peci_driver);

MODULE_AUTHOR("Ryan Chen <ryan_chen@aspeedtech.com>");
MODULE_AUTHOR("Jae Hyun Yoo <jae.hyun.yoo@linux.intel.com>");
MODULE_DESCRIPTION("Aspeed PECI driver");
MODULE_LICENSE("GPL v2");
