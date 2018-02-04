/*
 * Basic general purpose allocator for managing special purpose
 * memory, for example, memory that is not managed by the regular
 * kmalloc/kfree interface.  Uses for this includes on-device special
 * memory, uncached memory etc.
 *
 * It is safe to use the allocator in NMI handlers and other special
 * unblockable contexts that could otherwise deadlock on locks.  This
 * is implemented by using atomic operations and retries on any
 * conflicts.  The disadvantage is that there may be livelocks in
 * extreme cases.  For better scalability, one allocator can be used
 * for each CPU.
 *
 * The lockless operation only works if there is enough memory
 * available.  If new memory is added to the pool a lock has to be
 * still taken.  So any user relying on locklessness has to ensure
 * that sufficient memory is preallocated.
 *
 * The basic atomic operation of this allocator is cmpxchg on long.
 * On architectures that don't have NMI-safe cmpxchg implementation,
 * the allocator can NOT be used in NMI handler.  So code uses the
 * allocator in NMI handler should depend on
 * CONFIG_ARCH_HAVE_NMI_SAFE_CMPXCHG.
 *
 * Copyright 2005 (C) Jes Sorensen <jes@trained-monkey.org>
 *
 * This source code is licensed under the GNU General Public License,
 * Version 2.  See the file COPYING for more details.
 */

#include <linux/slab.h>
#include <linux/export.h>
#include <linux/bitmap.h>
#include <linux/rculist.h>
#include <linux/interrupt.h>
#include <linux/genalloc.h>
#include <linux/of_device.h>

#define ENTRY_ORDER 1UL
#define ENTRY_MASK ((1UL << ((ENTRY_ORDER) + 1UL)) - 1UL)
#define ENTRY_HEAD ENTRY_MASK
#define ENTRY_UNUSED 0UL
#define BITS_PER_ENTRY (1U << ENTRY_ORDER)
#define BITS_DIV_ENTRIES(x) ((x) >> ENTRY_ORDER)
#define ENTRIES_TO_BITS(x) ((x) << ENTRY_ORDER)
#define BITS_DIV_LONGS(x) ((x) / BITS_PER_LONG)
#define ENTRIES_DIV_LONGS(x) (BITS_DIV_LONGS(ENTRIES_TO_BITS(x)))

#define ENTRIES_PER_LONG BITS_DIV_ENTRIES(BITS_PER_LONG)

/* Binary pattern of 1010...1010 that spans one unsigned long. */
#define MASK (~0UL / 3 * 2)

/**
 * get_bitmap_entry - extracts the specified entry from the bitmap
 * @map: pointer to a bitmap
 * @entry_index: the index of the desired entry in the bitmap
 *
 * Returns the requested bitmap.
 */
static inline unsigned long get_bitmap_entry(unsigned long *map,
					    int entry_index)
{
	return (map[ENTRIES_DIV_LONGS(entry_index)] >>
		ENTRIES_TO_BITS(entry_index % ENTRIES_PER_LONG)) &
		ENTRY_MASK;
}


/**
 * mem_to_units - convert references to memory into orders of allocation
 * @size: amount in bytes
 * @order: pow of 2 represented by each entry in the bitmap
 *
 * Returns the number of units representing the size.
 */
static inline unsigned long mem_to_units(unsigned long size,
					 unsigned long order)
{
	return (size + (1UL << order) - 1) >> order;
}

/**
 * chunk_size - dimension of a chunk of memory
 * @chunk: pointer to the struct describing the chunk
 *
 * Returns the size of the chunk.
 */
static inline size_t chunk_size(const struct gen_pool_chunk *chunk)
{
	return chunk->end_addr - chunk->start_addr + 1;
}


/**
 * set_bits_ll - according to the mask, sets the bits specified by
 * value, at the address specified.
 * @addr: where to write
 * @mask: filter to apply for the bits to alter
 * @value: actual configuration of bits to store
 *
 * Returns 0 upon success, -EBUSY otherwise
 */
static int set_bits_ll(unsigned long *addr,
		       unsigned long mask, unsigned long value)
{
	unsigned long nval;
	unsigned long present;
	unsigned long target;

	nval = *addr;
	do {
		present = nval;
		if (present & mask)
			return -EBUSY;
		target =  present | value;
		cpu_relax();
	} while ((nval = cmpxchg(addr, present, target)) != target);
	return 0;
}


/**
 * cleart_bits_ll - according to the mask, clears the bits specified by
 * value, at the address specified.
 * @addr: where to write
 * @mask: filter to apply for the bits to alter
 * @value: actual configuration of bits to clear
 *
 * Returns 0 upon success, -EBUSY otherwise
 */
static int clear_bits_ll(unsigned long *addr,
			 unsigned long mask, unsigned long value)
{
	unsigned long nval;
	unsigned long present;
	unsigned long target;

	nval = *addr;
	present = nval;
	if (unlikely((present & mask) ^ value))
		return -EBUSY;
	do {
		present = nval;
		if (unlikely((present & mask) ^ value))
			return -EBUSY;
		target =  present & ~mask;
		cpu_relax();
	} while ((nval = cmpxchg(addr, present, target)) != target);
	return 0;
}


/**
 * get_boundary - verify that an allocation effectively
 * starts at the given address, then measure its length.
 * @map: pointer to a bitmap
 * @start_entry: the index of the first entry in the bitmap
 * @nentries: number of entries to alter
 *
 * Returns the length of an allocation, otherwise -EINVAL if the
 * parameters do not refer to a correct allocation.
 */
static int get_boundary(unsigned long *map, int start_entry, int nentries)
{
	int i;
	unsigned long bitmap_entry;


	if (unlikely(get_bitmap_entry(map, start_entry) != ENTRY_HEAD))
		return -EINVAL;
	for (i = start_entry + 1; i < nentries; i++) {
		bitmap_entry = get_bitmap_entry(map, i);
		if (bitmap_entry == ENTRY_HEAD ||
		    bitmap_entry == ENTRY_UNUSED)
			return i;
	}
	return nentries - start_entry;
}


#define SET_BITS 1
#define CLEAR_BITS 0

/*
 * alter_bitmap_ll - set or clear the entries associated to an allocation
 * @alteration: selection if the bits selected should be set or cleared
 * @map: pointer to a bitmap
 * @start: the index of the first entry in the bitmap
 * @nentries: number of entries to alter
 *
 * The modification happens lock-lessly.
 * Several users can write to the same map simultaneously, without lock.
 * If two users alter the same bit, one user will return remaining
 * entries, otherwise return 0.
 */
static int alter_bitmap_ll(bool alteration, unsigned long *map,
			   int start_entry, int nentries)
{
	unsigned long start_bit;
	unsigned long end_bit;
	unsigned long mask;
	unsigned long value;
	int nbits;
	int bits_to_write;
	int index;
	int (*action)(unsigned long *addr,
		      unsigned long mask, unsigned long value);

	action = (alteration == SET_BITS) ? set_bits_ll : clear_bits_ll;

	/* Prepare for writing the initial part of the allocation, from
	 * starting entry, to the end of the UL bitmap element which
	 * contains it. It might be larger than the actual allocation.
	 */
	start_bit = ENTRIES_TO_BITS(start_entry);
	end_bit = ENTRIES_TO_BITS(start_entry + nentries);
	nbits = ENTRIES_TO_BITS(nentries);
	bits_to_write = BITS_PER_LONG - start_bit % BITS_PER_LONG;
	mask = BITMAP_FIRST_WORD_MASK(start_bit);
	/* Mark the beginning of the allocation. */
	value = MASK | (1UL << (start_bit % BITS_PER_LONG));
	index =  BITS_DIV_LONGS(start_bit);

	/* Writes entries to the bitmap, as long as the reminder is
	 * positive or zero.
	 * Might be skipped if the entries to write do not reach the end
	 * of a bitmap UL unit.
	 */
	while (nbits >= bits_to_write) {
		if (action(map + index, mask, value & mask))
			return BITS_DIV_ENTRIES(nbits);
		nbits -= bits_to_write;
		bits_to_write = BITS_PER_LONG;
		mask = ~0UL;
		value = MASK;
		index++;
	}

	/* Takes care of the ending part of the entries to mark. */
	if (nbits > 0) {
		mask ^= BITMAP_FIRST_WORD_MASK((end_bit) % BITS_PER_LONG);
		bits_to_write = nbits;
		if (action(map + index, mask, value & mask))
			return BITS_DIV_ENTRIES(nbits);
	}
	return 0;
}


/**
 * gen_pool_create - create a new special memory pool
 * @min_alloc_order: log base 2 of number of bytes each bitmap entry represents
 * @nid: node id of the node the pool structure should be allocated on, or -1
 *
 * Create a new special memory pool that can be used to manage special purpose
 * memory not managed by the regular kmalloc/kfree interface.
 */
struct gen_pool *gen_pool_create(int min_alloc_order, int nid)
{
	struct gen_pool *pool;

	pool = kmalloc_node(sizeof(struct gen_pool), GFP_KERNEL, nid);
	if (pool != NULL) {
		spin_lock_init(&pool->lock);
		INIT_LIST_HEAD(&pool->chunks);
		pool->min_alloc_order = min_alloc_order;
		pool->algo = gen_pool_first_fit;
		pool->data = NULL;
		pool->name = NULL;
	}
	return pool;
}
EXPORT_SYMBOL(gen_pool_create);

/**
 * gen_pool_add_virt - add a new chunk of special memory to the pool
 * @pool: pool to add new memory chunk to
 * @virt: virtual starting address of memory chunk to add to pool
 * @phys: physical starting address of memory chunk to add to pool
 * @size: size in bytes of the memory chunk to add to pool
 * @nid: node id of the node the chunk structure and bitmap should be
 *       allocated on, or -1
 *
 * Add a new chunk of special memory to the specified pool.
 *
 * Returns 0 on success or a -ve errno on failure.
 */
int gen_pool_add_virt(struct gen_pool *pool, unsigned long virt, phys_addr_t phys,
		 size_t size, int nid)
{
	struct gen_pool_chunk *chunk;
	int nentries;
	int nbytes;

	nentries = size >> pool->min_alloc_order;
	nbytes = sizeof(struct gen_pool_chunk) +
		 ENTRIES_DIV_LONGS(nentries) * sizeof(long);
	chunk = kzalloc_node(nbytes, GFP_KERNEL, nid);
	if (unlikely(chunk == NULL))
		return -ENOMEM;

	chunk->phys_addr = phys;
	chunk->start_addr = virt;
	chunk->end_addr = virt + size - 1;
	atomic_long_set(&chunk->avail, size);

	spin_lock(&pool->lock);
	list_add_rcu(&chunk->next_chunk, &pool->chunks);
	spin_unlock(&pool->lock);

	return 0;
}
EXPORT_SYMBOL(gen_pool_add_virt);

/**
 * gen_pool_virt_to_phys - return the physical address of memory
 * @pool: pool to allocate from
 * @addr: starting address of memory
 *
 * Returns the physical address on success, or -1 on error.
 */
phys_addr_t gen_pool_virt_to_phys(struct gen_pool *pool, unsigned long addr)
{
	struct gen_pool_chunk *chunk;
	phys_addr_t paddr = -1;

	rcu_read_lock();
	list_for_each_entry_rcu(chunk, &pool->chunks, next_chunk) {
		if (addr >= chunk->start_addr && addr <= chunk->end_addr) {
			paddr = chunk->phys_addr + (addr - chunk->start_addr);
			break;
		}
	}
	rcu_read_unlock();

	return paddr;
}
EXPORT_SYMBOL(gen_pool_virt_to_phys);

/**
 * gen_pool_destroy - destroy a special memory pool
 * @pool: pool to destroy
 *
 * Destroy the specified special memory pool. Verifies that there are no
 * outstanding allocations.
 */
void gen_pool_destroy(struct gen_pool *pool)
{
	struct list_head *_chunk, *_next_chunk;
	struct gen_pool_chunk *chunk;
	int order = pool->min_alloc_order;
	int bit, end_bit;

	list_for_each_safe(_chunk, _next_chunk, &pool->chunks) {
		chunk = list_entry(_chunk, struct gen_pool_chunk, next_chunk);
		list_del(&chunk->next_chunk);

		end_bit = chunk_size(chunk) >> order;
		bit = find_next_bit(chunk->entries, end_bit, 0);
		BUG_ON(bit < end_bit);

		kfree(chunk);
	}
	kfree_const(pool->name);
	kfree(pool);
}
EXPORT_SYMBOL(gen_pool_destroy);

/**
 * gen_pool_alloc - allocate special memory from the pool
 * @pool: pool to allocate from
 * @size: number of bytes to allocate from the pool
 *
 * Allocate the requested number of bytes from the specified pool.
 * Uses the pool allocation function (with first-fit algorithm by default).
 * Can not be used in NMI handler on architectures without
 * NMI-safe cmpxchg implementation.
 */
unsigned long gen_pool_alloc(struct gen_pool *pool, size_t size)
{
	return gen_pool_alloc_algo(pool, size, pool->algo, pool->data);
}
EXPORT_SYMBOL(gen_pool_alloc);

/**
 * gen_pool_alloc_algo - allocate special memory from the pool
 * @pool: pool to allocate from
 * @size: number of bytes to allocate from the pool
 * @algo: algorithm passed from caller
 * @data: data passed to algorithm
 *
 * Allocate the requested number of bytes from the specified pool.
 * Uses the pool allocation function (with first-fit algorithm by default).
 * Can not be used in NMI handler on architectures without
 * NMI-safe cmpxchg implementation.
 */
unsigned long gen_pool_alloc_algo(struct gen_pool *pool, size_t size,
		genpool_algo_t algo, void *data)
{
	struct gen_pool_chunk *chunk;
	unsigned long addr = 0;
	int order = pool->min_alloc_order;
	int nentries, start_entry, end_entry, remain;

#ifndef CONFIG_ARCH_HAVE_NMI_SAFE_CMPXCHG
	BUG_ON(in_nmi());
#endif

	if (size == 0)
		return 0;

	nentries = mem_to_units(size, order);
	rcu_read_lock();
	list_for_each_entry_rcu(chunk, &pool->chunks, next_chunk) {
		if (size > atomic_long_read(&chunk->avail))
			continue;

		start_entry = 0;
		end_entry = chunk_size(chunk) >> order;
retry:
		start_entry = algo(chunk->entries, end_entry, start_entry,
				  nentries, data, pool);
		if (start_entry >= end_entry)
			continue;
		remain = alter_bitmap_ll(SET_BITS, chunk->entries,
					 start_entry, nentries);
		if (remain) {
			remain = alter_bitmap_ll(CLEAR_BITS,
						 chunk->entries,
						 start_entry,
						 nentries - remain);
			goto retry;
		}

		addr = chunk->start_addr +
			((unsigned long)start_entry << order);
		size = nentries << order;
		atomic_long_sub(size, &chunk->avail);
		break;
	}
	rcu_read_unlock();
	return addr;
}
EXPORT_SYMBOL(gen_pool_alloc_algo);

/**
 * gen_pool_dma_alloc - allocate special memory from the pool for DMA usage
 * @pool: pool to allocate from
 * @size: number of bytes to allocate from the pool
 * @dma: dma-view physical address return value.  Use NULL if unneeded.
 *
 * Allocate the requested number of bytes from the specified pool.
 * Uses the pool allocation function (with first-fit algorithm by default).
 * Can not be used in NMI handler on architectures without
 * NMI-safe cmpxchg implementation.
 */
void *gen_pool_dma_alloc(struct gen_pool *pool, size_t size, dma_addr_t *dma)
{
	unsigned long vaddr;

	if (!pool)
		return NULL;

	vaddr = gen_pool_alloc(pool, size);
	if (!vaddr)
		return NULL;

	if (dma)
		*dma = gen_pool_virt_to_phys(pool, vaddr);

	return (void *)vaddr;
}
EXPORT_SYMBOL(gen_pool_dma_alloc);

/**
 * gen_pool_free - free allocated special memory back to the pool
 * @pool: pool to free to
 * @addr: starting address of memory to free back to pool
 * @size: size in bytes of memory to free or 0, for auto-detection
 *
 * Free previously allocated special memory back to the specified
 * pool.  Can not be used in NMI handler on architectures without
 * NMI-safe cmpxchg implementation.
 */
void gen_pool_free(struct gen_pool *pool, unsigned long addr, size_t size)
{
	struct gen_pool_chunk *chunk;
	int order = pool->min_alloc_order;
	int start_entry, remaining_entries, nentries, remain;
	int boundary;

#ifndef CONFIG_ARCH_HAVE_NMI_SAFE_CMPXCHG
	BUG_ON(in_nmi());
#endif

	rcu_read_lock();
	list_for_each_entry_rcu(chunk, &pool->chunks, next_chunk) {
		if (addr >= chunk->start_addr && addr <= chunk->end_addr) {
			BUG_ON(addr + size - 1 > chunk->end_addr);
			start_entry = (addr - chunk->start_addr) >> order;
			remaining_entries = (chunk->end_addr - addr) >> order;
			boundary = get_boundary(chunk->entries, start_entry,
						remaining_entries);
			BUG_ON(boundary < 0);
			nentries = boundary - start_entry;
			BUG_ON(size &&
			       (nentries != mem_to_units(size, order)));
			remain = alter_bitmap_ll(CLEAR_BITS, chunk->entries,
						 start_entry, nentries);
			BUG_ON(remain);
			atomic_long_add(nentries << order, &chunk->avail);
			rcu_read_unlock();
			return;
		}
	}
	rcu_read_unlock();
	BUG();
}
EXPORT_SYMBOL(gen_pool_free);

/**
 * gen_pool_for_each_chunk - call func for every chunk of generic memory pool
 * @pool:	the generic memory pool
 * @func:	func to call
 * @data:	additional data used by @func
 *
 * Call @func for every chunk of generic memory pool.  The @func is
 * called with rcu_read_lock held.
 */
void gen_pool_for_each_chunk(struct gen_pool *pool,
	void (*func)(struct gen_pool *pool, struct gen_pool_chunk *chunk, void *data),
	void *data)
{
	struct gen_pool_chunk *chunk;

	rcu_read_lock();
	list_for_each_entry_rcu(chunk, &(pool)->chunks, next_chunk)
		func(pool, chunk, data);
	rcu_read_unlock();
}
EXPORT_SYMBOL(gen_pool_for_each_chunk);

/**
 * addr_in_gen_pool - checks if an address falls within the range of a pool
 * @pool:	the generic memory pool
 * @start:	start address
 * @size:	size of the region
 *
 * Check if the range of addresses falls within the specified pool. Returns
 * true if the entire range is contained in the pool and false otherwise.
 */
bool addr_in_gen_pool(struct gen_pool *pool, unsigned long start,
			size_t size)
{
	bool found = false;
	unsigned long end = start + size - 1;
	struct gen_pool_chunk *chunk;

	rcu_read_lock();
	list_for_each_entry_rcu(chunk, &(pool)->chunks, next_chunk) {
		if (start >= chunk->start_addr && start <= chunk->end_addr) {
			if (end <= chunk->end_addr) {
				found = true;
				break;
			}
		}
	}
	rcu_read_unlock();
	return found;
}

/**
 * gen_pool_avail - get available free space of the pool
 * @pool: pool to get available free space
 *
 * Return available free space of the specified pool.
 */
size_t gen_pool_avail(struct gen_pool *pool)
{
	struct gen_pool_chunk *chunk;
	size_t avail = 0;

	rcu_read_lock();
	list_for_each_entry_rcu(chunk, &pool->chunks, next_chunk)
		avail += atomic_long_read(&chunk->avail);
	rcu_read_unlock();
	return avail;
}
EXPORT_SYMBOL_GPL(gen_pool_avail);

/**
 * gen_pool_size - get size in bytes of memory managed by the pool
 * @pool: pool to get size
 *
 * Return size in bytes of memory managed by the pool.
 */
size_t gen_pool_size(struct gen_pool *pool)
{
	struct gen_pool_chunk *chunk;
	size_t size = 0;

	rcu_read_lock();
	list_for_each_entry_rcu(chunk, &pool->chunks, next_chunk)
		size += chunk_size(chunk);
	rcu_read_unlock();
	return size;
}
EXPORT_SYMBOL_GPL(gen_pool_size);

/**
 * gen_pool_set_algo - set the allocation algorithm
 * @pool: pool to change allocation algorithm
 * @algo: custom algorithm function
 * @data: additional data used by @algo
 *
 * Call @algo for each memory allocation in the pool.
 * If @algo is NULL use gen_pool_first_fit as default
 * memory allocation function.
 */
void gen_pool_set_algo(struct gen_pool *pool, genpool_algo_t algo, void *data)
{
	rcu_read_lock();

	pool->algo = algo;
	if (!pool->algo)
		pool->algo = gen_pool_first_fit;

	pool->data = data;

	rcu_read_unlock();
}
EXPORT_SYMBOL(gen_pool_set_algo);

/**
 * gen_pool_first_fit - find the first available region
 * of memory matching the size requirement (no alignment constraint)
 * @map: The address to base the search on
 * @size: The number of allocation units in the bitmap
 * @start: The allocation unit to start searching at
 * @nr: The number of allocation units we're looking for
 * @data: additional data - unused
 * @pool: pool to find the fit region memory from
 */
unsigned long gen_pool_first_fit(unsigned long *map, unsigned long size,
		unsigned long start, unsigned int nr, void *data,
		struct gen_pool *pool)
{
	unsigned long align_mask;
	unsigned long bit_index;

	align_mask = roundup_pow_of_two(BITS_PER_ENTRY) - 1;
	bit_index = bitmap_find_next_zero_area(map, ENTRIES_TO_BITS(size),
					       ENTRIES_TO_BITS(start),
					       ENTRIES_TO_BITS(nr),
					       align_mask);
	return BITS_DIV_ENTRIES(bit_index);
}
EXPORT_SYMBOL(gen_pool_first_fit);

/**
 * gen_pool_first_fit_align - find the first available region
 * of memory matching the size requirement (alignment constraint)
 * @map: The address to base the search on
 * @size: The number of allocation units in the bitmap
 * @start: The allocation unit to start searching at
 * @nr: The number of allocation units we're looking for
 * @data: data for alignment
 * @pool: pool to get order from
 */
unsigned long gen_pool_first_fit_align(unsigned long *map, unsigned long size,
		unsigned long start, unsigned int nr, void *data,
		struct gen_pool *pool)
{
	struct genpool_data_align *alignment;
	unsigned long align_mask;
	unsigned long bit_index;
	int order;

	alignment = data;
	order = pool->min_alloc_order;
	align_mask = roundup_pow_of_two(
			ENTRIES_TO_BITS(mem_to_units(alignment->align,
						     order))) - 1;
	bit_index = bitmap_find_next_zero_area(map, ENTRIES_TO_BITS(size),
					       ENTRIES_TO_BITS(start),
					       ENTRIES_TO_BITS(nr),
					       align_mask);
	return BITS_DIV_ENTRIES(bit_index);
}
EXPORT_SYMBOL(gen_pool_first_fit_align);

/**
 * gen_pool_fixed_alloc - reserve a specific region
 * @map: The address to base the search on
 * @size: The number of allocation units in the bitmap
 * @start: The allocation unit to start searching at
 * @nr: The number of allocation units we're looking for
 * @data: data for alignment
 * @pool: pool to get order from
 */
unsigned long gen_pool_fixed_alloc(unsigned long *map, unsigned long size,
		unsigned long start, unsigned int nr, void *data,
		struct gen_pool *pool)
{
	struct genpool_data_fixed *fixed_data;
	int order;
	unsigned long offset;
	unsigned long align_mask;
	unsigned long bit_index;

	fixed_data = data;
	order = pool->min_alloc_order;
	if (WARN_ON(fixed_data->offset & ((1UL << order) - 1)))
		return size;
	offset = fixed_data->offset >> order;
	align_mask = roundup_pow_of_two(BITS_PER_ENTRY) - 1;
	bit_index = bitmap_find_next_zero_area(map, ENTRIES_TO_BITS(size),
					       ENTRIES_TO_BITS(start + offset),
					       ENTRIES_TO_BITS(nr), align_mask);
	if (bit_index != ENTRIES_TO_BITS(offset))
		return size;

	return BITS_DIV_ENTRIES(bit_index);
}
EXPORT_SYMBOL(gen_pool_fixed_alloc);

/**
 * gen_pool_first_fit_order_align - find the first available region
 * of memory matching the size requirement. The region will be aligned
 * to the order of the size specified.
 * @map: The address to base the search on
 * @size: The number of allocation units in the bitmap
 * @start: The allocation unit to start searching at
 * @nr: The number of allocation units we're looking for
 * @data: additional data - unused
 * @pool: pool to find the fit region memory from
 */
unsigned long gen_pool_first_fit_order_align(unsigned long *map,
		unsigned long size, unsigned long start,
		unsigned int nr, void *data, struct gen_pool *pool)
{
	unsigned long align_mask;
	unsigned long bit_index;

	align_mask = roundup_pow_of_two(ENTRIES_TO_BITS(nr)) - 1;
	bit_index = bitmap_find_next_zero_area(map, ENTRIES_TO_BITS(size),
					       ENTRIES_TO_BITS(start),
					       ENTRIES_TO_BITS(nr),
					       align_mask);
	return BITS_DIV_ENTRIES(bit_index);
}
EXPORT_SYMBOL(gen_pool_first_fit_order_align);

/**
 * gen_pool_best_fit - find the best fitting region of memory
 * macthing the size requirement (no alignment constraint)
 * @map: The address to base the search on
 * @size: The number of allocation units in the bitmap
 * @start: The allocation unit to start searching at
 * @nr: The number of allocation units we're looking for
 * @data: additional data - unused
 * @pool: pool to find the fit region memory from
 *
 * Iterate over the bitmap to find the smallest free region
 * which we can allocate the memory.
 */
unsigned long gen_pool_best_fit(unsigned long *map, unsigned long size,
		unsigned long start, unsigned int nr, void *data,
		struct gen_pool *pool)
{
	unsigned long start_bit = ENTRIES_TO_BITS(size);
	unsigned long len = size + 1;
	unsigned long index;
	unsigned long align_mask;
	unsigned long bit_index;

	align_mask = roundup_pow_of_two(BITS_PER_ENTRY) - 1;
	bit_index = bitmap_find_next_zero_area(map, ENTRIES_TO_BITS(size),
					       ENTRIES_TO_BITS(start),
					       ENTRIES_TO_BITS(nr),
					       align_mask);
	index = BITS_DIV_ENTRIES(bit_index);

	while (index < size) {
		int next_bit;

		next_bit = find_next_bit(map, ENTRIES_TO_BITS(size),
					 ENTRIES_TO_BITS(index + nr));
		if ((BITS_DIV_ENTRIES(next_bit) - index) < len) {
			len = BITS_DIV_ENTRIES(next_bit) - index;
			start_bit = ENTRIES_TO_BITS(index);
			if (len == nr)
				return BITS_DIV_ENTRIES(start_bit);
		}
		bit_index =
			bitmap_find_next_zero_area(map,
						   ENTRIES_TO_BITS(size),
						   next_bit + 1,
						   ENTRIES_TO_BITS(nr),
						   align_mask);
		index = BITS_DIV_ENTRIES(bit_index);
	}

	return BITS_DIV_ENTRIES(start_bit);
}

static void devm_gen_pool_release(struct device *dev, void *res)
{
	gen_pool_destroy(*(struct gen_pool **)res);
}

static int devm_gen_pool_match(struct device *dev, void *res, void *data)
{
	struct gen_pool **p = res;

	/* NULL data matches only a pool without an assigned name */
	if (!data && !(*p)->name)
		return 1;

	if (!data || !(*p)->name)
		return 0;

	return !strcmp((*p)->name, data);
}

/**
 * gen_pool_get - Obtain the gen_pool (if any) for a device
 * @dev: device to retrieve the gen_pool from
 * @name: name of a gen_pool or NULL, identifies a particular gen_pool on device
 *
 * Returns the gen_pool for the device if one is present, or NULL.
 */
struct gen_pool *gen_pool_get(struct device *dev, const char *name)
{
	struct gen_pool **p;

	p = devres_find(dev, devm_gen_pool_release, devm_gen_pool_match,
			(void *)name);
	if (!p)
		return NULL;
	return *p;
}
EXPORT_SYMBOL_GPL(gen_pool_get);

/**
 * devm_gen_pool_create - managed gen_pool_create
 * @dev: device that provides the gen_pool
 * @min_alloc_order: log base 2 of number of bytes each bitmap bit represents
 * @nid: node selector for allocated gen_pool, %NUMA_NO_NODE for all nodes
 * @name: name of a gen_pool or NULL, identifies a particular gen_pool on device
 *
 * Create a new special memory pool that can be used to manage special purpose
 * memory not managed by the regular kmalloc/kfree interface. The pool will be
 * automatically destroyed by the device management code.
 */
struct gen_pool *devm_gen_pool_create(struct device *dev, int min_alloc_order,
				      int nid, const char *name)
{
	struct gen_pool **ptr, *pool;
	const char *pool_name = NULL;

	/* Check that genpool to be created is uniquely addressed on device */
	if (gen_pool_get(dev, name))
		return ERR_PTR(-EINVAL);

	if (name) {
		pool_name = kstrdup_const(name, GFP_KERNEL);
		if (!pool_name)
			return ERR_PTR(-ENOMEM);
	}

	ptr = devres_alloc(devm_gen_pool_release, sizeof(*ptr), GFP_KERNEL);
	if (!ptr)
		goto free_pool_name;

	pool = gen_pool_create(min_alloc_order, nid);
	if (!pool)
		goto free_devres;

	*ptr = pool;
	pool->name = pool_name;
	devres_add(dev, ptr);

	return pool;

free_devres:
	devres_free(ptr);
free_pool_name:
	kfree_const(pool_name);

	return ERR_PTR(-ENOMEM);
}
EXPORT_SYMBOL(devm_gen_pool_create);

#ifdef CONFIG_OF
/**
 * of_gen_pool_get - find a pool by phandle property
 * @np: device node
 * @propname: property name containing phandle(s)
 * @index: index into the phandle array
 *
 * Returns the pool that contains the chunk starting at the physical
 * address of the device tree node pointed at by the phandle property,
 * or NULL if not found.
 */
struct gen_pool *of_gen_pool_get(struct device_node *np,
	const char *propname, int index)
{
	struct platform_device *pdev;
	struct device_node *np_pool, *parent;
	const char *name = NULL;
	struct gen_pool *pool = NULL;

	np_pool = of_parse_phandle(np, propname, index);
	if (!np_pool)
		return NULL;

	pdev = of_find_device_by_node(np_pool);
	if (!pdev) {
		/* Check if named gen_pool is created by parent node device */
		parent = of_get_parent(np_pool);
		pdev = of_find_device_by_node(parent);
		of_node_put(parent);

		of_property_read_string(np_pool, "label", &name);
		if (!name)
			name = np_pool->name;
	}
	if (pdev)
		pool = gen_pool_get(&pdev->dev, name);
	of_node_put(np_pool);

	return pool;
}
EXPORT_SYMBOL_GPL(of_gen_pool_get);
#endif /* CONFIG_OF */
