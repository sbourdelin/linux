#ifndef _ASM_KASLR_H_
#define _ASM_KASLR_H_

unsigned long kaslr_get_random_boot_long(void);

#ifdef CONFIG_RANDOMIZE_MEMORY
extern unsigned long page_offset_base;
extern unsigned long vmalloc_base;
extern unsigned long vmemmap_base;

void kernel_randomize_memory(void);
void kaslr_trampoline_init(unsigned long page_size_mask);
#else
static inline void kernel_randomize_memory(void) { }
static inline void kaslr_trampoline_init(unsigned long page_size_mask) { }
#endif /* CONFIG_RANDOMIZE_MEMORY */

#endif
