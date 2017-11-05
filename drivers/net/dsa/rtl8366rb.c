/*
 * Realtek SMI subdriver for the Realtek RTL8366RB ethernet switch
 *
 * Copyright (C) 2017 Linus Walleij <linus.walleij@linaro.org>
 * Copyright (C) 2009-2010 Gabor Juhos <juhosg@openwrt.org>
 * Copyright (C) 2010 Antti Seppälä <a.seppala@gmail.com>
 * Copyright (C) 2010 Roman Yeryomin <roman@advem.lv>
 * Copyright (C) 2011 Colin Leitner <colin.leitner@googlemail.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation.
 */

#include <linux/regmap.h>
#include <linux/bitops.h>
#include <linux/interrupt.h>
#include <linux/irqdomain.h>
#include <linux/irqchip/chained_irq.h>
#include <linux/of_irq.h>

#include "realtek-smi.h"

#define RTL8366RB_PORT_NUM_CPU		5
#define RTL8366RB_NUM_PORTS		6
#define RTL8366RB_PHY_NO_MAX		4
#define RTL8366RB_PHY_ADDR_MAX		31

#define RTL8366RB_PAACR0		0x10 /* bits 0..7 = port 0, bits 8..15 = port 1 */
#define RTL8366RB_PAACR1		0x11 /* bits 0..7 = port 2, bits 8..15 = port 3 */
#define RTL8366RB_PAACR2		0x12 /* bits 0..7 = port 4, bits 8..15 = port 5 */
#define RTL8366RB_PAACR_SPEED_10M	0
#define RTL8366RB_PAACR_SPEED_100M	1
#define RTL8366RB_PAACR_SPEED_1000M	2
#define RTL8366RB_PAACR_FULL_DUPLEX	BIT(2)
#define RTL8366RB_PAACR_LINK_UP		BIT(4)
#define RTL8366RB_PAACR_TX_PAUSE	BIT(5)
#define RTL8366RB_PAACR_RX_PAUSE	BIT(6)
#define RTL8366RB_PAACR_AN		BIT(7)

#define RTL8366RB_PAACR_CPU_PORT	(RTL8366RB_PAACR_SPEED_1000M | \
					 RTL8366RB_PAACR_FULL_DUPLEX | \
					 RTL8366RB_PAACR_LINK_UP | \
					 RTL8366RB_PAACR_TX_PAUSE | \
					 RTL8366RB_PAACR_RX_PAUSE)

#define RTL8366RB_PSTAT0		0x14 /* bits 0..7 = port 0, bits 8..15 = port 1 */
#define RTL8366RB_PSTAT1		0x15 /* bits 0..7 = port 2, bits 8..15 = port 3 */
#define RTL8366RB_PSTAT2		0x16 /* bits 0..7 = port 4, bits 8..15 = port 5 */

#define RTL8366RB_POWER_SAVING_REG	0x21

/* CPU port control reg */
#define RTL8368RB_CPU_CTRL_REG		0x0061
#define RTL8368RB_CPU_PORTS_OFF		0
#define RTL8368RB_CPU_PORTS_MSK		0x00FF
#define RTL8368RB_CPU_INSTAG		BIT(15) /* enables inserting custom tag length/type 8899 */

#define RTL8366RB_SMAR0			0x0070 /* bits 0..15 */
#define RTL8366RB_SMAR1			0x0071 /* bits 16..31 */
#define RTL8366RB_SMAR2			0x0072 /* bits 32..47 */

/* Switch Global Configuration register */
#define RTL8366RB_SGCR				0x0000
#define RTL8366RB_SGCR_EN_BC_STORM_CTRL		BIT(0)
#define RTL8366RB_SGCR_MAX_LENGTH(_x)		(_x << 4)
#define RTL8366RB_SGCR_MAX_LENGTH_MASK		RTL8366RB_SGCR_MAX_LENGTH(0x3)
#define RTL8366RB_SGCR_MAX_LENGTH_1522		RTL8366RB_SGCR_MAX_LENGTH(0x0)
#define RTL8366RB_SGCR_MAX_LENGTH_1536		RTL8366RB_SGCR_MAX_LENGTH(0x1)
#define RTL8366RB_SGCR_MAX_LENGTH_1552		RTL8366RB_SGCR_MAX_LENGTH(0x2)
#define RTL8366RB_SGCR_MAX_LENGTH_9216		RTL8366RB_SGCR_MAX_LENGTH(0x3)
#define RTL8366RB_SGCR_EN_VLAN			BIT(13)
#define RTL8366RB_SGCR_EN_VLAN_4KTB		BIT(14)

/* Port Enable Control register */
#define RTL8366RB_PECR				0x0001

/* Port Mirror Control Register */
#define RTL8366RB_PMCR				0x0007
#define RTL8366RB_PMCR_SOURCE_PORT(_x)		(_x)
#define RTL8366RB_PMCR_SOURCE_PORT_MASK		0x000f
#define RTL8366RB_PMCR_MONITOR_PORT(_x)		((_x) << 4)
#define RTL8366RB_PMCR_MONITOR_PORT_MASK	0x00f0
#define RTL8366RB_PMCR_MIRROR_RX		BIT(8)
#define RTL8366RB_PMCR_MIRROR_TX		BIT(9)
#define RTL8366RB_PMCR_MIRROR_SPC		BIT(10)
#define RTL8366RB_PMCR_MIRROR_ISO		BIT(11)

/* Switch Security Control registers */
#define RTL8366RB_SSCR0				0x0002
#define RTL8366RB_SSCR1				0x0003
#define RTL8366RB_SSCR2				0x0004
#define RTL8366RB_SSCR2_DROP_UNKNOWN_DA		BIT(0)

#define RTL8366RB_RESET_CTRL_REG		0x0100
#define RTL8366RB_CHIP_CTRL_RESET_HW		1
#define RTL8366RB_CHIP_CTRL_RESET_SW		(1 << 1)

#define RTL8366RB_CHIP_ID_REG			0x0509
#define RTL8366RB_CHIP_ID_8366			0x5937
#define RTL8366RB_CHIP_VERSION_CTRL_REG		0x050A
#define RTL8366RB_CHIP_VERSION_MASK		0xf

/* PHY registers control */
#define RTL8366RB_PHY_ACCESS_CTRL_REG		0x8000
#define RTL8366RB_PHY_CTRL_READ			BIT(0)
#define RTL8366RB_PHY_CTRL_WRITE		0
#define RTL8366RB_PHY_ACCESS_BUSY_REG		0x8001
#define RTL8366RB_PHY_INT_BUSY			BIT(0)
#define RTL8366RB_PHY_EXT_BUSY			BIT(4)
#define RTL8366RB_PHY_ACCESS_DATA_REG		0x8002
#define RTL8366RB_PHY_EXT_CTRL_REG		0x8010
#define RTL8366RB_PHY_EXT_WRDATA_REG		0x8011
#define RTL8366RB_PHY_EXT_RDDATA_REG		0x8012

#define RTL8366RB_PHY_REG_MASK			0x1f
#define RTL8366RB_PHY_PAGE_OFFSET		5
#define RTL8366RB_PHY_PAGE_MASK			(0xf << 5)
#define RTL8366RB_PHY_NO_OFFSET			9
#define RTL8366RB_PHY_NO_MASK			(0x1f << 9)

#define RTL8366RB_VLAN_INGRESS_CTRL2_REG	0x037f

/* LED control registers */
#define RTL8366RB_LED_BLINKRATE_REG		0x0430
#define RTL8366RB_LED_BLINKRATE_BIT		0
#define RTL8366RB_LED_BLINKRATE_MASK		0x0007

#define RTL8366RB_LED_CTRL_REG			0x0431
#define RTL8366RB_LED_0_1_CTRL_REG		0x0432
#define RTL8366RB_LED_2_3_CTRL_REG		0x0433

#define RTL8366RB_MIB_COUNT			33
#define RTL8366RB_GLOBAL_MIB_COUNT		1
#define RTL8366RB_MIB_COUNTER_PORT_OFFSET	0x0050
#define RTL8366RB_MIB_COUNTER_BASE		0x1000
#define RTL8366RB_MIB_CTRL_REG			0x13F0
#define RTL8366RB_MIB_CTRL_USER_MASK		0x0FFC
#define RTL8366RB_MIB_CTRL_BUSY_MASK		BIT(0)
#define RTL8366RB_MIB_CTRL_RESET_MASK		BIT(1)
#define RTL8366RB_MIB_CTRL_PORT_RESET(_p)	BIT(2 + (_p))
#define RTL8366RB_MIB_CTRL_GLOBAL_RESET		BIT(11)

#define RTL8366RB_PORT_VLAN_CTRL_BASE		0x0063
#define RTL8366RB_PORT_VLAN_CTRL_REG(_p)  \
		(RTL8366RB_PORT_VLAN_CTRL_BASE + (_p) / 4)
#define RTL8366RB_PORT_VLAN_CTRL_MASK		0xf
#define RTL8366RB_PORT_VLAN_CTRL_SHIFT(_p)	(4 * ((_p) % 4))

#define RTL8366RB_VLAN_TABLE_READ_BASE		0x018C
#define RTL8366RB_VLAN_TABLE_WRITE_BASE		0x0185

#define RTL8366RB_TABLE_ACCESS_CTRL_REG		0x0180
#define RTL8366RB_TABLE_VLAN_READ_CTRL		0x0E01
#define RTL8366RB_TABLE_VLAN_WRITE_CTRL		0x0F01

#define RTL8366RB_VLAN_MC_BASE(_x)		(0x0020 + (_x) * 3)

#define RTL8366RB_PORT_LINK_STATUS_BASE		0x0014
#define RTL8366RB_PORT_STATUS_SPEED_MASK	0x0003
#define RTL8366RB_PORT_STATUS_DUPLEX_MASK	0x0004
#define RTL8366RB_PORT_STATUS_LINK_MASK		0x0010
#define RTL8366RB_PORT_STATUS_TXPAUSE_MASK	0x0020
#define RTL8366RB_PORT_STATUS_RXPAUSE_MASK	0x0040
#define RTL8366RB_PORT_STATUS_AN_MASK		0x0080

#define RTL8366RB_NUM_VLANS		16
#define RTL8366RB_NUM_LEDGROUPS		4
#define RTL8366RB_NUM_VIDS		4096
#define RTL8366RB_PRIORITYMAX		7
#define RTL8366RB_FIDMAX		7

#define RTL8366RB_PORT_1		(1 << 0) /* In userspace port 0 */
#define RTL8366RB_PORT_2		(1 << 1) /* In userspace port 1 */
#define RTL8366RB_PORT_3		(1 << 2) /* In userspace port 2 */
#define RTL8366RB_PORT_4		(1 << 3) /* In userspace port 3 */
#define RTL8366RB_PORT_5		(1 << 4) /* In userspace port 4 */

#define RTL8366RB_PORT_CPU		(1 << 5) /* CPU port */

#define RTL8366RB_PORT_ALL		(RTL8366RB_PORT_1 |	\
					 RTL8366RB_PORT_2 |	\
					 RTL8366RB_PORT_3 |	\
					 RTL8366RB_PORT_4 |	\
					 RTL8366RB_PORT_5 |	\
					 RTL8366RB_PORT_CPU)

#define RTL8366RB_PORT_ALL_BUT_CPU	(RTL8366RB_PORT_1 |	\
					 RTL8366RB_PORT_2 |	\
					 RTL8366RB_PORT_3 |	\
					 RTL8366RB_PORT_4 |	\
					 RTL8366RB_PORT_5)

#define RTL8366RB_PORT_ALL_EXTERNAL	(RTL8366RB_PORT_1 |	\
					 RTL8366RB_PORT_2 |	\
					 RTL8366RB_PORT_3 |	\
					 RTL8366RB_PORT_4)

#define RTL8366RB_PORT_ALL_INTERNAL	 RTL8366RB_PORT_CPU

#define RTL8366RB_VLAN_VID_MASK		0xfff
#define RTL8366RB_VLAN_PRIORITY_SHIFT	12
#define RTL8366RB_VLAN_PRIORITY_MASK	0x7
#define RTL8366RB_VLAN_UNTAG_SHIFT	8
#define RTL8366RB_VLAN_UNTAG_MASK	0xff
#define RTL8366RB_VLAN_MEMBER_MASK	0xff
#define RTL8366RB_VLAN_FID_MASK		0x7


/* Port ingress bandwidth control */
#define RTL8366RB_IB_BASE		0x0200
#define RTL8366RB_IB_REG(pnum)		(RTL8366RB_IB_BASE + pnum)
#define RTL8366RB_IB_BDTH_MASK		0x3fff
#define RTL8366RB_IB_PREIFG_OFFSET	14
#define RTL8366RB_IB_PREIFG_MASK	(1 << RTL8366RB_IB_PREIFG_OFFSET)

/* Port egress bandwidth control */
#define RTL8366RB_EB_BASE		0x02d1
#define RTL8366RB_EB_REG(pnum)		(RTL8366RB_EB_BASE + pnum)
#define RTL8366RB_EB_BDTH_MASK		0x3fff
#define RTL8366RB_EB_PREIFG_REG	0x02f8
#define RTL8366RB_EB_PREIFG_OFFSET	9
#define RTL8366RB_EB_PREIFG_MASK	(1 << RTL8366RB_EB_PREIFG_OFFSET)

#define RTL8366RB_BDTH_SW_MAX		1048512
#define RTL8366RB_BDTH_UNIT		64
#define RTL8366RB_BDTH_REG_DEFAULT	16383

/* QOS */
#define RTL8366RB_QOS_BIT		15
#define RTL8366RB_QOS_MASK		(1 << RTL8366RB_QOS_BIT)
/* Include/Exclude Preamble and IFG (20 bytes). 0:Exclude, 1:Include. */
#define RTL8366RB_QOS_DEFAULT_PREIFG	1

/* Interrupt handling */
#define RTL8366RB_INTERRUPT_CONTROL_REG	0x0440
#define RTL8366RB_INTERRUPT_POLARITY	BIT(0)
#define RTL8366RB_INTERRUPT_MASK_REG	0x0441
#define RTL8366RB_INTERRUPT_LINK_CHGALL	GENMASK(11, 0)
#define RTL8366RB_INTERRUPT_ACLEXCEED	BIT(8)
#define RTL8366RB_INTERRUPT_STORMEXCEED	BIT(9)
#define RTL8366RB_INTERRUPT_P4_FIBER	BIT(12)
#define RTL8366RB_INTERRUPT_P4_UTP	BIT(13)
#define RTL8366RB_INTERRUPT_VALID	(RTL8366RB_INTERRUPT_LINK_CHGALL | \
					 RTL8366RB_INTERRUPT_ACLEXCEED | \
					 RTL8366RB_INTERRUPT_STORMEXCEED | \
					 RTL8366RB_INTERRUPT_P4_FIBER | \
					 RTL8366RB_INTERRUPT_P4_UTP)
#define RTL8366RB_INTERRUPT_STATUS_REG	0x0442

/* bits 0..5 enable force when cleared */
#define RTL8366RB_MAC_FORCE_CTRL_REG	0x0F11

#define RTL8366RB_GREEN_FEATURE_REG	0x0F51
#define RTL8366RB_GREEN_FEATURE_MSK	0x0007
#define RTL8366RB_GREEN_FEATURE_TX	BIT(0)
#define RTL8366RB_GREEN_FEATURE_RX	BIT(2)

static struct rtl8366_mib_counter rtl8366rb_mib_counters[] = {
	{ 0,  0, 4, "IfInOctets"				},
	{ 0,  4, 4, "EtherStatsOctets"				},
	{ 0,  8, 2, "EtherStatsUnderSizePkts"			},
	{ 0, 10, 2, "EtherFragments"				},
	{ 0, 12, 2, "EtherStatsPkts64Octets"			},
	{ 0, 14, 2, "EtherStatsPkts65to127Octets"		},
	{ 0, 16, 2, "EtherStatsPkts128to255Octets"		},
	{ 0, 18, 2, "EtherStatsPkts256to511Octets"		},
	{ 0, 20, 2, "EtherStatsPkts512to1023Octets"		},
	{ 0, 22, 2, "EtherStatsPkts1024to1518Octets"		},
	{ 0, 24, 2, "EtherOversizeStats"			},
	{ 0, 26, 2, "EtherStatsJabbers"				},
	{ 0, 28, 2, "IfInUcastPkts"				},
	{ 0, 30, 2, "EtherStatsMulticastPkts"			},
	{ 0, 32, 2, "EtherStatsBroadcastPkts"			},
	{ 0, 34, 2, "EtherStatsDropEvents"			},
	{ 0, 36, 2, "Dot3StatsFCSErrors"			},
	{ 0, 38, 2, "Dot3StatsSymbolErrors"			},
	{ 0, 40, 2, "Dot3InPauseFrames"				},
	{ 0, 42, 2, "Dot3ControlInUnknownOpcodes"		},
	{ 0, 44, 4, "IfOutOctets"				},
	{ 0, 48, 2, "Dot3StatsSingleCollisionFrames"		},
	{ 0, 50, 2, "Dot3StatMultipleCollisionFrames"		},
	{ 0, 52, 2, "Dot3sDeferredTransmissions"		},
	{ 0, 54, 2, "Dot3StatsLateCollisions"			},
	{ 0, 56, 2, "EtherStatsCollisions"			},
	{ 0, 58, 2, "Dot3StatsExcessiveCollisions"		},
	{ 0, 60, 2, "Dot3OutPauseFrames"			},
	{ 0, 62, 2, "Dot1dBasePortDelayExceededDiscards"	},
	{ 0, 64, 2, "Dot1dTpPortInDiscards"			},
	{ 0, 66, 2, "IfOutUcastPkts"				},
	{ 0, 68, 2, "IfOutMulticastPkts"			},
	{ 0, 70, 2, "IfOutBroadcastPkts"			},
};

static int rtl8366rb_get_mib_counter(struct realtek_smi *smi,
				     int port,
				     struct rtl8366_mib_counter *mib,
				     u64 *mibvalue)
{
	u32 addr, val;
	int ret;
	int i;

	addr = RTL8366RB_MIB_COUNTER_BASE +
		RTL8366RB_MIB_COUNTER_PORT_OFFSET * (port) +
		mib->offset;

	/*
	 * Writing access counter address first
	 * then ASIC will prepare 64bits counter wait for being retrived
	 */
	ret = regmap_write(smi->map, addr, 0); /* Write whatever */
	if (ret)
		return ret;

	/* Read MIB control register */
	ret = regmap_read(smi->map, RTL8366RB_MIB_CTRL_REG, &val);
	if (ret)
		return -EIO;

	if (val & RTL8366RB_MIB_CTRL_BUSY_MASK)
		return -EBUSY;

	if (val & RTL8366RB_MIB_CTRL_RESET_MASK)
		return -EIO;

	/*
	 * Read each individual MIB 16 bits at the time
	 */
	*mibvalue = 0;
	for (i = mib->length; i > 0; i--) {
		ret = regmap_read(smi->map, addr + (i - 1), &val);
		if (ret)
			return ret;
		*mibvalue = (*mibvalue << 16) | (val & 0xFFFF);
	}
	return 0;
}

static u32 rtl8366rb_get_irqmask(struct irq_data *d)
{
	int line = irqd_to_hwirq(d);
	u32 val;

	/*
	 * For line interrupts we combine link down in bits
	 * 6..11 with link up in bits 0..5 into one interrupt.
	 */
	if (line < 12)
		val = BIT(line) | BIT(line+6);
	else
		val = BIT(line);
	return val;
}

static void rtl8366rb_mask_irq(struct irq_data *d)
{
	struct realtek_smi *smi = irq_data_get_irq_chip_data(d);
	int ret;

	ret = regmap_update_bits(smi->map, RTL8366RB_INTERRUPT_MASK_REG,
				 rtl8366rb_get_irqmask(d), 0);
	if (ret)
		dev_err(smi->dev, "could not mask IRQ\n");
}

static void rtl8366rb_unmask_irq(struct irq_data *d)
{
        struct realtek_smi *smi = irq_data_get_irq_chip_data(d);
	int ret;

	ret = regmap_update_bits(smi->map, RTL8366RB_INTERRUPT_MASK_REG,
				 rtl8366rb_get_irqmask(d),
				 rtl8366rb_get_irqmask(d));
	if (ret)
		dev_err(smi->dev, "could not unmask IRQ\n");
}

static irqreturn_t rtl8366rb_irq(int irq, void *data)
{
	struct realtek_smi *smi = data;
	u32 stat;
	int ret;

	/* This clears the IRQ status register */
	ret = regmap_read(smi->map, RTL8366RB_INTERRUPT_STATUS_REG,
			  &stat);
	if (ret) {
		dev_err(smi->dev, "can't read interrupt status\n");
		return IRQ_NONE;
	}
	stat &= RTL8366RB_INTERRUPT_VALID;
	if (!stat)
		return IRQ_NONE;
	while (stat) {
		int line = __ffs(stat);
		int child_irq;

		stat &= ~BIT(line);
		/*
		 * For line interrupts we combine link down in bits
		 * 6..11 with link up in bits 0..5 into one interrupt.
		 */
		if (line < 12 && line > 5)
			line -= 5;
		child_irq = irq_find_mapping(smi->irqdomain, line);
		handle_nested_irq(child_irq);
	}
	return IRQ_HANDLED;
}

static struct irq_chip rtl8366rb_irq_chip = {
	.name = "RTL8366RB",
	.irq_mask = rtl8366rb_mask_irq,
	.irq_unmask = rtl8366rb_unmask_irq,
};

static int rtl8366rb_irq_map(struct irq_domain *domain, unsigned int irq,
			     irq_hw_number_t hwirq)
{
	irq_set_chip_data(irq, domain->host_data);
	irq_set_chip_and_handler(irq, &rtl8366rb_irq_chip, handle_simple_irq);
	irq_set_nested_thread(irq, 1);
	irq_set_noprobe(irq);

	return 0;
}

static void rtl8366rb_irq_unmap(struct irq_domain *d, unsigned int irq)
{
	irq_set_nested_thread(irq, 0);
	irq_set_chip_and_handler(irq, NULL, NULL);
	irq_set_chip_data(irq, NULL);
}

static const struct irq_domain_ops rtl8366rb_irqdomain_ops = {
	.map = rtl8366rb_irq_map,
	.unmap = rtl8366rb_irq_unmap,
	.xlate  = irq_domain_xlate_onecell,
};

static int rtl8366rb_setup_cascaded_irq(struct realtek_smi *smi)
{
	struct device_node *intc = of_get_child_by_name(smi->dev->of_node,
							"interrupt-controller");
	unsigned long irq_trig;
	int irq;
	int ret;
	u32 val;
	int i;

	if (!intc) {
		dev_err(smi->dev, "missing child interrupt-controller node\n");
		return -EINVAL;
	}
	/* RB8366RB IRQs cascade off this one */
	irq = of_irq_get(intc, 0);
	if (irq <= 0) {
		dev_err(smi->dev, "failed to get parent IRQ\n");
		return irq ?: -EINVAL;
	}

	/* This clears the IRQ status register */
	ret = regmap_read(smi->map, RTL8366RB_INTERRUPT_STATUS_REG,
			  &val);
	if (ret) {
		dev_err(smi->dev, "can't read interrupt status\n");
		return ret;
	}

	/* Fetch IRQ edge information from the descriptor */
	irq_trig = irqd_get_trigger_type(irq_get_irq_data(irq));
	switch (irq_trig) {
	case IRQF_TRIGGER_RISING:
	case IRQF_TRIGGER_HIGH:
		dev_info(smi->dev, "active high/rising IRQ\n");
		val = 0;
		break;
	case IRQF_TRIGGER_FALLING:
	case IRQF_TRIGGER_LOW:
		dev_info(smi->dev, "active low/falling IRQ\n");
		val = RTL8366RB_INTERRUPT_POLARITY;
		break;
	};
	ret = regmap_update_bits(smi->map, RTL8366RB_INTERRUPT_CONTROL_REG,
				 RTL8366RB_INTERRUPT_POLARITY,
				 val);
	if (ret) {
		dev_err(smi->dev, "could not configure IRQ polarity\n");
		return ret;
	}

	ret = devm_request_threaded_irq(smi->dev, irq, NULL,
					rtl8366rb_irq, IRQF_ONESHOT,
					"RTL8366RB", smi);
	if (ret) {
		dev_err(smi->dev, "unable to request irq: %d\n", ret);
		return ret;
	}
	smi->irqdomain = irq_domain_add_linear(intc, smi->num_ports,
					       &rtl8366rb_irqdomain_ops, smi);
	if (!smi->irqdomain) {
		dev_err(smi->dev, "failed to create IRQ domain\n");
		return -EINVAL;
	}
	for (i = 0; i < smi->num_ports; i++)
		irq_set_parent(irq_create_mapping(smi->irqdomain, i), irq);

	return 0;
}

/*
 * This jam table activates "green ethernet", which means low power mode
 * and is claimed to detect the cable length and not use more power than
 * necessary, and the ports should enter power saving mode 10 seconds after
 * a cable is disconnected.
 */
static const u32 rtl8366rb_green_jam[][2] = {
	{0xBE78, 0x323C},
	{0xBE77, 0x5000},
	{0xBE2E, 0x7BA7},
	{0xBE59, 0x3459},
	{0xBE5A, 0x745A},
	{0xBE5B, 0x785C},
	{0xBE5C, 0x785C},
	{0xBE6E, 0xE120},
	{0xBE79, 0x323C},
};

static int rtl8366rb_setup(struct dsa_switch *ds)
{
	struct realtek_smi *smi = ds->priv;
	u32 chip_id = 0;
	u32 chip_ver = 0;
	u32 val;
	int ret;
	int i;

	ret = regmap_read(smi->map, RTL8366RB_CHIP_ID_REG, &chip_id);
	if (ret) {
		dev_err(smi->dev, "unable to read chip id\n");
		return ret;
	}

	switch (chip_id) {
	case RTL8366RB_CHIP_ID_8366:
		break;
	default:
		dev_err(smi->dev, "unknown chip id (%04x)\n", chip_id);
		return -ENODEV;
	}

	ret = regmap_read(smi->map, RTL8366RB_CHIP_VERSION_CTRL_REG,
			  &chip_ver);
	if (ret) {
		dev_err(smi->dev, "unable to read chip version\n");
		return ret;
	}

	dev_info(smi->dev, "RTL%04x ver %u chip found\n",
		 chip_id, chip_ver & RTL8366RB_CHIP_VERSION_MASK);

	/* Set up the "green ethernet" feature */
	i = 0;
	while (i < ARRAY_SIZE(rtl8366rb_green_jam)) {
		ret = regmap_read(smi->map, RTL8366RB_PHY_ACCESS_BUSY_REG,
				  &val);
		if (ret)
			return ret;
		if (!(val & RTL8366RB_PHY_INT_BUSY)) {
			ret = regmap_write(smi->map,
					   RTL8366RB_PHY_ACCESS_CTRL_REG,
					   RTL8366RB_PHY_CTRL_WRITE);
			if (ret)
				return ret;
			ret = regmap_write(smi->map,
					   rtl8366rb_green_jam[i][0],
					   rtl8366rb_green_jam[i][1]);
			if (ret)
				return ret;
			i++;
		}
	}
	ret = regmap_write(smi->map,
			   RTL8366RB_GREEN_FEATURE_REG,
			   (chip_ver == 1) ? 0x0007 : 0x0003);
	if (ret)
		return ret;
	/*
	 * The RTL8366RB PHY driver will set up the PHY registers for power
	 * saving mode.
	 */

	/* Force the fixed CPU port into 1Gbit mode, no autonegotiation */
	ret = regmap_update_bits(smi->map, RTL8366RB_MAC_FORCE_CTRL_REG,
				 BIT(5), 0);
	if (ret)
		return ret;

	ret = regmap_update_bits(smi->map, RTL8366RB_PAACR2,
				 0xFF00U,
				 RTL8366RB_PAACR_CPU_PORT << 8);
	if (ret)
		return ret;

	ret = regmap_update_bits(smi->map, RTL8366RB_MAC_FORCE_CTRL_REG,
				 BIT(5), BIT(5));
	if (ret)
		return ret;

	/* Vendor driver sets 0x240 in registers 0xc and 0xd (undocumented) */
	ret = regmap_write(smi->map, 0x0c, 0x240);
	if (ret)
		return ret;
	ret = regmap_write(smi->map, 0x0d, 0x240);
	if (ret)
		return ret;

	/*
	 * FIXME: this disables inserting the custom tag, enable this when we
	 * support the custom tag in net/dsa.
	 */
	ret = regmap_update_bits(smi->map, RTL8368RB_CPU_CTRL_REG,
				 0xFFFF, BIT(smi->cpu_port));

	/* Make sure we default-enable the fixed CPU port */
	ret = regmap_update_bits(smi->map, RTL8366RB_PECR,
				 BIT(smi->cpu_port),
				 0);

	/* set maximum packet length to 1536 bytes */
	ret = regmap_update_bits(smi->map, RTL8366RB_SGCR,
				 RTL8366RB_SGCR_MAX_LENGTH_MASK,
				 RTL8366RB_SGCR_MAX_LENGTH_1536);
	if (ret)
		return ret;

	/* enable learning for all ports */
	ret = regmap_write(smi->map, RTL8366RB_SSCR0, 0);
	if (ret)
		return ret;

	/* enable auto ageing for all ports */
	ret = regmap_write(smi->map, RTL8366RB_SSCR1, 0);
	if (ret)
		return ret;

	/*
	 * discard VLAN tagged packets if the port is not a member of
	 * the VLAN with which the packets is associated.
	 */
	ret = regmap_write(smi->map, RTL8366RB_VLAN_INGRESS_CTRL2_REG,
			   RTL8366RB_PORT_ALL);
	if (ret)
		return ret;

	/* don't drop packets whose DA has not been learned */
	ret = regmap_update_bits(smi->map, RTL8366RB_SSCR2,
				 RTL8366RB_SSCR2_DROP_UNKNOWN_DA, 0);
	if (ret)
		return ret;

	/* Issues reset_vlan(), enable_vlan(true) */
	ret = rtl8366_init_vlan(smi);
	if (ret)
		return ret;

	ret = rtl8366rb_setup_cascaded_irq(smi);
	if (ret)
		dev_info(smi->dev, "no interrupt support\n");

	return 0;
}

static int rtl8366rb_set_addr(struct dsa_switch *ds, u8 *addr)
{
	struct realtek_smi *smi = ds->priv;
	u16 val;
	int ret;

	dev_info(smi->dev, "set MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
		 addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
	val = addr[0] << 8 | addr[1];
	ret = regmap_write(smi->map, RTL8366RB_SMAR0, val);
	if (ret)
		return ret;
	val = addr[2] << 8 | addr[3];
	ret = regmap_write(smi->map, RTL8366RB_SMAR1, val);
	if (ret)
		return ret;
	val = addr[4] << 8 | addr[5];
	ret = regmap_write(smi->map, RTL8366RB_SMAR2, val);
	if (ret)
		return ret;

	return 0;
}

static int rtl8366rb_phy_read(struct dsa_switch *ds, int phy, int regnum)
{
	struct realtek_smi *smi = ds->priv;
	u32 val;
	u32 reg;
	int ret;

	if (phy > RTL8366RB_PHY_NO_MAX)
		return -EINVAL;

	ret = regmap_write(smi->map, RTL8366RB_PHY_ACCESS_CTRL_REG,
			   RTL8366RB_PHY_CTRL_READ);
	if (ret)
		return ret;

	reg = 0x8000 | (1 << (phy + RTL8366RB_PHY_NO_OFFSET)) | regnum;

	ret = regmap_write(smi->map, reg, 0);
	if (ret) {
		dev_err(smi->dev,
			"failed to write PHY%d reg %04x @ %04x, ret %d\n",
			phy, regnum, reg, ret);
		return ret;
	}

	ret = regmap_read(smi->map, RTL8366RB_PHY_ACCESS_DATA_REG, &val);
	if (ret)
		return ret;

	dev_dbg(smi->dev, "read PHY%d register 0x%04x @ %08x, val <- %04x\n",
		phy, regnum, reg, val);

	return val;
}

static int rtl8366rb_phy_write(struct dsa_switch *ds, int phy, int regnum,
			       u16 val)
{
	struct realtek_smi *smi = ds->priv;
	u32 reg;
	int ret;

	if (phy > RTL8366RB_PHY_NO_MAX)
		return -EINVAL;

	ret = regmap_write(smi->map, RTL8366RB_PHY_ACCESS_CTRL_REG,
			   RTL8366RB_PHY_CTRL_WRITE);
	if (ret)
		return ret;

	reg = 0x8000 | (1 << (phy + RTL8366RB_PHY_NO_OFFSET)) | regnum;

	dev_dbg(smi->dev, "write PHY%d register 0x%04x @ %04x, val -> %04x\n",
		phy, regnum, reg, val);

	ret = regmap_write(smi->map, reg, val);
	if (ret)
		return ret;

	return 0;
}

static enum dsa_tag_protocol rtl8366_get_tag_protocol(struct dsa_switch *ds)
{
	/* FIXME: implement the right tagging protocol */
	return DSA_TAG_PROTO_NONE;
}

static void rtl8366rb_adjust_link(struct dsa_switch *ds, int port,
				  struct phy_device *phydev)
{
	struct realtek_smi *smi = ds->priv;

	if (port == smi->cpu_port) {
		dev_info(smi->dev, "adjust link on CPU port\n");
	}
}

static int
rtl8366rb_port_enable(struct dsa_switch *ds, int port,
		      struct phy_device *phy)
{
	struct realtek_smi *smi = ds->priv;

	dev_info(smi->dev, "enable port %d\n", port);
	return regmap_update_bits(smi->map, RTL8366RB_PECR, BIT(port),
				  0);

}

static void
rtl8366rb_port_disable(struct dsa_switch *ds, int port,
		       struct phy_device *phy)
{
	struct realtek_smi *smi = ds->priv;

	dev_info(smi->dev, "disable port %d\n", port);
	regmap_update_bits(smi->map, RTL8366RB_PECR, BIT(port),
			   BIT(port));
}

static int rtl8366rb_get_vlan_4k(struct realtek_smi *smi, u32 vid,
				 struct rtl8366_vlan_4k *vlan4k)
{
	u32 data[3];
	int ret;
	int i;

	memset(vlan4k, '\0', sizeof(struct rtl8366_vlan_4k));

	if (vid >= RTL8366RB_NUM_VIDS)
		return -EINVAL;

	/* write VID */
	ret = regmap_write(smi->map, RTL8366RB_VLAN_TABLE_WRITE_BASE,
			   vid & RTL8366RB_VLAN_VID_MASK);
	if (ret)
		return ret;

	/* write table access control word */
	ret = regmap_write(smi->map, RTL8366RB_TABLE_ACCESS_CTRL_REG,
			   RTL8366RB_TABLE_VLAN_READ_CTRL);
	if (ret)
		return ret;

	for (i = 0; i < 3; i++) {
		ret = regmap_read(smi->map,
				  RTL8366RB_VLAN_TABLE_READ_BASE + i,
				  &data[i]);
		if (ret)
			return ret;
	}

	vlan4k->vid = vid;
	vlan4k->untag = (data[1] >> RTL8366RB_VLAN_UNTAG_SHIFT) &
			RTL8366RB_VLAN_UNTAG_MASK;
	vlan4k->member = data[1] & RTL8366RB_VLAN_MEMBER_MASK;
	vlan4k->fid = data[2] & RTL8366RB_VLAN_FID_MASK;

	return 0;
}

static int rtl8366rb_set_vlan_4k(struct realtek_smi *smi,
				 const struct rtl8366_vlan_4k *vlan4k)
{
	u32 data[3];
	int ret;
	int i;

	if (vlan4k->vid >= RTL8366RB_NUM_VIDS ||
	    vlan4k->member > RTL8366RB_VLAN_MEMBER_MASK ||
	    vlan4k->untag > RTL8366RB_VLAN_UNTAG_MASK ||
	    vlan4k->fid > RTL8366RB_FIDMAX)
		return -EINVAL;

	data[0] = vlan4k->vid & RTL8366RB_VLAN_VID_MASK;
	data[1] = (vlan4k->member & RTL8366RB_VLAN_MEMBER_MASK) |
		  ((vlan4k->untag & RTL8366RB_VLAN_UNTAG_MASK) <<
			RTL8366RB_VLAN_UNTAG_SHIFT);
	data[2] = vlan4k->fid & RTL8366RB_VLAN_FID_MASK;

	for (i = 0; i < 3; i++) {
		ret = regmap_write(smi->map,
					    RTL8366RB_VLAN_TABLE_WRITE_BASE + i,
					    data[i]);
		if (ret)
			return ret;
	}

	/* write table access control word */
	ret = regmap_write(smi->map, RTL8366RB_TABLE_ACCESS_CTRL_REG,
				    RTL8366RB_TABLE_VLAN_WRITE_CTRL);

	return ret;
}

static int rtl8366rb_get_vlan_mc(struct realtek_smi *smi, u32 index,
				 struct rtl8366_vlan_mc *vlanmc)
{
	u32 data[3];
	int ret;
	int i;

	memset(vlanmc, '\0', sizeof(struct rtl8366_vlan_mc));

	if (index >= RTL8366RB_NUM_VLANS)
		return -EINVAL;

	for (i = 0; i < 3; i++) {
		ret = regmap_read(smi->map,
					   RTL8366RB_VLAN_MC_BASE(index) + i,
					   &data[i]);
		if (ret)
			return ret;
	}

	vlanmc->vid = data[0] & RTL8366RB_VLAN_VID_MASK;
	vlanmc->priority = (data[0] >> RTL8366RB_VLAN_PRIORITY_SHIFT) &
			   RTL8366RB_VLAN_PRIORITY_MASK;
	vlanmc->untag = (data[1] >> RTL8366RB_VLAN_UNTAG_SHIFT) &
			RTL8366RB_VLAN_UNTAG_MASK;
	vlanmc->member = data[1] & RTL8366RB_VLAN_MEMBER_MASK;
	vlanmc->fid = data[2] & RTL8366RB_VLAN_FID_MASK;

	return 0;
}

static int rtl8366rb_set_vlan_mc(struct realtek_smi *smi, u32 index,
				 const struct rtl8366_vlan_mc *vlanmc)
{
	u32 data[3];
	int ret;
	int i;

	if (index >= RTL8366RB_NUM_VLANS ||
	    vlanmc->vid >= RTL8366RB_NUM_VIDS ||
	    vlanmc->priority > RTL8366RB_PRIORITYMAX ||
	    vlanmc->member > RTL8366RB_VLAN_MEMBER_MASK ||
	    vlanmc->untag > RTL8366RB_VLAN_UNTAG_MASK ||
	    vlanmc->fid > RTL8366RB_FIDMAX)
		return -EINVAL;

	data[0] = (vlanmc->vid & RTL8366RB_VLAN_VID_MASK) |
		  ((vlanmc->priority & RTL8366RB_VLAN_PRIORITY_MASK) <<
			RTL8366RB_VLAN_PRIORITY_SHIFT);
	data[1] = (vlanmc->member & RTL8366RB_VLAN_MEMBER_MASK) |
		  ((vlanmc->untag & RTL8366RB_VLAN_UNTAG_MASK) <<
			RTL8366RB_VLAN_UNTAG_SHIFT);
	data[2] = vlanmc->fid & RTL8366RB_VLAN_FID_MASK;

	for (i = 0; i < 3; i++) {
		ret = regmap_write(smi->map,
				   RTL8366RB_VLAN_MC_BASE(index) + i,
				   data[i]);
		if (ret)
			return ret;
	}

	return 0;
}

static int rtl8366rb_get_mc_index(struct realtek_smi *smi, int port, int *val)
{
	u32 data;
	int ret;

	if (port >= smi->num_ports)
		return -EINVAL;

	ret = regmap_read(smi->map, RTL8366RB_PORT_VLAN_CTRL_REG(port),
				   &data);
	if (ret)
		return ret;

	*val = (data >> RTL8366RB_PORT_VLAN_CTRL_SHIFT(port)) &
	       RTL8366RB_PORT_VLAN_CTRL_MASK;

	return 0;

}

static int rtl8366rb_set_mc_index(struct realtek_smi *smi, int port, int index)
{
	if (port >= smi->num_ports || index >= RTL8366RB_NUM_VLANS)
		return -EINVAL;

	return regmap_update_bits(smi->map, RTL8366RB_PORT_VLAN_CTRL_REG(port),
				RTL8366RB_PORT_VLAN_CTRL_MASK <<
					RTL8366RB_PORT_VLAN_CTRL_SHIFT(port),
				(index & RTL8366RB_PORT_VLAN_CTRL_MASK) <<
					RTL8366RB_PORT_VLAN_CTRL_SHIFT(port));
}

static bool rtl8366rb_is_vlan_valid(struct realtek_smi *smi, unsigned vlan)
{
	unsigned max = RTL8366RB_NUM_VLANS;

	if (smi->vlan4k_enabled)
		max = RTL8366RB_NUM_VIDS - 1;

	if (vlan == 0 || vlan >= max)
		return 0;

	return 1;
}

static int rtl8366rb_enable_vlan(struct realtek_smi *smi, bool enable)
{
	return regmap_update_bits(smi->map, RTL8366RB_SGCR, RTL8366RB_SGCR_EN_VLAN,
				  enable ? RTL8366RB_SGCR_EN_VLAN : 0);
}

static int rtl8366rb_enable_vlan4k(struct realtek_smi *smi, bool enable)
{
	return regmap_update_bits(smi->map, RTL8366RB_SGCR,
				  RTL8366RB_SGCR_EN_VLAN_4KTB,
				  enable ? RTL8366RB_SGCR_EN_VLAN_4KTB : 0);
}

static int rtl8366rb_reset_chip(struct realtek_smi *smi)
{
	int timeout = 10;
	u32 val;
	int ret;

	realtek_smi_write_reg_noack(smi, RTL8366RB_RESET_CTRL_REG,
				    RTL8366RB_CHIP_CTRL_RESET_HW);
	do {
		msleep(1);
		ret = regmap_read(smi->map, RTL8366RB_RESET_CTRL_REG, &val);
		if (ret)
			return ret;

		if (!(val & RTL8366RB_CHIP_CTRL_RESET_HW))
			break;
	} while (--timeout);

	if (!timeout) {
		dev_err(smi->dev, "timeout waiting for the switch to reset\n");
		return -EIO;
	}

	return 0;
}

static int rtl8366rb_detect(struct realtek_smi *smi)
{
	struct device *dev = smi->dev;
	int ret;
	u32 val;

	/* Detect device */
	ret = regmap_read(smi->map, 0x5c, &val);
	if (ret) {
		dev_err(dev, "can't get chip ID (%d)\n", ret);
		return ret;
	}

	switch(val) {
	case 0x6027:
		dev_info(dev, "found an RTL8366S switch\n");
		dev_err(dev, "this switch is not yet supported, submit patches!\n");
		return -ENODEV;
		break;
	case 0x5937:
		dev_info(dev, "found an RTL8366RB switch\n");
		smi->cpu_port = RTL8366RB_PORT_NUM_CPU;
		smi->num_ports = RTL8366RB_NUM_PORTS;
		smi->num_vlan_mc = RTL8366RB_NUM_VLANS;
		smi->mib_counters = rtl8366rb_mib_counters;
		smi->num_mib_counters = ARRAY_SIZE(rtl8366rb_mib_counters);
		break;
	default:
		dev_info(dev, "found an Unknown Realtek switch (id=0x%04x)\n",
			 val);
		break;
	}

	ret = rtl8366rb_reset_chip(smi);
	if (ret)
		return ret;

	return 0;
}

static const struct dsa_switch_ops rtl8366rb_switch_ops = {
	.get_tag_protocol = rtl8366_get_tag_protocol,
	.setup = rtl8366rb_setup,
	.set_addr = rtl8366rb_set_addr,
	.adjust_link = rtl8366rb_adjust_link,
	.get_strings = rtl8366_get_strings,
	.get_ethtool_stats = rtl8366_get_ethtool_stats,
	.get_sset_count = rtl8366_get_sset_count,
	.port_vlan_filtering = rtl8366_vlan_filtering,
	.port_vlan_prepare = rtl8366_vlan_prepare,
	.port_vlan_add = rtl8366_vlan_add,
	.port_vlan_del = rtl8366_vlan_del,
	.phy_read = rtl8366rb_phy_read,
	.phy_write = rtl8366rb_phy_write,
	.port_enable = rtl8366rb_port_enable,
	.port_disable = rtl8366rb_port_disable,
};

static const struct realtek_smi_ops rtl8366rb_smi_ops = {
	.detect		= rtl8366rb_detect,
	.get_vlan_mc	= rtl8366rb_get_vlan_mc,
	.set_vlan_mc	= rtl8366rb_set_vlan_mc,
	.get_vlan_4k	= rtl8366rb_get_vlan_4k,
	.set_vlan_4k	= rtl8366rb_set_vlan_4k,
	.get_mc_index	= rtl8366rb_get_mc_index,
	.set_mc_index	= rtl8366rb_set_mc_index,
	.get_mib_counter = rtl8366rb_get_mib_counter,
	.is_vlan_valid	= rtl8366rb_is_vlan_valid,
	.enable_vlan	= rtl8366rb_enable_vlan,
	.enable_vlan4k	= rtl8366rb_enable_vlan4k,
};

const struct realtek_smi_variant rtl8366rb_variant = {
	.ds_ops = &rtl8366rb_switch_ops,
	.ops = &rtl8366rb_smi_ops,
	.clk_delay = 10,
	.cmd_read = 0xa9,
	.cmd_write = 0xa8,
};
EXPORT_SYMBOL_GPL(rtl8366rb_variant);
