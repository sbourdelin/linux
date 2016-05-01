/*
 * SH IPR-INTC interrupt contoller driver
 *
 * Copyright 2016 Yoshinori Sato <ysato@users.sourceforge.jp>
 */

#include <linux/irq.h>
#include <linux/irqchip.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/of.h>
#include <asm/io.h>

static struct sh7751_intc_regs {
	void *icr;
	void *ipr;
	void *intpri00;
	void *intreq00;
	void *intmsk00;
	void *intmskclr00;
} sh7751_regs;

static const unsigned int ipr_table[] = {
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, /* 0 - 7 */
	0x41, 0xff, 0xff, 0x40, 0xff, 0xff, 0xff, 0xff, /* 8 - 15 */
	0x03, 0x02, 0x01, 0x01, 0x00, 0x00, 0x00, 0x11, /* 16 - 23 */
	0x11, 0x11, 0x11, 0x13, 0x12, 0x12, 0xff, 0xff, /* 24 - 31 */
	0x30, 0x33, 0x32, 0x32, 0x32, 0x32, 0x32, 0x21, /* 32 - 39 */
	0x21, 0x21, 0x21, 0x21, 0x32, 0x32, 0x32, 0x32, /* 40 - 47 */
	0xff, 0xff, 0xff, 0x40, 0xff, 0xff, 0xff, 0xff, /* 48 - 55 */
	0xff, 0xff, 0xff, 0x40, 0xff, 0xff, 0xff, 0xff, /* 56 - 63 */
};

static const unsigned int pri_table[] = {
	0, 4, 4, 4, 4, 4, 4, 4,
	8, 32, 32, 32, 12, 32, 32, 32,
};

static void sh_disable_irq(struct irq_data *data)
{
	int pos;
	unsigned int addr;
	unsigned long pri;
	int irq = data->irq;
	struct sh7751_intc_regs *reg = data->chip_data;

	if (irq < 64) {
		if (ipr_table[irq] != 0xff) {
			addr = (ipr_table[irq] & 0xf0) >> 2;
			pos = (ipr_table[irq] & 0x0f) << 4;
			pri = ~(0x000f << pos);
			pri &= __raw_readw(reg->ipr + addr);
			__raw_writew(pri, reg->ipr + addr);
		}
	} else {
		if (pri_table[irq - 64] < 32) {
			pos = pri_table[irq - 64];
			pri = ~(0x000f << pos);
			pri &= __raw_readw(reg->intpri00);
			__raw_writew(pri, reg->intpri00);
		}
	}
}

static void sh_enable_irq(struct irq_data *data)
{
	int pos;
	unsigned int addr;
	unsigned long pri;
	int irq = data->irq;
	struct sh7751_intc_regs *reg = data->chip_data;

	if (irq < 64) {
		if (ipr_table[irq] != 0xff) {
			addr = (ipr_table[irq] & 0xf0) >> 2;
			pos = (ipr_table[irq] & 0x0f) * 4;
			pri = ~(0x000f << pos);
			pri &= __raw_readw(reg->ipr + addr);
			pri |= 1 << pos;
			__raw_writew(pri, reg->ipr + addr);
		}
	} else {
		if (pri_table[irq - 64] < 32) {
			pos = pri_table[irq - 64];
			pri = ~(0x000f << pos);
			pri &= __raw_readw(reg->intpri00);
			pri |= 1 << pos;
			__raw_writew(pri, reg->intpri00);
		}
	}
}

struct irq_chip sh_irq_chip = {
	.name		= "SH-IPR",
	.irq_unmask	= sh_enable_irq,
	.irq_mask	= sh_disable_irq,
};

static __init int irq_map(struct irq_domain *h, unsigned int virq,
			  irq_hw_number_t hw_irq_num)
{
	irq_set_chip_and_handler(virq, &sh_irq_chip, handle_level_irq);
	irq_get_irq_data(virq)->chip_data = h->host_data;
	irq_modify_status(virq, IRQ_NOREQUEST, IRQ_NOPROBE);

	return 0;
}

static struct irq_domain_ops irq_ops = {
       .map    = irq_map,
       .xlate  = irq_domain_xlate_onecell,
};

static int __init sh_intc_of_init(struct device_node *intc,
				  struct device_node *parent)
{
	struct irq_domain *domain;
	void *intc_baseaddr;
	void *intc_baseaddr2;

	intc_baseaddr = of_iomap(intc, 0);
	intc_baseaddr2 = of_iomap(intc, 1);
	BUG_ON(!intc_baseaddr);

	sh7751_regs.icr = intc_baseaddr;
	sh7751_regs.ipr = intc_baseaddr + 4;
	sh7751_regs.intpri00 = intc_baseaddr2;
	sh7751_regs.intreq00 = intc_baseaddr2 + 0x20;
	sh7751_regs.intmsk00 = intc_baseaddr2 + 0x40;
	sh7751_regs.intmskclr00 = intc_baseaddr2 + 0x60;

	domain = irq_domain_add_linear(intc, NR_IRQS, &irq_ops, &sh7751_regs);
	BUG_ON(!domain);
	irq_set_default_host(domain);
	return 0;
}

IRQCHIP_DECLARE(sh_7751_intc, "renesas,sh7751-intc", sh_intc_of_init);
