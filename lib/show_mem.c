/*
 * Generic show_mem() implementation
 *
 * Copyright (C) 2008 Johannes Weiner <hannes@saeurebad.de>
 * All code subject to the GPL version 2.
 */

#include <linux/mm.h>
#include <linux/quicklist.h>
#include <linux/cma.h>
#include <linux/vm_event_item.h>

void show_mem(unsigned int filter)
{
	pg_data_t *pgdat;
	unsigned long total = 0, reserved = 0, highmem = 0;

	printk("Mem-Info:\n");
	show_free_areas(filter);

#ifdef CONFIG_COMPACTION
	if (filter & SHOW_COMPACTION_STATS) {
		printk("compaction_stall:%lu compaction_fail:%lu "
		       "compact_migrate_scanned:%lu compact_free_scanned:%lu "
		       "compact_isolated:%lu "
		       "pgmigrate_success:%lu pgmigrate_fail:%lu\n",
		       global_page_state(COMPACTSTALL), global_page_state(COMPACTFAIL),
		       global_page_state(COMPACTMIGRATE_SCANNED), global_page_state(COMPACTFAIL),
		       global_page_state(COMPACTISOLATED),
		       global_page_state(PGMIGRATE_SUCCESS), global_page_state(PGMIGRATE_FAIL));
	}
#endif

	for_each_online_pgdat(pgdat) {
		unsigned long flags;
		int zoneid;

		pgdat_resize_lock(pgdat, &flags);
		for (zoneid = 0; zoneid < MAX_NR_ZONES; zoneid++) {
			struct zone *zone = &pgdat->node_zones[zoneid];
			if (!populated_zone(zone))
				continue;

			total += zone->present_pages;
			reserved += zone->present_pages - zone->managed_pages;

			if (is_highmem_idx(zoneid))
				highmem += zone->present_pages;
		}
		pgdat_resize_unlock(pgdat, &flags);
	}

	printk("%lu pages RAM\n", total);
	printk("%lu pages HighMem/MovableOnly\n", highmem);
	printk("%lu pages reserved\n", reserved);
#ifdef CONFIG_CMA
	printk("%lu pages cma reserved\n", totalcma_pages);
#endif
#ifdef CONFIG_QUICKLIST
	printk("%lu pages in pagetable cache\n",
		quicklist_total_size());
#endif
#ifdef CONFIG_MEMORY_FAILURE
	printk("%lu pages hwpoisoned\n", atomic_long_read(&num_poisoned_pages));
#endif
}
