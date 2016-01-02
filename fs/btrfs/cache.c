/*
 * (c) Sanidhya Solanki, 2016
 *
 * Licensed under the FSF's GNU Public License v2 or later.
 */
#include <linux/types.h>

/* Cache size configuration )in MiB).*/
#define MAX_CACHE_SIZE = 10000
#define MIN_CACHE_SIZE = 10

/* Time (in seconds)before retrying to increase the cache size.*/
#define CACHE_RETRY = 10

/* Space required to be free (in MiB) before increasing the size of the
 * cache. If cache size is less than cache_grow_limit, a block will be freed
 * from the cache to allow the cache to continue growning.
 */
#define CACHE_GROW_LIMIT = 100

/* Size required to be free (in MiB) after we shrink the cache, so that it
 * does not grow in size immediately.
 */
#define CACHE_SHRINK_FREE_SPACE_LIMIT = 100

/* Age (in seconds) of oldest and newest block in the cache.*/
#define MAX_AGE_LIMIT = 300	/* Five Minute Rule recommendation,
				 * optimum size depends on size of data
				 * blocks.
				 */
#define MIN_AGE_LIMIT = 15	/* In case of cache stampede.*/

/* Memory constraints (in percentage) before we stop caching.*/
#define MIN_MEM_FREE = 10

/* Cache statistics. */
struct cache_stats {
	u64		cache_size;
	u64		maximum_cache_size_attained;
	int		cache_hit_rate;
	int		cache_miss_rate;
	u64		cache_evicted;
	u64		duplicate_read;
	u64		duplicate_write;
	int		stats_update_interval;
};

#define cache_size	CACHE_SIZE /* Current cache size.*/
#define max_cache_size	MAX_SIZE /* Max cache limit. */
#define min_cache_size	MIN_SIZE /* Min cache limit.*/
#define cache_time	MAX_TIME /* Maximum time to keep data in cache.*/
#define evicted_csum	EVICTED_CSUM	/* Checksum of the evited data
					 * (to avoid repeatedly caching
					 * data that was just evicted.
					 */
#define read_csum	READ_CSUM /* Checksum of the read data.*/
#define write_csum	WRITE_CSUM /* Checksum of the written data.*/
#define evict_interval	EVICT_INTERVAL /* Time to keep data before eviction.*/
