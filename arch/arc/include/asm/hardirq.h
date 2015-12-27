#ifndef __ASM_ARC_HARDIRQ_H
#define __ASM_ARC_HARDIRQ_H

#include <linux/cache.h>
#include <linux/threads.h>

typedef struct {
	unsigned int __softirq_pending;
#ifdef CONFIG_SMP
	unsigned int ipi_irqs;
#endif
} ____cacheline_aligned irq_cpustat_t;

#include <linux/irq_cpustat.h>	/* Standard mappings for irq_cpustat_t above */
#include <linux/irq.h>

static inline void ack_bad_irq(unsigned int irq)
{
	printk(KERN_CRIT "unexpected IRQ trap at vector %02x\n", irq);
}

#endif /* __ASM_ARC_HARDIRQ_H */
