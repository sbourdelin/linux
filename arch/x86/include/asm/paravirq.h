#ifndef _ASM_X86_PARAVIRQ_H
#define _ASM_X86_PARAVIRQ_H

int paravirq_init_chip(void (*irq_mask)(struct irq_data *data),
				void (*irq_unmask)(struct irq_data *data));
int paravirq_alloc_irq(void);
void paravirq_free_irq(unsigned int irq);

#endif /* _ASM_X86_PARAVIRQ_H */
