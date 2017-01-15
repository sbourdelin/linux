#ifndef BLK_STAT_H
#define BLK_STAT_H

/*
 * ~0.13s window as a power-of-2 (2^27 nsecs)
 */
#define BLK_STAT_NSEC		134217728ULL
#define BLK_STAT_NSEC_MASK	~(BLK_STAT_NSEC - 1)

/*
 * from upper:
 * 3 bits: reserved for other usage
 * 12 bits: size
 * 49 bits: time
 */
#define BLK_STAT_RES_BITS	3
#define BLK_STAT_SIZE_BITS	12
#define BLK_STAT_RES_SHIFT	(64 - BLK_STAT_RES_BITS)
#define BLK_STAT_SIZE_SHIFT	(BLK_STAT_RES_SHIFT - BLK_STAT_SIZE_BITS)
#define BLK_STAT_TIME_MASK	((1ULL << BLK_STAT_SIZE_SHIFT) - 1)
#define BLK_STAT_SIZE_MASK	\
	(((1ULL << BLK_STAT_SIZE_BITS) - 1) << BLK_STAT_SIZE_SHIFT)
#define BLK_STAT_RES_MASK	(~((1ULL << BLK_STAT_RES_SHIFT) - 1))

enum {
	BLK_STAT_READ	= 0,
	BLK_STAT_WRITE,
};

void blk_stat_add(struct blk_rq_stat *, struct request *);
void blk_hctx_stat_get(struct blk_mq_hw_ctx *, struct blk_rq_stat *);
void blk_queue_stat_get(struct request_queue *, struct blk_rq_stat *);
void blk_stat_clear(struct request_queue *);
void blk_stat_init(struct blk_rq_stat *);
bool blk_stat_is_current(struct blk_rq_stat *);
void blk_stat_set_issue(struct blk_issue_stat *stat, sector_t size);
bool blk_stat_enable(struct request_queue *);

static inline u64 __blk_stat_time(u64 time)
{
	return time & BLK_STAT_TIME_MASK;
}

static inline u64 blk_stat_time(struct blk_issue_stat *stat)
{
	return __blk_stat_time(stat->stat);
}

static inline sector_t blk_capped_size(sector_t size)
{
	return size & ((1ULL << BLK_STAT_SIZE_BITS) - 1);
}

static inline sector_t blk_stat_size(struct blk_issue_stat *stat)
{
	return (stat->stat & BLK_STAT_SIZE_MASK) >> BLK_STAT_SIZE_SHIFT;
}

#endif
