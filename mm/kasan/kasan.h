/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __MM_KASAN_KASAN_H
#define __MM_KASAN_KASAN_H

#include <linux/kasan.h>
#include <linux/stackdepot.h>

#define KASAN_SHADOW_SCALE_SIZE (1UL << KASAN_SHADOW_SCALE_SHIFT)
#define KASAN_SHADOW_MASK       (KASAN_SHADOW_SCALE_SIZE - 1)

/* We devide one shadow byte into two parts: "check" and "poison".
 * "check" is a used for advanced check.
 * "poison" is used to store the foot print of the tracked memory,
 * For a paticular address, one extra check is enough. So we can have up to
 * (1 << (KASAN_CHECK_BITS) - 1) - 1 checks. That's 0b001 to 0b110 (0b111 is
 * reserved for poison values)
 *
 * The bits occupition in shadow bytes (P for poison, C for check):
 *
 * |P|C|C|C|P|P|P|P|
 *
 */
#define KASAN_FREE_PAGE         0xFF  /* page was freed */
#define KASAN_PAGE_REDZONE      0xFE  /* redzone for kmalloc_large allocations */
#define KASAN_KMALLOC_REDZONE   0xFC  /* redzone inside slub object */
#define KASAN_KMALLOC_FREE      0xFB  /* object was freed (kmem_cache_free/kfree) */
#define KASAN_GLOBAL_REDZONE    0xFA  /* redzone for global variable */

/*
 * Stack redzone shadow values
 * (Those are compiler's ABI, don't change them)
 */
#define KASAN_STACK_LEFT        0xF1
#define KASAN_STACK_MID         0xF2
#define KASAN_STACK_RIGHT       0xF3
#define KASAN_STACK_PARTIAL     0xF4
#define KASAN_USE_AFTER_SCOPE   0xF8

/* Don't break randconfig/all*config builds */
#ifndef KASAN_ABI_VERSION
#define KASAN_ABI_VERSION 1
#endif

#define KASAN_POISON_MASK	0x8F
#define KASAN_POISON_MASK_16	0x8F8F
#define KASAN_POISON_MASK_64	0x8F8F8F8F8F8F8F8F
#define KASAN_CHECK_MASK	0x70
#define KASAN_CHECK_SHIFT	4
#define KASAN_CHECK_BITS	3

#define KASAN_GET_POISON(val) ((s8)((val) & KASAN_POISON_MASK))
#define KASAN_CHECK_LOWMASK (KASAN_CHECK_MASK >> KASAN_CHECK_SHIFT)
/* 16 bits and 64 bits version */
#define KASAN_GET_POISON_16(val) ((val) & KASAN_POISON_MASK_16)
#define KASAN_GET_POISON_64(val) ((val) & KASAN_POISON_MASK_64)

struct kasan_access_info {
	const void *access_addr;
	const void *first_bad_addr;
	size_t access_size;
	bool is_write;
	unsigned long ip;
};

/* The layout of struct dictated by compiler */
struct kasan_source_location {
	const char *filename;
	int line_no;
	int column_no;
};

/* The layout of struct dictated by compiler */
struct kasan_global {
	const void *beg;		/* Address of the beginning of the global variable. */
	size_t size;			/* Size of the global variable. */
	size_t size_with_redzone;	/* Size of the variable + size of the red zone. 32 bytes aligned */
	const void *name;
	const void *module_name;	/* Name of the module where the global variable is declared. */
	unsigned long has_dynamic_init;	/* This needed for C++ */
#if KASAN_ABI_VERSION >= 4
	struct kasan_source_location *location;
#endif
#if KASAN_ABI_VERSION >= 5
	char *odr_indicator;
#endif
};

/**
 * Structures to keep alloc and free tracks *
 */

#define KASAN_STACK_DEPTH 64

struct kasan_track {
	u32 pid;
	depot_stack_handle_t stack;
};

struct kasan_alloc_meta {
	struct kasan_track alloc_track;
	struct kasan_track free_track;
};

struct qlist_node {
	struct qlist_node *next;
};
struct kasan_free_meta {
	/* This field is used while the object is in the quarantine.
	 * Otherwise it might be used for the allocator freelist.
	 */
	struct qlist_node quarantine_link;
};

struct kasan_adv_check {
	enum kasan_adv_chk_type	ac_type;
	bool			(*ac_check_func)(bool, void *);
	void			*ac_data;
	char			*ac_msg;
	bool			ac_violation;
};

extern struct kasan_adv_check *get_check_by_nr(int nr);

struct kasan_alloc_meta *get_alloc_info(struct kmem_cache *cache,
					const void *object);
struct kasan_free_meta *get_free_info(struct kmem_cache *cache,
					const void *object);

static inline const void *kasan_shadow_to_mem(const void *shadow_addr)
{
	return (void *)(((unsigned long)shadow_addr - KASAN_SHADOW_OFFSET)
		<< KASAN_SHADOW_SCALE_SHIFT);
}

void kasan_report(unsigned long addr, size_t size,
		bool is_write, unsigned long ip);
void kasan_report_double_free(struct kmem_cache *cache, void *object,
					void *ip);

#if defined(CONFIG_SLAB) || defined(CONFIG_SLUB)
void quarantine_put(struct kasan_free_meta *info, struct kmem_cache *cache);
void quarantine_reduce(void);
void quarantine_remove_cache(struct kmem_cache *cache);
#else
static inline void quarantine_put(struct kasan_free_meta *info,
				struct kmem_cache *cache) { }
static inline void quarantine_reduce(void) { }
static inline void quarantine_remove_cache(struct kmem_cache *cache) { }
#endif

static inline u8 kasan_get_check(u8 val)
{
	val &= KASAN_CHECK_MASK;
	val >>= KASAN_CHECK_SHIFT;

	return (val ^ KASAN_CHECK_LOWMASK) ?  val : 0;
}
#endif
