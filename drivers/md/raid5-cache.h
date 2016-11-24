#ifndef _RAID5_CACHE_H
#define _RAID5_CACHE_H

/*
 * r5c journal modes of the array: write-back or write-through.
 * write-through mode has identical behavior as existing log only
 * implementation.
 */
enum r5c_journal_mode {
	R5C_JOURNAL_MODE_WRITE_THROUGH = 0,
	R5C_JOURNAL_MODE_WRITE_BACK = 1,
};

struct r5l_log {
	struct md_rdev *rdev;

	u32 uuid_checksum;

	sector_t device_size;		/* log device size, round to
					 * BLOCK_SECTORS */
	sector_t max_free_space;	/* reclaim run if free space is at
					 * this size */

	sector_t last_checkpoint;	/* log tail. where recovery scan
					 * starts from */
	u64 last_cp_seq;		/* log tail sequence */

	sector_t log_start;		/* log head. where new data appends */
	u64 seq;			/* log head sequence */

	sector_t next_checkpoint;
	u64 next_cp_seq;

	struct mutex io_mutex;
	struct r5l_io_unit *current_io;	/* current io_unit accepting new data */

	spinlock_t io_list_lock;
	struct list_head running_ios;	/* io_units which are still running,
					 * and have not yet been completely
					 * written to the log */
	struct list_head io_end_ios;	/* io_units which have been completely
					 * written to the log but not yet written
					 * to the RAID */
	struct list_head flushing_ios;	/* io_units which are waiting for log
					 * cache flush */
	struct list_head finished_ios;	/* io_units which settle down in log disk */
	struct bio flush_bio;

	struct list_head no_mem_stripes;   /* pending stripes, -ENOMEM */

	struct kmem_cache *io_kc;
	mempool_t *io_pool;
	struct bio_set *bs;
	mempool_t *meta_pool;

	struct md_thread *reclaim_thread;
	unsigned long reclaim_target;	/* number of space that need to be
					 * reclaimed.  if it's 0, reclaim spaces
					 * used by io_units which are in
					 * IO_UNIT_STRIPE_END state (eg, reclaim
					 * dones't wait for specific io_unit
					 * switching to IO_UNIT_STRIPE_END
					 * state) */
	wait_queue_head_t iounit_wait;

	struct list_head no_space_stripes; /* pending stripes, log has no space */
	spinlock_t no_space_stripes_lock;

	bool need_cache_flush;

	/* for r5c_cache */
	enum r5c_journal_mode r5c_journal_mode;

	/* all stripes in r5cache, in the order of seq at sh->log_start */
	struct list_head stripe_in_journal_list;

	spinlock_t stripe_in_journal_lock;
	atomic_t stripe_in_journal_count;

	/* to submit async io_units, to fulfill ordering of flush */
	struct work_struct deferred_io_work;

	struct r5l_policy *policy;
	enum {
		RWH_POLICY_OFF,
		RWH_POLICY_JOURNAL,
		RWH_POLICY_PPL,
	} rwh_policy;

	void *private;
};

/*
 * an IO range starts from a meta data block and end at the next meta data
 * block. The io unit's the meta data block tracks data/parity followed it. io
 * unit is written to log disk with normal write, as we always flush log disk
 * first and then start move data to raid disks, there is no requirement to
 * write io unit with FLUSH/FUA
 */
struct r5l_io_unit {
	struct r5l_log *log;

	struct page *meta_page;	/* store meta block */
	int meta_offset;	/* current offset in meta_page */

	struct bio *current_bio;/* current_bio accepting new data */

	atomic_t pending_stripe;/* how many stripes not flushed to raid */
	u64 seq;		/* seq number of the metablock */
	sector_t log_start;	/* where the io_unit starts */
	sector_t log_end;	/* where the io_unit ends */
	struct list_head log_sibling; /* log->running_ios */
	struct list_head stripe_list; /* stripes added to the io_unit */

	int state;
	bool need_split_bio;

	struct bio *split_bio;

	unsigned int has_flush:1;      /* include flush request */
	unsigned int has_fua:1;        /* include fua request */
	unsigned int has_null_flush:1; /* include empty flush request */
	/*
	 * io isn't sent yet, flush/fua request can only be submitted till it's
	 * the first IO in running_ios list
	 */
	unsigned int io_deferred:1;

	struct bio_list flush_barriers;   /* size == 0 flush bios */
};

/* r5l_io_unit state */
enum r5l_io_unit_state {
	IO_UNIT_RUNNING = 0,	/* accepting new IO */
	IO_UNIT_IO_START = 1,	/* io_unit bio start writing to log,
				 * don't accepting new bio */
	IO_UNIT_IO_END = 2,	/* io_unit bio finish writing to log */
	IO_UNIT_STRIPE_END = 3,	/* stripes data finished writing to raid */
};

struct r5l_policy {
	int (*init_log)(struct r5l_log *log, struct r5conf *conf);
	void (*exit_log)(struct r5l_log *log);
	int (*write_stripe)(struct r5l_log *log, struct stripe_head *sh);
	void (*write_stripe_run)(struct r5l_log *log);
	void (*flush_stripe_to_raid)(struct r5l_log *log);
	void (*stripe_write_finished)(struct r5l_io_unit *io);
	int (*handle_flush_request)(struct r5l_log *log, struct bio *bio);
	void (*quiesce)(struct r5l_log *log, int state);
};

extern int r5l_init_log(struct r5conf *conf, struct md_rdev *rdev, int policy_type);
extern void r5l_exit_log(struct r5l_log *log);
extern int r5l_write_stripe(struct r5l_log *log, struct stripe_head *sh);
extern void r5l_write_stripe_run(struct r5l_log *log);
extern void r5l_flush_stripe_to_raid(struct r5l_log *log);
extern void r5l_stripe_write_finished(struct stripe_head *sh);
extern int r5l_handle_flush_request(struct r5l_log *log, struct bio *bio);
extern void r5l_quiesce(struct r5l_log *log, int state);
extern bool r5l_log_disk_error(struct r5conf *conf);

extern void __r5l_set_io_unit_state(struct r5l_io_unit *io,
				    enum r5l_io_unit_state state);
extern void r5l_io_run_stripes(struct r5l_io_unit *io);
extern void r5l_run_no_mem_stripe(struct r5l_log *log);
extern void __r5l_flush_stripe_to_raid(struct r5l_log *log);

#endif /* _RAID5_CACHE_H */
