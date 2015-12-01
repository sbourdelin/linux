/*
 * Copyright(c) 2015 EZchip Technologies.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * The full GNU General Public License is included in this distribution in
 * the file called "COPYING".
 */

#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/irqdomain.h>
#include <linux/irqchip.h>
#include <asm/irq.h>
#include <plat/mtm.h>

#define NPS_MSU_EN_CFG	0x80	/* MSU Enable Configuration Register */
#define MSU_EN		BIT(0)  /* MSU block enable */
#define IPI_EN		BIT(16) /* Enable service of incoming IPI Messages */
#define GIM0_EN		BIT(17) /* Enable service of incoming GIM 0 messages */
#define GIM1_EN		BIT(18) /* Enable service of incoming GIM 1 messages */

#define NPS_GIM_P_POL	0x110	/* Peripheral interrupts source polarity */
#define NPS_GIM_P_SENS	0x114	/* Peripheral interrupts sensitivity */
#define GIM_UART	BIT(7)
#define GIM_LAN_TX	(BIT(10) | BIT(25))
#define GIM_LAN_RX	(BIT(11) | BIT(26))
#define GIM_PERIPH_ALL	(GIM_UART | GIM_LAN_TX | GIM_LAN_RX)

#define NPS_GIM_P_DST10	0x13A	/* Peripheral Interrupt Destination (LAN RX) */
#define NPS_GIM_P_DST11	0x13B	/* Peripheral Interrupt Destination (LAN TX) */
#define NPS_GIM_P_DST25	0x149	/* Peripheral Interrupt Destination (LAN RX) */
#define NPS_GIM_P_DST26	0x14A	/* Peripheral Interrupt Destination (LAN TX) */
#define DST_IS		BIT(26) /* Interrupt select for line 7 */

#define NPS_GIM_P_EN	0x100	/* Peripheral interrupts source enable */
#define NPS_GIM_P_BLK	0x118	/* Peripheral interrupts blocking for sources */

/* Messaging and Scheduling Unit:
 * Provides message management for a CPU cluster.
 */
static void __init eznps_configure_msu(void)
{
	int cpu;
	u32 value = MSU_EN | IPI_EN | GIM0_EN | GIM1_EN;

	/* Enable IPI and GIM messages on all clusters */
	for (cpu = 0 ; cpu < eznps_max_cpus; cpu += eznps_cpus_per_cluster)
		iowrite32be(value,
			    nps_host_reg(cpu, NPS_MSU_BLKID, NPS_MSU_EN_CFG));
}

/* Global Interrupt Manager:
 * Configures and manages up to 64 interrupts from peripherals,
 * 16 interrupts from CPUs (virtual interrupts) and ECC interrupts.
 * Receives the interrupts and transmits them to relevant CPU.
 */
static void __init eznps_configure_gim(void)
{
	u32 reg_addr, reg_val;

	/* IRQ polarity, low or high level, negative or positive edge */
	reg_addr = nps_host_reg_non_cl(NPS_GIM_BLKID, NPS_GIM_P_POL);
	reg_val = ioread32be(reg_addr);
	reg_val &= ~GIM_PERIPH_ALL;
	iowrite32be(reg_val, reg_addr);

	/* IRQ type level or edge */
	reg_addr = nps_host_reg_non_cl(NPS_GIM_BLKID, NPS_GIM_P_SENS);
	reg_val = ioread32be(reg_addr);
	reg_val |= GIM_LAN_TX;
	iowrite32be(reg_val, reg_addr);

	/* GIM interrupt select type for debug LAN interrupts (both sides) */
	reg_val = DST_IS;
	reg_addr = nps_host_reg_non_cl(NPS_GIM_BLKID, NPS_GIM_P_DST10);
	iowrite32be(reg_val, reg_addr);
	reg_addr = nps_host_reg_non_cl(NPS_GIM_BLKID, NPS_GIM_P_DST11);
	iowrite32be(reg_val, reg_addr);
	reg_addr = nps_host_reg_non_cl(NPS_GIM_BLKID, NPS_GIM_P_DST25);
	iowrite32be(reg_val, reg_addr);
	reg_addr = nps_host_reg_non_cl(NPS_GIM_BLKID, NPS_GIM_P_DST26);
	iowrite32be(reg_val, reg_addr);

	/* CTOP IRQ lines should be defined as blocking in GIM */
	iowrite32be(GIM_PERIPH_ALL,
		    nps_host_reg_non_cl(NPS_GIM_BLKID, NPS_GIM_P_BLK));

	/* Enable CTOP IRQ lines in GIM */
	iowrite32be(GIM_PERIPH_ALL,
		    nps_host_reg_non_cl(NPS_GIM_BLKID, NPS_GIM_P_EN));
}

/*
 * NPS400 core includes a Interrupt Controller (IC) support.
 * All cores can deactivate level irqs at first level control
 * at cores mesh layer called MTM.
 * For devices out side chip e.g. uart, network there is another
 * level called Global Interrupt Manager (GIM).
 * This second level can control level and edge interrupt.
 *
 * NOTE: AUX_IENABLE and CTOP_AUX_IACK are auxiliary registers
 * with private HW copy per CPU.
 */

static void nps400_irq_mask(struct irq_data *data)
{
	unsigned int ienb;

	ienb = read_aux_reg(AUX_IENABLE);
	ienb &= ~(1 << data->hwirq);
	write_aux_reg(AUX_IENABLE, ienb);
}

static void nps400_irq_unmask(struct irq_data *data)
{
	unsigned int ienb;

	ienb = read_aux_reg(AUX_IENABLE);
	ienb |= (1 << data->hwirq);
	write_aux_reg(AUX_IENABLE, ienb);
}

static void nps400_irq_eoi_global(struct irq_data *data)
{
	write_aux_reg(CTOP_AUX_IACK, 1 << data->hwirq);

	/* Don't ack before all device access is done */
	mb();

	__asm__ __volatile__ (
	"       .word %0\n"
	:
	: "i"(CTOP_INST_RSPI_GIC_0_R12)
	: "memory");
}

static void nps400_irq_eoi(struct irq_data *data)
{
	write_aux_reg(CTOP_AUX_IACK, 1 << data->hwirq);
}


static struct irq_chip nps400_irq_chip_fasteoi = {
	.name		= "NPS400 IC Global",
	.irq_mask	= nps400_irq_mask,
	.irq_unmask	= nps400_irq_unmask,
	.irq_eoi	= nps400_irq_eoi_global,
};

static struct irq_chip nps400_irq_chip_percpu = {
	.name		= "NPS400 IC",
	.irq_mask	= nps400_irq_mask,
	.irq_unmask	= nps400_irq_unmask,
	.irq_eoi	= nps400_irq_eoi,
};

static int nps400_irq_map(struct irq_domain *d, unsigned int irq,
			  irq_hw_number_t hw)
{
	switch (irq) {
	case TIMER0_IRQ:
#if defined(CONFIG_SMP)
	case IPI_IRQ:
#endif
		irq_set_chip_and_handler(irq, &nps400_irq_chip_percpu,
					 handle_percpu_irq);
	break;
	default:
		irq_set_chip_and_handler(irq, &nps400_irq_chip_fasteoi,
					 handle_fasteoi_irq);
	break;
	}

	return 0;
}

static const struct irq_domain_ops nps400_irq_ops = {
	.xlate = irq_domain_xlate_onecell,
	.map = nps400_irq_map,
};

static struct irq_domain *nps400_root_domain;

static int __init nps400_of_init(struct device_node *node,
				 struct device_node *parent)
{
	if (parent)
		panic("DeviceTree incore ic not a root irq controller\n");

	eznps_configure_msu();
	eznps_configure_gim();

	nps400_root_domain = irq_domain_add_legacy(node, NR_CPU_IRQS, 0, 0,
						   &nps400_irq_ops, NULL);

	if (!nps400_root_domain)
		panic("nps400 root irq domain not avail\n");

	/* with this we don't need to export nps400_root_domain */
	irq_set_default_host(nps400_root_domain);

	return 0;
}
IRQCHIP_DECLARE(ezchip_nps400_ic, "ezchip,nps400-ic", nps400_of_init);
