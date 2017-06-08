#ifndef _MMC_CORE_BLOCK_H
#define _MMC_CORE_BLOCK_H

struct mmc_queue;
struct request;

void mmc_blk_issue_rq(struct mmc_queue *mq, struct request *req);
int mmc_blk_card_status_get(struct mmc_card *card, u64 *val);
int mmc_blk_get_ext_csd(struct mmc_card *card, u8 **ext_csd);

#endif
