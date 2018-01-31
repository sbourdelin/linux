/* SPDX-License-Identifier: GPL-2.0 */
#ifndef LINUX_MM_INLINE_H
#define LINUX_MM_INLINE_H

#include <linux/huge_mm.h>
#include <linux/random.h>
#include <linux/swap.h>

/**
 * page_is_file_cache - should the page be on a file LRU or anon LRU?
 * @page: the page to test
 *
 * Returns 1 if @page is page cache page backed by a regular filesystem,
 * or 0 if @page is anonymous, tmpfs or otherwise ram or swap backed.
 * Used by functions that manipulate the LRU lists, to sort a page
 * onto the right LRU list.
 *
 * We would like to get this info without a page flag, but the state
 * needs to survive until the page is last deleted from the LRU, which
 * could be as far down as __page_cache_release.
 */
static inline int page_is_file_cache(struct page *page)
{
	return !PageSwapBacked(page);
}

static __always_inline void __update_lru_size(struct lruvec *lruvec,
				enum lru_list lru, enum zone_type zid,
				int nr_pages)
{
	struct pglist_data *pgdat = lruvec_pgdat(lruvec);

	__mod_node_page_state(pgdat, NR_LRU_BASE + lru, nr_pages);
	__mod_zone_page_state(&pgdat->node_zones[zid],
				NR_ZONE_LRU_BASE + lru, nr_pages);
}

static __always_inline void update_lru_size(struct lruvec *lruvec,
				enum lru_list lru, enum zone_type zid,
				int nr_pages)
{
	__update_lru_size(lruvec, lru, zid, nr_pages);
#ifdef CONFIG_MEMCG
	mem_cgroup_update_lru_size(lruvec, lru, zid, nr_pages);
#endif
}

static __always_inline void __add_page_to_lru_list(struct page *page,
				struct lruvec *lruvec, enum lru_list lru)
{
	int tag;
	struct page *cur, *next, *second_page;
	struct lru_list_head *head = &lruvec->lists[lru];

	list_add(&page->lru, lru_head(head));
	/* Set sentinel unconditionally until batch is full. */
	page->lru_sentinel = true;

	second_page = container_of(page->lru.next, struct page, lru);
	VM_BUG_ON_PAGE(!second_page->lru_sentinel, second_page);

	page->lru_batch = head->first_batch_tag;
	++head->first_batch_npages;

	if (head->first_batch_npages < LRU_BATCH_MAX)
		return;

	tag = head->first_batch_tag;
	if (likely(second_page->lru_batch == tag)) {
		/* Unset sentinel bit in all non-sentinel nodes. */
		cur = second_page;
		list_for_each_entry_from(cur, lru_head(head), lru) {
			next = list_next_entry(cur, lru);
			if (next->lru_batch != tag)
				break;
			cur->lru_sentinel = false;
		}
	}

	tag = prandom_u32_max(NUM_LRU_BATCH_LOCKS);
	if (unlikely(tag == head->first_batch_tag))
		tag = (tag + 1) % NUM_LRU_BATCH_LOCKS;
	head->first_batch_tag = tag;
	head->first_batch_npages = 0;
}

static __always_inline void add_page_to_lru_list(struct page *page,
				struct lruvec *lruvec, enum lru_list lru)
{
	update_lru_size(lruvec, lru, page_zonenum(page), hpage_nr_pages(page));
	__add_page_to_lru_list(page, lruvec, lru);
}

static __always_inline void __add_page_to_lru_list_tail(struct page *page,
				struct lruvec *lruvec, enum lru_list lru)
{
	int tag;
	struct page *cur, *prev, *second_page;
	struct lru_list_head *head = &lruvec->lists[lru];

	list_add_tail(&page->lru, lru_head(head));
	/* Set sentinel unconditionally until batch is full. */
	page->lru_sentinel = true;

	second_page = container_of(page->lru.prev, struct page, lru);
	VM_BUG_ON_PAGE(!second_page->lru_sentinel, second_page);

	page->lru_batch = head->last_batch_tag;
	++head->last_batch_npages;

	if (head->last_batch_npages < LRU_BATCH_MAX)
		return;

	tag = head->last_batch_tag;
	if (likely(second_page->lru_batch == tag)) {
		/* Unset sentinel bit in all non-sentinel nodes. */
		cur = second_page;
		list_for_each_entry_from_reverse(cur, lru_head(head), lru) {
			prev = list_prev_entry(cur, lru);
			if (prev->lru_batch != tag)
				break;
			cur->lru_sentinel = false;
		}
	}

	tag = prandom_u32_max(NUM_LRU_BATCH_LOCKS);
	if (unlikely(tag == head->last_batch_tag))
		tag = (tag + 1) % NUM_LRU_BATCH_LOCKS;
	head->last_batch_tag = tag;
	head->last_batch_npages = 0;
}

static __always_inline void add_page_to_lru_list_tail(struct page *page,
				struct lruvec *lruvec, enum lru_list lru)
{

	update_lru_size(lruvec, lru, page_zonenum(page), hpage_nr_pages(page));
	__add_page_to_lru_list_tail(page, lruvec, lru);
}

static __always_inline void __del_page_from_lru_list(struct page *page,
				struct lruvec *lruvec, enum lru_list lru)
{
	struct page *left, *right;

	left  = container_of(page->lru.prev, struct page, lru);
	right = container_of(page->lru.next, struct page, lru);

	if (page->lru_sentinel) {
		VM_BUG_ON(!left->lru_sentinel && !right->lru_sentinel);
		left->lru_sentinel = true;
		right->lru_sentinel = true;
	}

	list_del(&page->lru);
}

static __always_inline void del_page_from_lru_list(struct page *page,
				struct lruvec *lruvec, enum lru_list lru)
{
	__del_page_from_lru_list(page, lruvec, lru);
	update_lru_size(lruvec, lru, page_zonenum(page), -hpage_nr_pages(page));
}

static __always_inline void move_page_to_lru_list(struct page *page,
						  struct lruvec *lruvec,
						  enum lru_list lru)
{
	__del_page_from_lru_list(page, lruvec, lru);
	__add_page_to_lru_list(page, lruvec, lru);
}

static __always_inline void move_page_to_lru_list_tail(struct page *page,
						       struct lruvec *lruvec,
						       enum lru_list lru)
{
	__del_page_from_lru_list(page, lruvec, lru);
	__add_page_to_lru_list_tail(page, lruvec, lru);
}

static __always_inline void lru_lock_all(struct pglist_data *pgdat,
					 unsigned long *flags)
{
	size_t i;

	if (flags)
		local_irq_save(*flags);
	else
		local_irq_disable();

	for (i = 0; i < NUM_LRU_BATCH_LOCKS; ++i)
		spin_lock(&pgdat->lru_batch_locks[i].lock);

	spin_lock(&pgdat->lru_lock);
}

static __always_inline void lru_unlock_all(struct pglist_data *pgdat,
					   unsigned long *flags)
{
	int i;

	spin_unlock(&pgdat->lru_lock);

	for (i = NUM_LRU_BATCH_LOCKS - 1; i >= 0; --i)
		spin_unlock(&pgdat->lru_batch_locks[i].lock);

	if (flags)
		local_irq_restore(*flags);
	else
		local_irq_enable();
}

static __always_inline spinlock_t *page_lru_batch_lock(struct page *page)
{
	return &page_pgdat(page)->lru_batch_locks[page->lru_batch].lock;
}

/**
 * lru_batch_lock - lock an LRU list batch
 */
static __always_inline void lru_batch_lock(struct page *page,
					   spinlock_t **locked_lru_batch,
					   struct pglist_data **locked_pgdat,
					   unsigned long *flags)
{
	spinlock_t *lru_batch = page_lru_batch_lock(page);
	struct pglist_data *pgdat = page_pgdat(page);

	VM_BUG_ON(*locked_pgdat && !page->lru_sentinel);

	if (lru_batch != *locked_lru_batch) {
		VM_BUG_ON(*locked_pgdat);
		VM_BUG_ON(*locked_lru_batch);
		spin_lock_irqsave(lru_batch, *flags);
		*locked_lru_batch = lru_batch;
		if (page->lru_sentinel) {
			spin_lock(&pgdat->lru_lock);
			*locked_pgdat = pgdat;
		}
	} else if (!*locked_pgdat && page->lru_sentinel) {
		spin_lock(&pgdat->lru_lock);
		*locked_pgdat = pgdat;
	}
}

/**
 * lru_batch_unlock - unlock an LRU list batch
 */
static __always_inline void lru_batch_unlock(struct page *page,
					     spinlock_t **locked_lru_batch,
					     struct pglist_data **locked_pgdat,
					     unsigned long *flags)
{
	spinlock_t *lru_batch = (page) ? page_lru_batch_lock(page) : NULL;

	VM_BUG_ON(!*locked_lru_batch);

	if (lru_batch != *locked_lru_batch) {
		if (*locked_pgdat) {
			spin_unlock(&(*locked_pgdat)->lru_lock);
			*locked_pgdat = NULL;
		}
		spin_unlock_irqrestore(*locked_lru_batch, *flags);
		*locked_lru_batch = NULL;
	} else if (*locked_pgdat && !page->lru_sentinel) {
		spin_unlock(&(*locked_pgdat)->lru_lock);
		*locked_pgdat = NULL;
	}
}

/**
 * page_lru_base_type - which LRU list type should a page be on?
 * @page: the page to test
 *
 * Used for LRU list index arithmetic.
 *
 * Returns the base LRU type - file or anon - @page should be on.
 */
static inline enum lru_list page_lru_base_type(struct page *page)
{
	if (page_is_file_cache(page))
		return LRU_INACTIVE_FILE;
	return LRU_INACTIVE_ANON;
}

/**
 * page_off_lru - which LRU list was page on? clearing its lru flags.
 * @page: the page to test
 *
 * Returns the LRU list a page was on, as an index into the array of LRU
 * lists; and clears its Unevictable or Active flags, ready for freeing.
 */
static __always_inline enum lru_list page_off_lru(struct page *page)
{
	enum lru_list lru;

	if (PageUnevictable(page)) {
		__ClearPageUnevictable(page);
		lru = LRU_UNEVICTABLE;
	} else {
		lru = page_lru_base_type(page);
		if (PageActive(page)) {
			__ClearPageActive(page);
			lru += LRU_ACTIVE;
		}
	}
	return lru;
}

/**
 * page_lru - which LRU list should a page be on?
 * @page: the page to test
 *
 * Returns the LRU list a page should be on, as an index
 * into the array of LRU lists.
 */
static __always_inline enum lru_list page_lru(struct page *page)
{
	enum lru_list lru;

	if (PageUnevictable(page))
		lru = LRU_UNEVICTABLE;
	else {
		lru = page_lru_base_type(page);
		if (PageActive(page))
			lru += LRU_ACTIVE;
	}
	return lru;
}

#define lru_to_page(head) (list_entry((head)->prev, struct page, lru))

#ifdef arch_unmap_kpfn
extern void arch_unmap_kpfn(unsigned long pfn);
#else
static __always_inline void arch_unmap_kpfn(unsigned long pfn) { }
#endif

#endif
