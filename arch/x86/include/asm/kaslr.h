#ifndef _ASM_KASLR_H_
#define _ASM_KASLR_H_

unsigned long kaslr_get_random_long(const char *purpose);

#ifdef CONFIG_RANDOMIZE_MEMORY
extern unsigned long page_offset_base;
extern unsigned long vmalloc_base;
extern unsigned long vmemmap_base;

void kernel_randomize_memory(void);
void kernel_randomize_smp(void);
void* kaslr_get_gdt_remap(int cpu);
#else
static inline void kernel_randomize_memory(void) { }
static inline void kernel_randomize_smp(void) { }
static inline void *kaslr_get_gdt_remap(int cpu) { return NULL; }
#endif /* CONFIG_RANDOMIZE_MEMORY */

#endif
