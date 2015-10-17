#ifndef __ASM_IRQ_H
#define __ASM_IRQ_H

#ifndef CONFIG_ARM64_64K_PAGES
#define IRQ_STACK_SIZE_ORDER	2
#endif

#define IRQ_STACK_SIZE		16384
#define IRQ_STACK_START_SP	(IRQ_STACK_SIZE - 16)

#ifndef __ASSEMBLY__

#include <linux/gfp.h>
#include <linux/irqchip/arm-gic-acpi.h>
#include <linux/slab.h>

#include <asm-generic/irq.h>

#if IRQ_STACK_SIZE >= PAGE_SIZE
static inline void *__alloc_irq_stack(void)
{
       return (void *)__get_free_pages(THREADINFO_GFP | __GFP_ZERO,
                                       IRQ_STACK_SIZE_ORDER);
}
#else
static inline void *__alloc_irq_stack(void)
{
       return kmalloc(IRQ_STACK_SIZE, THREADINFO_GFP | __GFP_ZERO);
}
#endif

struct pt_regs;

extern void set_handle_irq(void (*handle_irq)(struct pt_regs *));

extern int alloc_irq_stack(unsigned int cpu);

static inline void acpi_irq_init(void)
{
	/*
	 * Hardcode ACPI IRQ chip initialization to GICv2 for now.
	 * Proper irqchip infrastructure will be implemented along with
	 * incoming  GICv2m|GICv3|ITS bits.
	 */
	acpi_gic_init();
}
#define acpi_irq_init acpi_irq_init

#endif
#endif
