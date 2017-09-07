#ifndef BLK_MQ_SCHED_H
#define BLK_MQ_SCHED_H

/*
 * Scheduler helper functions.
 */
void blk_mq_sched_free_hctx_data(struct request_queue *q,
				 void (*exit)(struct blk_mq_hw_ctx *));
void blk_mq_sched_request_inserted(struct request *rq);
bool blk_mq_sched_try_merge(struct request_queue *q, struct bio *bio,
			    struct request **merged_request);
bool blk_mq_sched_try_insert_merge(struct request_queue *q, struct request *rq);

#endif
